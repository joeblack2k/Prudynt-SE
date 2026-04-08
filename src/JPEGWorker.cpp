#include "JPEGWorker.hpp"

#include "Config.hpp"
#include "Logger.hpp"
#include "WorkerUtils.hpp"
#include "globals.hpp"

#include <chrono>
#include <fcntl.h>   // For O_RDWR, O_CREAT, O_TRUNC flags
#include <thread>
#include <unistd.h>  // For open(), close(), etc.

#define MODULE "JPEGWorker"

namespace
{
using namespace std::chrono;

microseconds frame_interval_for_fps(int fps)
{
    if (fps <= 0)
        return 0us;
    return microseconds(1000000 / fps);
}

bool wake_source_video_for_jpeg(int jpgChn)
{
    const int streamChn = global_jpeg[jpgChn]->streamChn;
    if (global_video[streamChn]->active)
        return true;

    std::unique_lock<std::mutex> lock_stream{mutex_main};
    global_video[streamChn]->run_for_jpeg = true;
    global_video[streamChn]->should_grab_frames.notify_one();
    lock_stream.unlock();

    const auto activation_timeout = milliseconds(cfg->general.imp_polling_timeout + 250);
    if (!global_video[streamChn]->is_activated.try_acquire_for(activation_timeout))
    {
        LOG_ERROR("Timed out waiting for video channel " << streamChn
                                                         << " to activate for JPEG capture");
        return false;
    }

    return true;
}

uint32_t stream_bytes(const IMPEncoderStream &stream)
{
    uint32_t total = 0;
    for (int i = 0; i < stream.packCount; ++i)
        total += stream.pack[i].length;
    return total;
}

void append_jpeg_pack_bytes(const IMPEncoderStream &stream,
                            const IMPEncoderPack &pack,
                            std::vector<unsigned char> &dst)
{
#if defined(PLATFORM_T31) || defined(PLATFORM_T40) || defined(PLATFORM_T41) || defined(PLATFORM_C100)
    uint32_t remSize = 0;
    if (!pack.length)
        return;
    remSize = stream.streamSize - pack.offset;
    const auto *data_ptr = reinterpret_cast<const unsigned char *>(
        reinterpret_cast<const char *>(stream.virAddr) + ((remSize < pack.length) ? 0 : pack.offset));
    const size_t data_len = (remSize < pack.length) ? remSize : pack.length;
    dst.insert(dst.end(), data_ptr, data_ptr + data_len);
    if (remSize && pack.length > remSize)
    {
        const auto *wrap_ptr = reinterpret_cast<const unsigned char *>(stream.virAddr);
        dst.insert(dst.end(), wrap_ptr, wrap_ptr + (pack.length - remSize));
    }
#elif defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) \
    || defined(PLATFORM_T23) || defined(PLATFORM_T30)
    const auto *data_ptr = reinterpret_cast<const unsigned char *>(pack.virAddr);
    dst.insert(dst.end(), data_ptr, data_ptr + pack.length);
#endif
}
}

JPEGWorker::JPEGWorker(int jpgChnIndex, int impEncoderChn)
    : jpgChn(jpgChnIndex)
    , impEncChn(impEncoderChn)
{
    LOG_DEBUG("JPEGWorker created for JPEG channel index " << jpgChn << " (IMP Encoder Channel "
                                                           << impEncChn << ")");
}

JPEGWorker::~JPEGWorker()
{
    LOG_DEBUG("JPEGWorker destroyed for JPEG channel index " << jpgChn);
}

