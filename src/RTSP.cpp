#include "RTSP.hpp"
#include "H264TimingPatch.hpp"
#include "IMPBackchannel.hpp"
#include "BackchannelServerMediaSubsession.hpp"
#include "GroupsockHelper.hh"
#include <chrono>
#include <stdlib.h>
#include <stdio.h>
#include <iomanip>

#undef MODULE
#define MODULE "RTSP"

namespace
{
const char *bool_to_text(bool value)
{
    return value ? "true" : "false";
}

void publish_stream_status(const std::string &streamName,
                           const _stream &stream,
                           const std::string &url,
                           const char *state,
                           const std::string &error,
                           bool have_sps,
                           bool have_pps,
                           bool have_vps)
{
    RTSPStatus::StreamInfo streamInfo;
    streamInfo.format = stream.format;
    streamInfo.fps = stream.fps;
    streamInfo.width = stream.width;
    streamInfo.height = stream.height;
    streamInfo.endpoint = stream.rtsp_endpoint;
    streamInfo.url = url;
    streamInfo.bitrate = stream.bitrate;
    streamInfo.mode = stream.mode;
    streamInfo.enabled = stream.enabled;

    RTSPStatus::updateStreamStatus(streamName, streamInfo);
    RTSPStatus::writeCustomParameter(streamName, "state", state ? state : "unknown");
    RTSPStatus::writeCustomParameter(streamName, "error", error.empty() ? "none" : error);
    RTSPStatus::writeCustomParameter(streamName, "audio_enabled",
#if defined(AUDIO_SUPPORT)
                                     bool_to_text(cfg->audio.input_enabled && stream.audio_enabled)
#else
                                     "false"
#endif
    );
    RTSPStatus::writeCustomParameter(streamName, "sps", bool_to_text(have_sps));
    RTSPStatus::writeCustomParameter(streamName, "pps", bool_to_text(have_pps));
    RTSPStatus::writeCustomParameter(streamName, "vps", bool_to_text(have_vps));
}

bool load_cached_parameter_sets(video_stream &stream_state,
                                bool is_h265,
                                H264NALUnit &sps,
                                H264NALUnit &pps,
                                H264NALUnit *&vps,
                                bool &have_sps,
                                bool &have_pps,
                                bool &have_vps)
{
    std::lock_guard<std::mutex> lock(stream_state.parameterCache.mutex);
    if (!stream_state.parameterCache.have_sps || !stream_state.parameterCache.have_pps)
        return false;
    if (is_h265 && !stream_state.parameterCache.have_vps)
        return false;

    sps = stream_state.parameterCache.sps;
    pps = stream_state.parameterCache.pps;
    have_sps = true;
    have_pps = true;
    if (is_h265)
    {
        if (!vps)
            vps = new H264NALUnit;
        *vps = stream_state.parameterCache.vps;
        have_vps = true;
    }
    return true;
}

bool cache_has_bootstrap_ready(const video_stream &stream_state, bool is_h265)
{
    if (!stream_state.parameterCache.have_sps || !stream_state.parameterCache.have_pps)
        return false;
    if (is_h265 && !stream_state.parameterCache.have_vps)
        return false;
    return true;
}

bool wait_for_bootstrap(video_stream &stream_state,
                        int chnNr,
                        bool is_h265,
                        H264NALUnit &sps,
                        H264NALUnit &pps,
                        H264NALUnit *&vps,
                        bool &have_sps,
                        bool &have_pps,
                        bool &have_vps)
{
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    auto next_idr_retry = std::chrono::steady_clock::now();
    uint32_t wait_timeout_count = 0;

    while (!load_cached_parameter_sets(stream_state,
                                       is_h265,
                                       sps,
                                       pps,
                                       vps,
                                       have_sps,
                                       have_pps,
                                       have_vps))
    {
        std::unique_lock<std::mutex> lock(stream_state.parameterCache.mutex);
        if (!cache_has_bootstrap_ready(stream_state, is_h265))
        {
            stream_state.parameterCache.cv.wait_for(lock, 500ms);
        }
        lock.unlock();

        auto now = std::chrono::steady_clock::now();
        if (now >= next_idr_retry)
        {
            IMP_Encoder_RequestIDR(chnNr);
            next_idr_retry = now + 250ms;
        }
        if ((++wait_timeout_count % 4) == 0)
        {
            LOG_WARN("Still waiting for bootstrap parameter sets on stream " << chnNr
                     << ", retrying IDR");
        }
        if (now >= deadline)
        {
            return false;
        }
    }

    return true;
}
}

