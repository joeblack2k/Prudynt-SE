#include "IPCServer.hpp"

#include "Config.hpp"
#include "Logger.hpp"
#include "WS.hpp"
#include "globals.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <stddef.h>

#define MODULE "IPC"

namespace
{
constexpr const char *kSockPath = "/run/prudynt/prudynt.sock";
constexpr const char *kRunDir = "/run/prudynt";

bool write_full(int fd, const void *data, size_t len)
{
    const char *ptr = static_cast<const char *>(data);
    while (len > 0)
    {
        ssize_t written = ::write(fd, ptr, len);
        if (written < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        ptr += written;
        len -= static_cast<size_t>(written);
    }
    return true;
}

bool starts_with(const std::string &value, const char *prefix)
{
    return value.rfind(prefix, 0) == 0;
}

socklen_t sockaddr_len(const sockaddr_un &sa)
{
    return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + std::strlen(sa.sun_path) + 1);
}

int parse_int_arg(const std::string &request, const char *key, int fallback)
{
    size_t pos = request.find(key);
    if (pos == std::string::npos)
        return fallback;

    pos += std::strlen(key);
    while (pos < request.size() && (request[pos] == ' ' || request[pos] == '='))
        ++pos;

    size_t start = pos;
    while (pos < request.size() && std::isdigit(static_cast<unsigned char>(request[pos])))
        ++pos;

    if (start == pos)
        return fallback;

    return std::stoi(request.substr(start, pos - start));
}
}

IPCServer::IPCServer()
{
}

IPCServer::~IPCServer()
{
    stop();
}

void IPCServer::start()
{
    bool expected = false;
    if (!running_->compare_exchange_strong(expected, true))
        return;

    th_ = std::thread(&IPCServer::server_loop, this);
}

void IPCServer::stop()
{
    bool expected = true;
    if (!running_->compare_exchange_strong(expected, false))
        return;

    {
        std::lock_guard<std::mutex> lock(client_mutex_);
        for (const int fd : client_fds_)
        {
            ::shutdown(fd, SHUT_RDWR);
        }
    }

    int wake_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (wake_fd >= 0)
    {
        sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        std::strncpy(sa.sun_path, kSockPath, sizeof(sa.sun_path) - 1);
        ::connect(wake_fd, reinterpret_cast<sockaddr *>(&sa), sockaddr_len(sa));
        ::close(wake_fd);
    }

    if (th_.joinable())
        th_.join();

    join_client_threads();
}

void IPCServer::server_loop()
{
    ::mkdir(kRunDir, 0775);
    ::unlink(kSockPath);

    int listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0)
    {
        LOG_ERROR("IPC socket() failed: " << strerror(errno));
        running_->store(false);
        return;
    }

    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, kSockPath, sizeof(sa.sun_path) - 1);
    if (::bind(listen_fd, reinterpret_cast<sockaddr *>(&sa), sockaddr_len(sa)) < 0)
    {
        LOG_ERROR("IPC bind(" << kSockPath << ") failed: " << strerror(errno));
        ::close(listen_fd);
        running_->store(false);
        return;
    }

    ::chmod(kSockPath, 0660);

    if (::listen(listen_fd, 8) < 0)
    {
        LOG_ERROR("IPC listen() failed: " << strerror(errno));
        ::close(listen_fd);
        ::unlink(kSockPath);
        running_->store(false);
        return;
    }

    LOG_INFO("IPC listening on " << kSockPath);

    const auto running = running_;
    while (running->load())
    {
        int client_fd = ::accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0)
        {
            if (errno == EINTR)
                continue;
            if (running->load())
                LOG_ERROR("IPC accept() failed: " << strerror(errno));
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(client_mutex_);
            client_fds_.push_back(client_fd);
            client_threads_.emplace_back([this, running](int fd) {
                IPCServer::handle_client(fd, running);
                ::close(fd);
                std::lock_guard<std::mutex> client_lock(client_mutex_);
                const auto it = std::find(client_fds_.begin(), client_fds_.end(), fd);
                if (it != client_fds_.end())
                    client_fds_.erase(it);
            }, client_fd);
        }
    }

    ::close(listen_fd);
    ::unlink(kSockPath);
}

void IPCServer::join_client_threads()
{
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(client_mutex_);
        threads.swap(client_threads_);
        client_fds_.clear();
    }

    for (auto &thread : threads)
    {
        if (thread.joinable())
            thread.join();
    }
}