int JPEGWorker::save_jpeg_stream(int fd, IMPEncoderStream *stream)
{
    int ret, i, nr_pack = stream->packCount;

    for (i = 0; i < nr_pack; i++)
    {
        void *data_ptr;
        size_t data_len;

#if defined(PLATFORM_T31) || defined(PLATFORM_T40) || defined(PLATFORM_T41) || defined(PLATFORM_C100)
        IMPEncoderPack *pack = &stream->pack[i];
        uint32_t remSize = 0; // Declare remSize here
        if (pack->length)
        {
            remSize = stream->streamSize - pack->offset;
            data_ptr = (void *) ((char *) stream->virAddr
                                 + ((remSize < pack->length) ? 0 : pack->offset));
            data_len = (remSize < pack->length) ? remSize : pack->length;
        }
        else
        {
            continue; // Skip empty packs
        }
#elif defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) \
    || defined(PLATFORM_T23) || defined(PLATFORM_T30)
        data_ptr = reinterpret_cast<void *>(stream->pack[i].virAddr);
        data_len = stream->pack[i].length;
#endif

        // Write data to file
        ret = write(fd, data_ptr, data_len);
        if (ret != static_cast<int>(data_len))
        {
            printf("Stream write error: %s\n", strerror(errno));
            return -1; // Return error on write failure
        }

#if defined(PLATFORM_T31) || defined(PLATFORM_T40) || defined(PLATFORM_T41) || defined(PLATFORM_C100)
        // Check the condition only under T31 platform, as remSize is used here
        if (remSize && pack->length > remSize)
        {
            ret = write(fd, (void *) ((char *) stream->virAddr), pack->length - remSize);
            if (ret != static_cast<int>(pack->length - remSize))
            {
                printf("Stream write error (remaining part): %s\n", strerror(errno));
                return -1;
            }
        }
#endif
    }

    return 0;
}

// Main processing loop, adapted from Worker::jpeg_grabber
void JPEGWorker::run()
{
    LOG_DEBUG("Start JPEG processing run loop for index " << jpgChn << " (IMP Encoder Channel "
                                                          << impEncChn << ")");

    int targetFps = global_jpeg[jpgChn]->stream->jpeg_idle_fps;
    auto next_capture_due = steady_clock::now();

    // Local stats counters
    uint32_t bps{0}; // Bytes per second
    uint32_t fps{0}; // frames per second

    // timestamp for stream stats calculation
    unsigned long long ms{0};

    // Initialize timestamp for stats calculation (ensure it's set before first use)
    WorkerUtils::getMonotonicTimeOfDay(&global_jpeg[jpgChn]->stream->stats.ts);
    global_jpeg[jpgChn]->stream->stats.ts.tv_sec -= 10;

    while (global_jpeg[jpgChn]->running)
    {
        const bool request_or_overrun = global_jpeg[jpgChn]->request_or_overrun();
        const int desiredFps = request_or_overrun ? global_jpeg[jpgChn]->stream->fps
                                                  : global_jpeg[jpgChn]->stream->jpeg_idle_fps;

        if (desiredFps > 0)
        {
            targetFps = desiredFps;

            if (!wake_source_video_for_jpeg(jpgChn))
            {
                std::this_thread::sleep_for(milliseconds(50));
                continue;
            }

            const auto now = steady_clock::now();
            if (now < next_capture_due)
            {
                std::this_thread::sleep_until(next_capture_due);
                continue;
            }

            if (IMP_Encoder_PollingStream(global_jpeg[jpgChn]->encChn,
                                          cfg->general.imp_polling_timeout)
                == 0)
            {
                IMPEncoderStream stream;
                if (IMP_Encoder_GetStream(global_jpeg[jpgChn]->encChn,
                                          &stream,
                                          GET_STREAM_BLOCKING)
                    == 0)
                {
                    fps++;
                    bps += stream_bytes(stream);
                    std::vector<unsigned char> snapshot_buf;
                    snapshot_buf.reserve(stream_bytes(stream));
                    for (int i = 0; i < stream.packCount; ++i)
                        append_jpeg_pack_bytes(stream, stream.pack[i], snapshot_buf);

                    {
                        std::unique_lock<std::mutex> lock_stream{mutex_main};
                        global_jpeg[jpgChn]->snapshot_buf = snapshot_buf;
                        ++global_jpeg[jpgChn]->frame_seq;
                    }

                    const char *tempPath = "/tmp/snapshot.tmp";
                    const char *finalPath = global_jpeg[jpgChn]->stream->jpeg_path;

                    int snap_fd = open(tempPath, O_RDWR | O_CREAT | O_TRUNC, 0666);
                    if (snap_fd >= 0)
                    {
                        const int save_ret = save_jpeg_stream(snap_fd, &stream);
                        close(snap_fd);

                        if (save_ret != 0)
                        {
                            LOG_ERROR("Failed to write a complete JPEG snapshot to " << tempPath);
                            std::remove(tempPath);
                        }
                        else if (rename(tempPath, finalPath) != 0)
                        {
                            LOG_ERROR("Failed to move JPEG snapshot from " << tempPath << " to "
                                                                           << finalPath);
                            std::remove(tempPath);
                        }
                        else
                        {
                            global_jpeg[jpgChn]->mark_image_produced();
                        }
                    }
                    else
                    {
                        LOG_ERROR("Failed to open JPEG snapshot for writing: " << tempPath);
                    }

                    IMP_Encoder_ReleaseStream(global_jpeg[jpgChn]->encChn,
                                              &stream);
                }

                ms = WorkerUtils::getMonotonicTimeDiffInMs(&global_jpeg[jpgChn]->stream->stats.ts);
                if (ms > 1000)
                {
                    global_jpeg[jpgChn]->stream->stats.fps = fps;
                    global_jpeg[jpgChn]->stream->stats.bps = bps;
                    fps = 0;
                    bps = 0;
                    WorkerUtils::getMonotonicTimeOfDay(&global_jpeg[jpgChn]->stream->stats.ts);
                }
            }

            next_capture_due = steady_clock::now() + frame_interval_for_fps(targetFps);
        }
        else
        {
            LOG_DDEBUG("JPEG LOCK" << " channel:" << jpgChn);

            global_jpeg[jpgChn]->stream->stats.bps = 0;
            global_jpeg[jpgChn]->stream->stats.fps = 0;
            targetFps = 0;

            std::unique_lock<std::mutex> lock_stream{mutex_main};
            global_jpeg[jpgChn]->active = false;
            global_video[global_jpeg[jpgChn]->streamChn]->run_for_jpeg = false;
            global_jpeg[jpgChn]->should_grab_frames.wait(lock_stream, [this]() {
                return !global_jpeg[jpgChn]->running || global_restart_video.load(std::memory_order_relaxed)
                    || global_jpeg[jpgChn]->request_or_overrun()
                    || global_jpeg[jpgChn]->stream->jpeg_idle_fps > 0;
            });

            if (!global_jpeg[jpgChn]->running)
                break;

            targetFps = global_jpeg[jpgChn]->stream->fps;
            next_capture_due = steady_clock::now();

            global_jpeg[jpgChn]->is_activated.release();
            global_jpeg[jpgChn]->active = true;

            LOG_DDEBUG("JPEG UNLOCK" << " channel:" << jpgChn);
        }
    }

    LOG_DEBUG("Exiting JPEG processing run loop for index " << jpgChn);
}

