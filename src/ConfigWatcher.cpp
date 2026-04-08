#include "ConfigWatcher.hpp"

#include "Config.hpp"
#include "Logger.hpp"
#include "globals.hpp"

#include <sys/inotify.h>
#include <sys/stat.h>
#include <chrono>
#include <string>
#include <thread>
#include <unistd.h>

#define MODULE "ConfigWatcher"

#define EVENT_SIZE (sizeof(struct inotify_event))
#define EVENT_BUF_LEN (1024 * (EVENT_SIZE + 16))

namespace
{
std::string parent_dir(const std::string &path)
{
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return ".";
    if (slash == 0)
        return "/";
    return path.substr(0, slash);
}

std::string base_name(const std::string &path)
{
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return path;
    return path.substr(slash + 1);
}

void request_full_restart()
{
    std::unique_lock<std::mutex> lock(mutex_main);
    global_restart_audio.store(true, std::memory_order_relaxed);
    global_restart_video.store(true, std::memory_order_relaxed);
    global_restart_rtsp.store(true, std::memory_order_relaxed);
    global_cv_worker_restart.notify_one();
}
}

ConfigWatcher::ConfigWatcher()
{
    LOG_DEBUG("ConfigWatcher created.");
}

ConfigWatcher::~ConfigWatcher()
{
    LOG_DEBUG("ConfigWatcher destroyed.");
}

void ConfigWatcher::run()
{
#ifdef __linux__ // Check if on Linux where inotify is expected
    watch_using_notify();
#else
    // Fallback to polling on non-Linux systems or if inotify fails
    watch_using_poll();
#endif
}

void *ConfigWatcher::thread_entry(void *arg)
{
    (void) arg; // Mark unused
    LOG_DEBUG("Starting config watch thread.");
    ConfigWatcher watcher;
    watcher.run();
    LOG_DEBUG("Exiting config watch thread.");
    return nullptr;
}

void ConfigWatcher::watch_using_notify()
{
    int inotifyFd = inotify_init();
    if (inotifyFd < 0)
    {
        LOG_ERROR("inotify_init() failed");
        return;
    }

    const std::string watchDir = parent_dir(cfg->filePath);
    const std::string watchName = base_name(cfg->filePath);
    int watchDescriptor = inotify_add_watch(inotifyFd,
                                            watchDir.c_str(),
                                            IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
    if (watchDescriptor == -1)
    {
        LOG_ERROR("inotify_add_watch() failed");
        close(inotifyFd);
        return;
    }

    char buffer[EVENT_BUF_LEN];

    LOG_DEBUG("Monitoring file for changes: " << cfg->filePath << " via directory " << watchDir);

    while (true)
    {
        int length = read(inotifyFd, buffer, EVENT_BUF_LEN);
        if (length < 0)
        {
            LOG_ERROR("Error reading file change notification.");
            break;
        }

        int i = 0;
        while (i < length)
        {
            struct inotify_event *event = (struct inotify_event *) &buffer[i];

            const bool same_entry = event->len > 0 && watchName == event->name;
            if (same_entry && (event->mask & (IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE)))
            {
                cfg->load();
                LOG_INFO("Config file changed, the config is reloaded from: " << cfg->filePath);
                request_full_restart();
            }

            i += EVENT_SIZE + event->len;
        }
    }

    inotify_rm_watch(inotifyFd, watchDescriptor);
    close(inotifyFd);
    return;
}

void ConfigWatcher::watch_using_poll()
{
    struct stat fileInfo;
    time_t lastModifiedTime = 0;

    while (true)
    {
        if (stat(cfg->filePath.c_str(), &fileInfo) == 0)
        {
            if (lastModifiedTime == 0)
            {
                lastModifiedTime = fileInfo.st_mtime;
            }
            else if (fileInfo.st_mtime != lastModifiedTime)
            {
                lastModifiedTime = fileInfo.st_mtime;
                cfg->load();
                LOG_INFO("Config file changed, the config is reloaded from: " << cfg->filePath);
                request_full_restart();
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}
