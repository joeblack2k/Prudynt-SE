#include <chrono>
#include <thread>
#include <atomic>
#include <condition_variable>
#include "RTSP.hpp"
#include "Logger.hpp"
#include "Config.hpp"
#include "WS.hpp"
#include "version.hpp"
#include "ConfigWatcher.hpp"
#include "AudioWorker.hpp"
#include "BackchannelWorker.hpp"
#include "VideoWorker.hpp"
#include "JPEGWorker.hpp"
#include "globals.hpp"
#include "IMPSystem.hpp"
#include "Motion.hpp"
#include "WorkerUtils.hpp"
#include "IMPBackchannel.hpp"
#include "TimestampManager.hpp"
#include "IPCServer.hpp"
#include <imp/imp_isp.h>
#include <cstdio>
using namespace std::chrono;

std::mutex mutex_main;
std::condition_variable global_cv_worker_restart;

bool startup = true;
std::atomic<bool> global_restart{false};

std::atomic<bool> global_restart_rtsp{false};
std::atomic<bool> global_restart_video{false};
std::atomic<bool> global_restart_audio{false};

std::atomic<bool> global_osd_thread_signal{false};
std::atomic<bool> global_main_thread_signal{false};
std::atomic<bool> global_motion_thread_signal{false};
std::atomic<char> global_rtsp_thread_signal{1};
volatile char global_rtsp_thread_watch = 1;

std::shared_ptr<jpeg_stream> global_jpeg[NUM_JPEG_CHANNELS] = {nullptr};
std::shared_ptr<video_stream> global_video[NUM_VIDEO_CHANNELS] = {nullptr};
#if defined(AUDIO_SUPPORT)
std::shared_ptr<audio_stream> global_audio[NUM_AUDIO_CHANNELS] = {nullptr};
std::shared_ptr<backchannel_stream> global_backchannel = nullptr;
#endif

std::shared_ptr<CFG> cfg = std::make_shared<CFG>();

WS ws;
RTSP rtsp;
Motion motion;
IMPSystem *imp_system = nullptr;
IPCServer ipc_server;

namespace
{
const char *ops_mode_name(IMPISPTuningOpsMode mode)
{
    switch (mode)
    {
    case IMPISP_TUNING_OPS_MODE_DISABLE:
        return "disable";
    case IMPISP_TUNING_OPS_MODE_ENABLE:
        return "enable";
    default:
        return "unknown";
    }
}

void log_isp_frame_drop_attr(const IMPISPFrameDropAttr &attr, const char *prefix)
{
    for (int i = 0; i < 3; ++i)
    {
        char fmark_hex[16];
        std::snprintf(fmark_hex, sizeof(fmark_hex), "0x%x", attr.fdrop[i].fmark);
        LOG_INFO(prefix << " frame drop ch" << i
                 << ": mode=" << ops_mode_name(attr.fdrop[i].enable)
                 << ", lsize=" << static_cast<int>(attr.fdrop[i].lsize)
                 << ", fmark=" << fmark_hex);
    }
}

void normalize_isp_frame_drop()
{
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
    IMPISPFrameDropAttr attr{};
    int ret = IMP_ISP_GetFrameDrop(&attr);
    LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_GetFrameDrop(&attr)");
    if (ret != 0)
        return;

    log_isp_frame_drop_attr(attr, "Current");

    bool needs_reset = false;
    for (const auto &channel : attr.fdrop)
    {
        if (channel.enable != IMPISP_TUNING_OPS_MODE_DISABLE)
        {
            needs_reset = true;
            break;
        }
    }

    if (!needs_reset)
        return;

    IMPISPFrameDropAttr reset{};
    for (auto &channel : reset.fdrop)
    {
        channel.enable = IMPISP_TUNING_OPS_MODE_DISABLE;
        channel.lsize = 0;
        channel.fmark = 0x1;
    }

    ret = IMP_ISP_SetFrameDrop(&reset);
    LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_SetFrameDrop(&reset)");
    if (ret != 0)
        return;

    IMPISPFrameDropAttr verify{};
    ret = IMP_ISP_GetFrameDrop(&verify);
    LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_GetFrameDrop(&verify)");
    if (ret == 0)
        log_isp_frame_drop_attr(verify, "Normalized");
#endif
}
}