int IPCServer::handle_client(int fd, const std::shared_ptr<std::atomic<bool>>& running)
{
    std::string request;
    char buffer[4096];
    ssize_t n = 0;
    while ((n = ::read(fd, buffer, sizeof(buffer))) > 0)
    {
        request.append(buffer, buffer + n);
        if (request.find('\n') != std::string::npos || static_cast<size_t>(n) < sizeof(buffer))
            break;
    }

    if (request.empty())
        return 0;

    if (starts_with(request, "JSON ") || request.front() == '{')
    {
        const std::string payload = request.front() == '{' ? request : request.substr(5);
        std::string response;
        if (WS::process_json_message(payload, response))
        {
            write_full(fd, response.data(), response.size());
            write_full(fd, "\n", 1);
        }
        else
        {
            static const char *err = "{\"error\":\"json_process_failed\"}\n";
            write_full(fd, err, std::strlen(err));
        }
        return 0;
    }

    if (starts_with(request, "SNAPSHOT"))
    {
        const int ch = parse_int_arg(request, "ch", 0);
        const int q = parse_int_arg(request, "q", -1);
        if (ch >= 0 && ch < NUM_JPEG_CHANNELS && global_jpeg[ch] && q >= 1 && q <= 100)
            global_jpeg[ch]->quality_override.store(q, std::memory_order_relaxed);
        std::vector<unsigned char> image;
        if (WS::get_snapshot_bytes(image, ch) && !image.empty())
        {
            char header[64];
            const int len = static_cast<int>(image.size());
            const int header_len = std::snprintf(header, sizeof(header), "OK %d\n", len);
            write_full(fd, header, static_cast<size_t>(header_len));
            write_full(fd, image.data(), image.size());
        }
        else
        {
            static const char *err = "ERR no_image\n";
            write_full(fd, err, std::strlen(err));
        }
        return 0;
    }

    if (starts_with(request, "MJPEG"))
    {
        const int ch = parse_int_arg(request, "ch", 0);
        int fps = parse_int_arg(request, "f", 5);
        const int q = parse_int_arg(request, "q", -1);
        if (fps < 1)
            fps = 1;
        if (fps > 30)
            fps = 30;
        if (ch >= 0 && ch < NUM_JPEG_CHANNELS && global_jpeg[ch] && q >= 1 && q <= 100)
            global_jpeg[ch]->quality_override.store(q, std::memory_order_relaxed);

        size_t boundary_pos = request.find("boundary=");
        std::string boundary = "prudyntmjpegboundary";
        if (boundary_pos != std::string::npos)
        {
            boundary_pos += std::strlen("boundary=");
            size_t boundary_end = boundary_pos;
            while (boundary_end < request.size() &&
                   !std::isspace(static_cast<unsigned char>(request[boundary_end])))
            {
                ++boundary_end;
            }
            boundary = request.substr(boundary_pos, boundary_end - boundary_pos);
            if (boundary.empty())
                boundary = "prudyntmjpegboundary";
        }

        while (running && running->load())
        {
            std::vector<unsigned char> image;
            if (!WS::get_snapshot_bytes(image, ch) || image.empty())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            char part_header[160];
            const int header_len = std::snprintf(
                part_header,
                sizeof(part_header),
                "--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",
                boundary.c_str(),
                image.size());
            if (!write_full(fd, part_header, static_cast<size_t>(header_len)))
                break;
            if (!write_full(fd, image.data(), image.size()))
                break;
            if (!write_full(fd, "\r\n", 2))
                break;

            std::this_thread::sleep_for(std::chrono::milliseconds(1000 / fps));
        }
        return 0;
    }

    if (starts_with(request, "EVENTS"))
    {
        while (running && running->load())
        {
            const long now = static_cast<long>(time(nullptr));
            std::string payload = "{\"ts\":";
            payload += std::to_string(now);
            payload += ",\"stats\":{\"stream0\":{\"fps\":";
            payload += std::to_string(cfg->stream0.stats.fps);
            payload += ",\"Bps\":";
            payload += std::to_string(cfg->stream0.stats.bps);
            payload += "},\"stream1\":{\"fps\":";
            payload += std::to_string(cfg->stream1.stats.fps);
            payload += ",\"Bps\":";
            payload += std::to_string(cfg->stream1.stats.bps);
            payload += "},\"stream2\":{\"fps\":";
            payload += std::to_string(cfg->stream2.stats.fps);
            payload += ",\"Bps\":";
            payload += std::to_string(cfg->stream2.stats.bps);
            payload += "}";
            if (cfg->stream3.enabled)
            {
                payload += ",\"stream3\":{\"fps\":";
                payload += std::to_string(cfg->stream3.stats.fps);
                payload += ",\"Bps\":";
                payload += std::to_string(cfg->stream3.stats.bps);
            }
            payload += "}}\n";
            if (!write_full(fd, payload.data(), payload.size()))
                break;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        return 0;
    }

    static const char *err = "ERR unknown_command\n";
    write_full(fd, err, std::strlen(err));
    return 0;
}