void RTSP::addSubsession(int chnNr, _stream &stream)
{
    LOG_DEBUG("identify stream " << chnNr);
    const std::string streamName = "stream" + std::to_string(chnNr);
    global_video[chnNr]->bootstrap_requested.store(true, std::memory_order_relaxed);
    publish_stream_status(streamName,
                          stream,
                          "",
                          "initializing",
                          "waiting_for_sps_pps",
                          false,
                          false,
                          false);

    H264NALUnit sps;
    H264NALUnit pps;
    H264NALUnit *vps = nullptr;
    bool have_pps = false;
    bool have_sps = false;
    bool have_vps = false;
    bool is_h265 = strcmp(stream.format, "H265") == 0 ? true : false;
    global_video[chnNr]->should_grab_frames.notify_one();

    if (!wait_for_bootstrap(*global_video[chnNr],
                            chnNr,
                            is_h265,
                            sps,
                            pps,
                            vps,
                            have_sps,
                            have_pps,
                            have_vps))
    {
        global_video[chnNr]->bootstrap_requested.store(false, std::memory_order_relaxed);
        LOG_ERROR("Timed out waiting for SPS/PPS on stream " << chnNr
                  << "; skipping this RTSP stream so the server can continue");
        publish_stream_status(streamName,
                              stream,
                              "",
                              "error",
                              "timeout_waiting_for_sps_pps",
                              have_sps,
                              have_pps,
                              have_vps);
        return;
    }

    global_video[chnNr]->bootstrap_requested.store(false, std::memory_order_relaxed);

    LOG_DEBUG("Got necessary NAL Units.");

    ServerMediaSession *sms = ServerMediaSession::createNew(
        *env, stream.rtsp_endpoint, stream.rtsp_info, cfg->rtsp.name);
    IMPServerMediaSubsession *sub = IMPServerMediaSubsession::createNew(
        *env, (is_h265 ? vps : nullptr), sps, pps, chnNr // Conditional VPS
    );

    sms->addSubsession(sub);

#if defined(AUDIO_SUPPORT)
    if (cfg->audio.input_enabled && stream.audio_enabled) {
        IMPAudioServerMediaSubsession *audioSub = IMPAudioServerMediaSubsession::createNew(*env, 0);
        sms->addSubsession(audioSub);
        LOG_INFO("Audio stream " << chnNr << " added to session");
    }

    if (cfg->audio.output_enabled && stream.audio_enabled)
    {
        #define ADD_BACKCHANNEL_SUBSESSION(EnumName, NameString, PayloadType, Frequency, MimeType) \
            { \
                BackchannelServerMediaSubsession* backchannelSub = \
                    BackchannelServerMediaSubsession::createNew(*env, IMPBackchannelFormat::EnumName); \
                sms->addSubsession(backchannelSub); \
                LOG_INFO("Backchannel stream " << NameString << " added to session"); \
            }

        X_FOREACH_BACKCHANNEL_FORMAT(ADD_BACKCHANNEL_SUBSESSION)
        #undef ADD_BACKCHANNEL_SUBSESSION
    }
#endif

    rtspServer->addServerMediaSession(sms);

    char *url = rtspServer->rtspURL(sms);
    LOG_INFO("stream " << chnNr << " available at: " << url);
    publish_stream_status(streamName,
                          stream,
                          url,
                          "ready",
                          "",
                          have_sps,
                          have_pps,
                          have_vps);

    delete[] url; // Free the URL string allocated by rtspURL()
}

void RTSP::start()
{
    // Initialize RTSP status interface
    if (!RTSPStatus::initialize()) {
        LOG_WARN("Failed to initialize RTSP status interface");
    }

    scheduler = BasicTaskScheduler::createNew();
    env = BasicUsageEnvironment::createNew(*scheduler);

    if (cfg->rtsp.auth_required)
    {
        UserAuthenticationDatabase *auth = new UserAuthenticationDatabase;
        auth->addUserRecord(
            cfg->rtsp.username,
            cfg->rtsp.password);
        rtspServer = RTSPServer::createNew(*env, cfg->rtsp.port, auth, cfg->rtsp.session_reclaim);
    }
    else
    {
        rtspServer = RTSPServer::createNew(*env, cfg->rtsp.port, nullptr, cfg->rtsp.session_reclaim);
    }
    if (rtspServer == NULL)
    {
        LOG_ERROR("Failed to create RTSP server: " << env->getResultMsg() << "\n");
        return;
    }
    OutPacketBuffer::maxSize = cfg->rtsp.out_buffer_size;

    // Let each RTP sink keep its own timestamp base. Sharing one numeric base
    // across different RTP clock domains (for example 90 kHz video and 16 kHz audio)
    // is not required for A/V sync and can confuse downstream demuxers.
    unsetenv("PRUDYNT_SHARED_TIMESTAMP_BASE");

    if (cfg->stream0.enabled)
    {
        addSubsession(0, cfg->stream0);
    }

    if (cfg->stream1.enabled)
    {
        addSubsession(1, cfg->stream1);
    }

    global_rtsp_thread_signal.store(0, std::memory_order_relaxed);
    env->taskScheduler().doEventLoop(&global_rtsp_thread_signal);
    global_rtsp_thread_signal.store(1, std::memory_order_relaxed);

    // Clean up VPS if it was allocated
    /*
    if (vps) {
        delete vps;
        vps = nullptr;
    }
    */

    LOG_DEBUG("Stop RTSP Server.");

    for (int chnNr = 0; chnNr < NUM_VIDEO_CHANNELS; ++chnNr)
    {
        if (global_video[chnNr] == nullptr)
            continue;

    }

    // Cleanup RTSP status interface
    RTSPStatus::cleanup();

    // Cleanup RTSP server and environment
    Medium::close(rtspServer);
    env->reclaim();
    delete scheduler;
}

void* RTSP::run(void* arg) {
    ((RTSP*)arg)->start();
    return nullptr;
}