bool timesync_wait()
{
    // I don't really have a better way to do this than
    // a no-earlier-than time. The most common sync failure
    // is time() == 0
    int timeout = 0;
    while (time(NULL) < 1647489843)
    {
        std::this_thread::sleep_for(seconds(1));
        ++timeout;
        if (timeout == 60)
            return false;
    }
    return true;
}

void start_video(int encChn)
{
    StartHelper sh{encChn};
    int ret = pthread_create(&global_video[encChn]->thread, nullptr, VideoWorker::thread_entry, static_cast<void *>(&sh));
    LOG_DEBUG_OR_ERROR(ret, "create video["<< encChn << "] thread");

    // wait for initialization done
    sh.has_started.acquire();
}

static void init_video_framesource(int encChn)
{
    const auto &stream_state = global_video[encChn];
    if (!stream_state || !stream_state->stream->enabled || stream_state->imp_framesource)
        return;

    if (encChn == 1 && stream_state->encGrp == 0 && global_video[0] && global_video[0]->imp_framesource)
    {
        stream_state->imp_framesource = global_video[0]->imp_framesource;
        stream_state->owns_framesource = false;
        return;
    }

    LOG_INFO("Video stream " << encChn
             << " framesource binding: fs=" << stream_state->fsChn
             << " source=" << stream_state->sourceChn
             << " encGrp=" << stream_state->encGrp
             << " size=" << stream_state->stream->width << "x" << stream_state->stream->height
             << "@" << static_cast<int>(stream_state->stream->fps));

    stream_state->owns_framesource = true;
    stream_state->imp_framesource = IMPFramesource::createNew(stream_state->stream,
                                                              &cfg->sensor,
                                                              stream_state->fsChn,
                                                              stream_state->sourceChn);
}

static void init_video_encoder(int encChn)
{
    const auto &stream_state = global_video[encChn];
    if (!stream_state || !stream_state->stream->enabled || stream_state->imp_encoder)
        return;

    stream_state->imp_encoder = IMPEncoder::createNew(stream_state->stream,
                                                      encChn,
                                                      stream_state->encGrp,
                                                      stream_state->fsChn,
                                                      stream_state->name);
}

static void enable_video_framesource(int encChn)
{
    const auto &stream_state = global_video[encChn];
    if (!stream_state || !stream_state->stream->enabled || !stream_state->imp_framesource)
        return;

    stream_state->imp_framesource->enable();
}

static bool stream_depends_on(int encChn, int sourceChn)
{
    const auto &stream_state = global_video[encChn];
    if (stream_state && stream_state->encGrp == global_video[sourceChn]->encGrp && encChn != sourceChn)
        return true;

    return stream_state && stream_state->fsChn != stream_state->sourceChn
        && stream_state->sourceChn == sourceChn;
}