// Static entry point for creating the thread
void *JPEGWorker::thread_entry(void *arg)
{
    LOG_DEBUG("Start jpeg_grabber thread.");

    StartHelper *sh = static_cast<StartHelper *>(arg);
    int jpgChn = sh->encChn - 2;
    int ret;

    /* do not use the live config variable
    */
    global_jpeg[jpgChn]->streamChn = global_jpeg[jpgChn]->stream->jpeg_channel;

    auto *jpeg_stream_cfg = global_jpeg[jpgChn]->stream;

    if (global_jpeg[jpgChn]->streamChn == 0)
    {
        jpeg_stream_cfg->width = cfg->stream0.width;
        jpeg_stream_cfg->height = cfg->stream0.height;
    }
    else
    {
        jpeg_stream_cfg->width = cfg->stream1.width;
        jpeg_stream_cfg->height = cfg->stream1.height;
    }

    global_jpeg[jpgChn]->imp_encoder = IMPEncoder::createNew(global_jpeg[jpgChn]->stream,
                                                             sh->encChn,
                                                             global_jpeg[jpgChn]->streamChn,
                                                             global_jpeg[jpgChn]->streamChn,
                                                             (jpgChn == 0) ? "stream2" : "stream3");

    // inform main that initialization is complete
    sh->has_started.release();

    ret = IMP_Encoder_StartRecvPic(global_jpeg[jpgChn]->encChn);
    LOG_DEBUG_OR_ERROR(ret, "IMP_Encoder_StartRecvPic(" << global_jpeg[jpgChn]->encChn << ")");
    if (ret != 0)
        return 0;

    global_jpeg[jpgChn]->active = true;
    global_jpeg[jpgChn]->running = true;
    JPEGWorker worker(jpgChn, sh->encChn);
    worker.run();

    if (global_jpeg[jpgChn]->imp_encoder)
    {
        global_jpeg[jpgChn]->imp_encoder->deinit();

        delete global_jpeg[jpgChn]->imp_encoder;
        global_jpeg[jpgChn]->imp_encoder = nullptr;
    }

    return 0;
}