int main(int argc, const char *argv[])
{
    LOG_INFO("PRUDYNT-T Next-Gen Video Daemon: " << FULL_VERSION_STRING);

    pthread_t cw_thread;
    pthread_t ws_thread;
    pthread_t osd_thread;
    pthread_t rtsp_thread;
    pthread_t motion_thread;
    pthread_t backchannel_thread;

    if (Logger::init(cfg->general.loglevel))
    {
        LOG_ERROR("Logger initialization failed.");
        return 1;
    }
    LOG_INFO("Starting Prudynt-SE Video Server.");

    if (!timesync_wait())
    {
        LOG_ERROR("Time is not synchronized.");
        return 1;
    }

    if (!imp_system)
    {
        imp_system = IMPSystem::createNew();
    }

    // Initialize the global timestamp manager after IMP system is ready
    // This ensures all audio/video timestamps use a single monotonic timeline
    if (TimestampManager::getInstance().initialize() != 0) {
        LOG_ERROR("Failed to initialize TimestampManager");
        return 1;
    }

    normalize_isp_frame_drop();

    const auto stream0Binding = framesource_binding_for_video(0, cfg->sensor, cfg->stream0);
    const auto stream1Binding = framesource_binding_for_video(1, cfg->sensor, cfg->stream1);
    global_video[0] = std::make_shared<video_stream>(0,
                                                     encoder_group_for_video(0, cfg->sensor, cfg->stream0),
                                                     stream0Binding.fsChn,
                                                     stream0Binding.sourceChn,
                                                     &cfg->stream0,
                                                     "stream0");
    global_video[1] = std::make_shared<video_stream>(1,
                                                     encoder_group_for_video(1, cfg->sensor, cfg->stream1),
                                                     stream1Binding.fsChn,
                                                     stream1Binding.sourceChn,
                                                     &cfg->stream1,
                                                     "stream1");
    global_jpeg[0] = std::make_shared<jpeg_stream>(2, &cfg->stream2);
    global_jpeg[1] = std::make_shared<jpeg_stream>(3, &cfg->stream3);

#if defined(AUDIO_SUPPORT)
    global_audio[0] = std::make_shared<audio_stream>(1, 0, 0);
    global_backchannel = std::make_shared<backchannel_stream>();
#endif

    pthread_create(&cw_thread, nullptr, ConfigWatcher::thread_entry, nullptr);
    pthread_create(&ws_thread, nullptr, WS::run, &ws);
    ipc_server.start();

    while (true)
    {
        global_restart.store(true, std::memory_order_relaxed);
#if defined(AUDIO_SUPPORT)
        if (cfg->audio.output_enabled && (global_restart_audio.load(std::memory_order_relaxed) || startup))
        {
             int ret = pthread_create(&backchannel_thread, nullptr, BackchannelWorker::thread_entry, NULL);
             LOG_DEBUG_OR_ERROR(ret, "create backchannel thread");
        }

        if (cfg->audio.input_enabled && (global_restart_audio.load(std::memory_order_relaxed) || startup))
        {
            StartHelper sh{0};
            int ret = pthread_create(&global_audio[0]->thread, nullptr, AudioWorker::thread_entry, static_cast<void *>(&sh));
            LOG_DEBUG_OR_ERROR(ret, "create audio thread");
            // wait for initialization done
            sh.has_started.acquire();
        }
#endif
        if (global_restart_video.load(std::memory_order_relaxed) || startup)
        {
            const bool stream1_depends_on_stream0 = cfg->stream1.enabled && stream_depends_on(1, 0);
            const bool stream1_can_start = cfg->stream1.enabled && (!stream1_depends_on_stream0 || cfg->stream0.enabled);

            if (cfg->stream1.enabled && !stream1_can_start)
                LOG_ERROR("stream1 depends on stream0 framesource, but stream0 is disabled");

            if (cfg->stream0.enabled)
                init_video_framesource(0);
            if (stream1_can_start)
                init_video_framesource(1);

            if (cfg->stream0.enabled)
                init_video_encoder(0);
            if (stream1_can_start)
                init_video_encoder(1);

            if (cfg->stream0.enabled)
                enable_video_framesource(0);
            if (stream1_can_start)
                enable_video_framesource(1);

            if (cfg->stream0.enabled)
                start_video(0);
            if (stream1_can_start)
                start_video(1);

            if (cfg->stream2.enabled)
            {
                StartHelper sh{2};
                int ret = pthread_create(&global_jpeg[0]->thread, nullptr, JPEGWorker::thread_entry, static_cast<void *>(&sh));
                LOG_DEBUG_OR_ERROR(ret, "create jpeg thread");
                // wait for initialization done
                sh.has_started.acquire();
            }

            if (cfg->stream3.enabled)
            {
                StartHelper sh{3};
                int ret = pthread_create(&global_jpeg[1]->thread, nullptr, JPEGWorker::thread_entry, static_cast<void *>(&sh));
                LOG_DEBUG_OR_ERROR(ret, "create jpeg thread 2");
                sh.has_started.acquire();
            }

            if (cfg->stream0.osd.enabled || cfg->stream1.osd.enabled)
            {
                int ret = pthread_create(&osd_thread, nullptr, OSD::thread_entry, NULL);
                LOG_DEBUG_OR_ERROR(ret, "create osd thread");
            }

            if (cfg->motion.enabled)
            {
                int ret = pthread_create(&motion_thread, nullptr, Motion::run, &motion);
                LOG_DEBUG_OR_ERROR(ret, "create motion thread");
            }
        }

        // start rtsp server
        if (global_rtsp_thread_signal.load(std::memory_order_relaxed) != 0
            && (global_restart_rtsp.load(std::memory_order_relaxed) || startup))
        {
            int ret = pthread_create(&rtsp_thread, nullptr, RTSP::run, &rtsp);
            LOG_DEBUG_OR_ERROR(ret, "create rtsp thread");
        }

        /* we should wait a short period to ensure all services are up
         * and running, additionally we add the timespan which is configured as
         * OSD startup delay.
         */
        usleep(250000 + (cfg->stream0.osd.start_delay * 1000) + cfg->stream1.osd.start_delay * 1000);

        LOG_DEBUG("main thread is going to sleep");
        std::unique_lock lck(mutex_main);

        startup = false;
        global_restart.store(false, std::memory_order_relaxed);
        global_restart_video.store(false, std::memory_order_relaxed);
        global_restart_audio.store(false, std::memory_order_relaxed);
        global_restart_rtsp.store(false, std::memory_order_relaxed);

        while (!global_restart_rtsp.load(std::memory_order_relaxed)
               && !global_restart_video.load(std::memory_order_relaxed)
               && !global_restart_audio.load(std::memory_order_relaxed))
            global_cv_worker_restart.wait(lck);
        lck.unlock();

        global_restart.store(true, std::memory_order_relaxed);

        if (global_restart_rtsp.load(std::memory_order_relaxed))
        {
            // stop rtsp thread
            if (global_rtsp_thread_signal.load(std::memory_order_relaxed) == 0)
            {
                global_rtsp_thread_signal.store(1, std::memory_order_relaxed);
                global_rtsp_thread_watch = 1;
                int ret = pthread_join(rtsp_thread, NULL);
                LOG_DEBUG_OR_ERROR(ret, "join rtsp thread");
            }
        }

        // stop audio
        if (global_audio[0]->imp_audio && global_restart_audio.load(std::memory_order_relaxed))
        {
            global_audio[0]->running = false;
            global_audio[0]->should_grab_frames.notify_one();
            int ret = pthread_join(global_audio[0]->thread, NULL);
            LOG_DEBUG_OR_ERROR(ret, "join audio thread");
        }

        // stop backchannel
        if (global_backchannel->imp_backchannel && global_restart_audio.load(std::memory_order_relaxed))
        {
             global_backchannel->running = false;
             global_backchannel->should_grab_frames.notify_one();
             int ret = pthread_join(backchannel_thread, NULL);
             LOG_DEBUG_OR_ERROR(ret, "join backchannel thread");
        }

        if (global_restart_video.load(std::memory_order_relaxed))
        {
            // stop motion thread
            if (global_motion_thread_signal.load(std::memory_order_relaxed))
            {
                global_motion_thread_signal.store(false, std::memory_order_relaxed);
                int ret = pthread_join(motion_thread, NULL);
                LOG_DEBUG_OR_ERROR(ret, "join motion thread");
            }

            // stop osd thread
            if (global_osd_thread_signal.load(std::memory_order_relaxed))
            {
                global_osd_thread_signal.store(false, std::memory_order_relaxed);
                int ret = pthread_join(osd_thread, NULL);
                LOG_DEBUG_OR_ERROR(ret, "join osd thread");
            }

            // stop jpeg
            if (global_jpeg[0]->imp_encoder)
            {
                global_jpeg[0]->running = false;
                global_jpeg[0]->should_grab_frames.notify_one();
                int ret = pthread_join(global_jpeg[0]->thread, NULL);
                LOG_DEBUG_OR_ERROR(ret, "join jpeg thread");
            }

            if (global_jpeg[1]->imp_encoder)
            {
                global_jpeg[1]->running = false;
                global_jpeg[1]->should_grab_frames.notify_one();
                int ret = pthread_join(global_jpeg[1]->thread, NULL);
                LOG_DEBUG_OR_ERROR(ret, "join jpeg thread 2");
            }

            // stop stream1
            if (global_video[1]->imp_encoder)
            {
                global_video[1]->running = false;
                global_video[1]->should_grab_frames.notify_one();
                int ret = pthread_join(global_video[1]->thread, NULL);
                LOG_DEBUG_OR_ERROR(ret, "join stream1 thread");
            }

            // stop stream0
            if (global_video[0]->imp_encoder)
            {
                global_video[0]->running = false;
                global_video[0]->should_grab_frames.notify_one();
                int ret = pthread_join(global_video[0]->thread, NULL);
                LOG_DEBUG_OR_ERROR(ret, "join stream0 thread");
            }
        }
    }

    return 0;
}
