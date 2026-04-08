#include "VideoWorker.hpp"

#include "Config.hpp"
#include "H264TimingPatch.hpp"
#include "IMPEncoder.hpp"
#include "IMPFramesource.hpp"
#include "Logger.hpp"
#include "RTSPStatus.hpp"
#include "WorkerUtils.hpp"
#include "TimestampManager.hpp"
#include "globals.hpp"

#include <chrono>
#include <cstring>
#include <sys/time.h>

#undef MODULE
#define MODULE "VideoWorker"

namespace
{
uint64_t monotonic_now_us()
{
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

struct timeval timeval_from_us(uint64_t ts_us)
{
    struct timeval tv;
    tv.tv_sec = ts_us / 1000000ULL;
    tv.tv_usec = ts_us % 1000000ULL;
    return tv;
}

bool parameter_cache_ready(video_stream &stream_state)
{
    std::lock_guard<std::mutex> lock(stream_state.parameterCache.mutex);
    if (!stream_state.parameterCache.have_sps || !stream_state.parameterCache.have_pps)
        return false;
    if (strcmp(stream_state.stream->format, "H265") == 0 && !stream_state.parameterCache.have_vps)
        return false;
    return true;
}

bool is_idr_or_parameter_set(const IMPEncoderPack &pack)
{
#if defined(PLATFORM_T31) || defined(PLATFORM_T40) || defined(PLATFORM_T41) || defined(PLATFORM_C100)
    return pack.nalType.h264NalType == 5 || pack.nalType.h264NalType == 7
        || pack.nalType.h264NalType == 8 || pack.nalType.h265NalType == 19
        || pack.nalType.h265NalType == 20 || pack.nalType.h265NalType == 32
        || pack.nalType.h265NalType == 33 || pack.nalType.h265NalType == 34;
#elif defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) \
    || defined(PLATFORM_T23)
    return pack.dataType.h264Type == 5 || pack.dataType.h264Type == 7
        || pack.dataType.h264Type == 8;
#elif defined(PLATFORM_T30)
    return pack.dataType.h264Type == 5 || pack.dataType.h264Type == 7
        || pack.dataType.h264Type == 8 || pack.dataType.h265Type == 19
        || pack.dataType.h265Type == 20 || pack.dataType.h265Type == 32;
#else
    return false;
#endif
}

bool is_keyframe_nal(const IMPEncoderPack &pack)
{
#if defined(PLATFORM_T31) || defined(PLATFORM_T40) || defined(PLATFORM_T41) || defined(PLATFORM_C100)
    return pack.nalType.h264NalType == 5 || pack.nalType.h265NalType == 19
        || pack.nalType.h265NalType == 20;
#elif defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) \
    || defined(PLATFORM_T23)
    return pack.dataType.h264Type == 5;
#elif defined(PLATFORM_T30)
    return pack.dataType.h264Type == 5 || pack.dataType.h265Type == 19
        || pack.dataType.h265Type == 20;
#else
    return false;
#endif
}

uint8_t h264_pack_nal_type(const IMPEncoderPack &pack)
{
#if defined(PLATFORM_T31) || defined(PLATFORM_T40) || defined(PLATFORM_T41) || defined(PLATFORM_C100)
    return pack.nalType.h264NalType;
#elif defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) \
    || defined(PLATFORM_T23)
    return pack.dataType.h264Type;
#elif defined(PLATFORM_T30)
    return pack.dataType.h264Type;
#else
    return 0;
#endif
}

uint8_t h265_pack_nal_type(const IMPEncoderPack &pack)
{
#if defined(PLATFORM_T31) || defined(PLATFORM_T40) || defined(PLATFORM_T41) || defined(PLATFORM_C100)
    return pack.nalType.h265NalType;
#elif defined(PLATFORM_T30)
    return pack.dataType.h265Type;
#else
    return 0;
#endif
}

uint8_t h264_payload_nal_type(const std::vector<uint8_t> &payload)
{
    if (payload.empty())
        return 0;
    return payload[0] & 0x1f;
}

uint8_t h265_payload_nal_type(const std::vector<uint8_t> &payload)
{
    if (payload.empty())
        return 0;
    return (payload[0] >> 1) & 0x3f;
}

void copy_nalu_payload(const IMPEncoderStream &stream, const IMPEncoderPack &pack, std::vector<uint8_t> &dst)
{
    const uint8_t *start = nullptr;
#if defined(PLATFORM_T31) || defined(PLATFORM_T40) || defined(PLATFORM_T41) || defined(PLATFORM_C100)
    if (pack.length <= 4)
    {
        dst.clear();
        return;
    }

    uint32_t rem_size = stream.streamSize - pack.offset;
    size_t skip = 4;
    dst.clear();
    dst.reserve(static_cast<size_t>(pack.length - 4));

    auto append_payload = [&](const uint8_t *ptr, size_t len) {
        if (len == 0)
            return;
        if (skip >= len)
        {
            skip -= len;
            return;
        }
        ptr += skip;
        len -= skip;
        skip = 0;
        dst.insert(dst.end(), ptr, ptr + len);
    };

    if (rem_size < pack.length)
    {
        const auto *tail_ptr = reinterpret_cast<const uint8_t *>(stream.virAddr) + pack.offset;
        append_payload(tail_ptr, rem_size);

        const auto *wrap_ptr = reinterpret_cast<const uint8_t *>(stream.virAddr);
        append_payload(wrap_ptr, pack.length - rem_size);
    }
    else
    {
        start = reinterpret_cast<const uint8_t *>(stream.virAddr) + pack.offset;
        append_payload(start, pack.length);
    }
    return;
#elif defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) \
    || defined(PLATFORM_T23) || defined(PLATFORM_T30)
    start = reinterpret_cast<const uint8_t *>(pack.virAddr);
#endif

    const size_t payload_size = pack.length > 4 ? static_cast<size_t>(pack.length - 4) : 0;
    dst.resize(payload_size);
    if (payload_size)
        std::memcpy(dst.data(), start + 4, payload_size);
}

struct timeval frame_timestamp_to_timeval(video_stream &stream_state, uint64_t raw_ts)
{
    const uint64_t expected_frame_us =
        (stream_state.stream && stream_state.stream->fps > 0)
        ? (1000000ULL / static_cast<uint64_t>(stream_state.stream->fps))
        : 40000ULL;

    uint64_t step_us = 0;
    const uint64_t last_raw =
        stream_state.last_frame_timestamp_raw.load(std::memory_order_relaxed);
    const uint64_t last_ts =
        stream_state.last_timestamp_us.load(std::memory_order_relaxed);

    if (raw_ts > 0 && last_raw != 0 && raw_ts == last_raw && last_ts != 0)
        return timeval_from_us(last_ts);

    if (raw_ts > 0)
    {
        uint64_t origin_raw = stream_state.timestamp_origin_raw.load(std::memory_order_relaxed);
        if (origin_raw == 0)
        {
            uint64_t expected = 0;
            if (stream_state.timestamp_origin_raw.compare_exchange_strong(
                    expected, raw_ts, std::memory_order_relaxed))
            {
                origin_raw = raw_ts;
            }
            else
            {
                origin_raw = expected;
            }
        }

        if (last_raw != 0 && raw_ts > last_raw)
        {
            step_us = raw_ts - last_raw;

            // Preserve the encoder cadence. For the re-homed native substream on
            // this camera, forcing a 30 fps presentation step while the encoder is
            // actually delivering ~15 fps makes downstream clients play at ~2x.
            // Only clamp obviously broken jumps and otherwise trust the hardware
            // timestamps so playback speed stays correct.
            if (step_us > (expected_frame_us * 10))
            {
                step_us = expected_frame_us;
            }
        }

        stream_state.last_frame_timestamp_raw.store(raw_ts, std::memory_order_relaxed);
    }

    uint64_t presentation_origin_us =
        stream_state.presentation_origin_us.load(std::memory_order_relaxed);
    if (presentation_origin_us == 0)
    {
        const uint64_t fallback_origin_us = expected_frame_us;
        uint64_t expected = 0;
        if (stream_state.presentation_origin_us.compare_exchange_strong(
                expected, fallback_origin_us, std::memory_order_relaxed))
        {
            presentation_origin_us = fallback_origin_us;
        }
        else
        {
            presentation_origin_us = expected;
        }
    }

    uint64_t ts_us = last_ts;
    if (ts_us == 0)
    {
        ts_us = presentation_origin_us;
    }
    else
    {
        if (step_us == 0)
            step_us = expected_frame_us;
        ts_us += step_us;
    }

    stream_state.last_timestamp_us.store(ts_us, std::memory_order_relaxed);
    return timeval_from_us(ts_us);
}

void request_idr(video_stream &stream_state)
{
    IMP_Encoder_RequestIDR(stream_state.encChn);
    stream_state.last_idr_request_us.store(monotonic_now_us(), std::memory_order_relaxed);
}

void update_parameter_cache(video_stream &stream_state, const H264NALUnit &nalu)
{
    auto &cache = stream_state.parameterCache;
    std::lock_guard<std::mutex> lock(cache.mutex);

    if (nalu.is_parameter_set)
    {
        if (strcmp(stream_state.stream->format, "H264") == 0)
        {
            if (nalu.nal_type == 7)
            {
                cache.sps = nalu;
                cache.have_sps = true;
                if (nalu.data.size() >= 4)
                {
                    cache.profile_idc = nalu.data[1];
                    cache.level_idc = nalu.data[3];
                }
            }
            else if (nalu.nal_type == 8)
            {
                cache.pps = nalu;
                cache.have_pps = true;
            }
        }
        else if (strcmp(stream_state.stream->format, "H265") == 0)
        {
            if (nalu.nal_type == 32)
            {
                cache.vps = nalu;
                cache.have_vps = true;
            }
            else if (nalu.nal_type == 33)
            {
                cache.sps = nalu;
                cache.have_sps = true;
            }
            else if (nalu.nal_type == 34)
            {
                cache.pps = nalu;
                cache.have_pps = true;
            }
        }
    }

    if (nalu.is_sync)
    {
        cache.latest_sync = nalu;
        cache.have_latest_sync = true;
        cache.last_idr_us = monotonic_now_us();
    }

    cache.cv.notify_all();
}

void publish_video_runtime_status(video_stream &stream_state)
{
    const std::string streamName = stream_state.name;
    RTSPStatus::writeCustomParameter(streamName, "gop",
                                     std::to_string(stream_state.stream->gop));
    RTSPStatus::writeCustomParameter(streamName, "fs_channel",
                                     std::to_string(stream_state.fsChn));
    RTSPStatus::writeCustomParameter(streamName, "source_channel",
                                     std::to_string(stream_state.sourceChn));
    RTSPStatus::writeCustomParameter(streamName, "ring_depth",
                                     std::to_string(stream_state.streamCore->depth()));
    RTSPStatus::writeCustomParameter(streamName, "client_count",
                                     std::to_string(stream_state.streamCore->subscriberCount()));
    RTSPStatus::writeCustomParameter(streamName, "drop_count",
                                     std::to_string(stream_state.streamCore->producerDropCount()));

    uint64_t last_idr_age_ms = 0;
    uint8_t profile_idc = 0;
    uint8_t level_idc = 0;
    {
        std::lock_guard<std::mutex> lock(stream_state.parameterCache.mutex);
        if (stream_state.parameterCache.last_idr_us != 0)
        {
            last_idr_age_ms =
                (monotonic_now_us() - stream_state.parameterCache.last_idr_us) / 1000ULL;
        }
        profile_idc = stream_state.parameterCache.profile_idc;
        level_idc = stream_state.parameterCache.level_idc;
    }

    RTSPStatus::writeCustomParameter(streamName, "last_idr_age_ms",
                                     std::to_string(last_idr_age_ms));
    RTSPStatus::writeCustomParameter(streamName, "profile",
                                     std::to_string(profile_idc));
    RTSPStatus::writeCustomParameter(streamName, "level",
                                     std::to_string(level_idc));
    RTSPStatus::writeCustomParameter(
        streamName,
        "observed_fps",
        std::to_string(stream_state.observed_frame_rate.load(std::memory_order_relaxed)));
    RTSPStatus::writeCustomParameter(
        streamName,
        "producer_wall_delta_ms",
        std::to_string(stream_state.last_encoder_wall_delta_us.load(std::memory_order_relaxed) / 1000ULL));
    RTSPStatus::writeCustomParameter(
        streamName,
        "producer_raw_delta_ms",
        std::to_string(stream_state.last_encoder_raw_delta_us.load(std::memory_order_relaxed) / 1000ULL));
    RTSPStatus::writeCustomParameter(
        streamName,
        "producer_state",
        stream_state.producer_has_keyframe ? "streaming" : "waiting_for_keyframe");
}
}

VideoWorker::VideoWorker(int chn)
    : encChn(chn)
{
    LOG_DEBUG("VideoWorker created for channel " << encChn);
}

VideoWorker::~VideoWorker()
{
    LOG_DEBUG("VideoWorker destroyed for channel " << encChn);
}

void VideoWorker::run()
{
    LOG_DEBUG("Start video processing run loop for stream " << encChn);
    using namespace std::chrono_literals;

    uint32_t bps = 0;
    uint32_t fps = 0;
    uint32_t observed_frames = 0;
    uint32_t error_count = 0; // Keep track of polling errors
    unsigned long long ms = 0;
    bool run_for_jpeg = false;
    bool had_subscribers = false;
    int bootstrap_log_budget = 24;
    auto next_idr_retry = std::chrono::steady_clock::now();

    while (global_video[encChn]->running)
    {
        /* bool helper to check if this is the active jpeg channel and a jpeg is requested while
         * the channel is inactive
         */
        bool jpeg_wants_frames = false;
        for (int j = 0; j < NUM_JPEG_CHANNELS; ++j)
        {
            auto jpeg_stream_state = global_jpeg[j];
            if (jpeg_stream_state && encChn == jpeg_stream_state->streamChn)
            {
                jpeg_wants_frames = true;
                break;
            }
        }
        run_for_jpeg = jpeg_wants_frames && global_video[encChn]->run_for_jpeg;
        const bool has_subscribers = global_video[encChn]->streamCore->subscriberCount() > 0;
        const bool bootstrap_requested =
            global_video[encChn]->bootstrap_requested.load(std::memory_order_relaxed);
        const bool keep_warm =
            global_video[encChn]->fsChn != global_video[encChn]->sourceChn
            && !parameter_cache_ready(*global_video[encChn]);
        const bool should_capture = true;
        const bool needs_video_bootstrap = has_subscribers || bootstrap_requested || keep_warm;

        if (has_subscribers && !had_subscribers)
        {
            IMPEncoder::flush(encChn);
            global_video[encChn]->producer_has_keyframe = false;
            global_video[encChn]->last_timestamp_us.store(0, std::memory_order_relaxed);
            global_video[encChn]->timestamp_origin_raw.store(0, std::memory_order_relaxed);
            global_video[encChn]->last_frame_timestamp_raw.store(0, std::memory_order_relaxed);
            global_video[encChn]->presentation_origin_us.store(0, std::memory_order_relaxed);
            next_idr_retry = std::chrono::steady_clock::now() + 250ms;
        }
        had_subscribers = has_subscribers;

        /* now we need to verify that
         * 1. a client is connected (hasDataCallback)
         * 2. a jpeg is requested
         */
        if (should_capture)
        {
            if (IMP_Encoder_PollingStream(encChn, cfg->general.imp_polling_timeout) == 0)
            {
                IMPEncoderStream stream;
                if (IMP_Encoder_GetStream(encChn, &stream, GET_STREAM_BLOCKING) != 0)
                {
                    LOG_ERROR("IMP_Encoder_GetStream(" << encChn << ") failed");
                    error_count++;
                    continue;
                }
                bool wrote_video_data = false;
                bool waiting_for_keyframe =
                    needs_video_bootstrap && !global_video[encChn]->producer_has_keyframe;
                const uint64_t encoder_wall_now_us = monotonic_now_us();
                const uint64_t last_encoder_wall_us =
                    global_video[encChn]->last_encoder_wall_us.load(std::memory_order_relaxed);
                if (last_encoder_wall_us != 0 && encoder_wall_now_us > last_encoder_wall_us)
                {
                    global_video[encChn]->last_encoder_wall_delta_us.store(
                        encoder_wall_now_us - last_encoder_wall_us,
                        std::memory_order_relaxed);
                }
                global_video[encChn]->last_encoder_wall_us.store(
                    encoder_wall_now_us,
                    std::memory_order_relaxed);
                uint64_t last_batch_raw_timestamp = 0;

                for (uint32_t i = 0; i < stream.packCount; ++i)
                {
                    const auto &pack = stream.pack[i];
                    bps += pack.length;
                    const uint64_t pack_raw_timestamp =
                        pack.timestamp > 0 ? static_cast<uint64_t>(pack.timestamp) : 0;
                    const bool new_frame_in_batch =
                        i == 0 || pack_raw_timestamp == 0 || pack_raw_timestamp != last_batch_raw_timestamp;
                    if (new_frame_in_batch)
                    {
                        ++fps;
                        ++observed_frames;
                        if (pack_raw_timestamp != 0)
                            last_batch_raw_timestamp = pack_raw_timestamp;
                    }

                    const uint64_t last_raw_timestamp =
                        global_video[encChn]->last_frame_timestamp_raw.load(std::memory_order_relaxed);
                    if (pack_raw_timestamp > 0 && last_raw_timestamp > 0
                        && pack_raw_timestamp > last_raw_timestamp)
                    {
                        global_video[encChn]->last_encoder_raw_delta_us.store(
                            pack_raw_timestamp - last_raw_timestamp,
                            std::memory_order_relaxed);
                    }
                    const struct timeval frame_time =
                        frame_timestamp_to_timeval(*global_video[encChn], pack_raw_timestamp);

                    if (should_capture || needs_video_bootstrap)
                    {
                        std::vector<uint8_t> payload;
                        copy_nalu_payload(stream, pack, payload);
                        if (payload.empty())
                            continue;

                        const bool is_h264 =
                            strcmp(global_video[encChn]->stream->format, "H264") == 0;
                        const uint8_t payload_nal_type = is_h264
                            ? h264_payload_nal_type(payload)
                            : h265_payload_nal_type(payload);
                        const uint8_t pack_nal_type = is_h264
                            ? h264_pack_nal_type(pack)
                            : h265_pack_nal_type(pack);
                        const uint8_t nal_type =
                            payload_nal_type != 0 ? payload_nal_type : pack_nal_type;

                        const bool is_parameter_set = is_h264
                            ? (nal_type == 7 || nal_type == 8)
                            : (nal_type == 32 || nal_type == 33 || nal_type == 34);
                        const bool is_keyframe = is_h264
                            ? (nal_type == 5 || is_keyframe_nal(pack))
                            : (nal_type == 19 || nal_type == 20 || is_keyframe_nal(pack));
                        const bool is_bootstrap_nal = is_keyframe || is_parameter_set;

                        if (bootstrap_log_budget > 0 && encChn == 1 && needs_video_bootstrap)
                        {
                            LOG_INFO("stream1 bootstrap pack: payload_nal=" << static_cast<int>(payload_nal_type)
                                     << " pack_nal=" << static_cast<int>(pack_nal_type)
                                     << " chosen_nal=" << static_cast<int>(nal_type)
                                     << " keyframe=" << (is_keyframe ? "true" : "false")
                                     << " param=" << (is_parameter_set ? "true" : "false")
                                     << " len=" << payload.size());
                            --bootstrap_log_budget;
                        }

                        if (!global_video[encChn]->producer_has_keyframe
                            && !is_bootstrap_nal)
                        {
                            continue;
                        }

                        if (global_video[encChn]->producer_has_keyframe || is_bootstrap_nal)
                        {
                            H264NALUnit nalu;
                            nalu.time = frame_time;
                            nalu.data = std::move(payload);
                            nalu.is_keyframe = is_keyframe;
                            nalu.is_sync = nalu.is_keyframe;
                            nalu.is_parameter_set = is_parameter_set;
                            if (is_h264)
                            {
                                nalu.nal_type = nal_type;
                                const auto *stream_cfg = global_video[encChn]->stream;
                                const uint8_t target_level_idc =
                                    (stream_cfg->width <= 1280 && stream_cfg->height <= 720
                                     && stream_cfg->fps <= 30) ? 40 : 0;
                                patch_h264_sps_timing(nalu.data, stream_cfg->fps, target_level_idc);
                            }
                            else
                            {
                                nalu.nal_type = nal_type;
                            }

                            update_parameter_cache(*global_video[encChn], nalu);

                            if (!global_video[encChn]->producer_has_keyframe)
                            {
                                if (!nalu.is_keyframe)
                                {
                                    continue;
                                }
                                global_video[encChn]->producer_has_keyframe = true;
                            }

                            if (has_subscribers)
                            {
                                global_video[encChn]->streamCore->publish(std::move(nalu));
                                wrote_video_data = true;
                            }
                        }
                    }
                }

                if (waiting_for_keyframe && !global_video[encChn]->producer_has_keyframe)
                {
                    auto now = std::chrono::steady_clock::now();
                    if (now >= next_idr_retry)
                    {
                        request_idr(*global_video[encChn]);
                        next_idr_retry = now + 250ms;
                    }
                }

#if defined(USE_AUDIO_STREAM_REPLICATOR)
                if (wrote_video_data && cfg->audio.input_enabled && !global_audio[0]->active
                    && !global_restart.load(std::memory_order_relaxed))
                {
                    global_audio[0]->should_grab_frames.notify_one();
                }
#endif

                IMP_Encoder_ReleaseStream(encChn, &stream);

                ms = WorkerUtils::getMonotonicTimeDiffInMs(&global_video[encChn]->stream->stats.ts);
                if (ms > 1000)
                {
                    /* currently we write into osd and stream stats,
                     * osd will be removed and redesigned in future
                    */
                    global_video[encChn]->stream->stats.bps = bps;
                    global_video[encChn]->stream->osd.stats.bps = bps;
                    global_video[encChn]->stream->stats.fps = fps;
                    global_video[encChn]->stream->osd.stats.fps = fps;
                    global_video[encChn]->observed_frame_rate.store(
                        observed_frames,
                        std::memory_order_relaxed);

                    fps = 0;
                    observed_frames = 0;
                    bps = 0;
                    WorkerUtils::getMonotonicTimeOfDay(&global_video[encChn]->stream->stats.ts);
                    global_video[encChn]->stream->osd.stats.ts = global_video[encChn]
                                                                     ->stream->stats.ts;
                    /*
                    IMPEncoderCHNStat encChnStats;
                    IMP_Encoder_Query(channel->encChn, &encChnStats);
                    LOG_DEBUG("ChannelStats::" << channel->encChn <<
                                ", registered:" << encChnStats.registered <<
                                ", leftPics:" << encChnStats.leftPics <<
                                ", leftStreamBytes:" << encChnStats.leftStreamBytes <<
                                ", leftStreamFrames:" << encChnStats.leftStreamFrames <<
                                ", curPacks:" << encChnStats.curPacks <<
                                ", work_done:" << encChnStats.work_done);
                    */
                    if (global_video[encChn]->idr_fix)
                    {
                        request_idr(*global_video[encChn]);
                        global_video[encChn]->idr_fix--;
                    }
                    publish_video_runtime_status(*global_video[encChn]);
                }
            }
            else
            {
                error_count++;
                LOG_DDEBUG("IMP_Encoder_PollingStream("
                           << encChn << ", " << cfg->general.imp_polling_timeout << ") timeout !");
            }
        }
    }
}

void *VideoWorker::thread_entry(void *arg)
{
    StartHelper *sh = static_cast<StartHelper *>(arg);
    int encChn = sh->encChn;

    LOG_DEBUG("Start stream_grabber thread for stream " << encChn);

    int ret;

    const bool pipeline_prepared = global_video[encChn]->imp_framesource && global_video[encChn]->imp_encoder;
    if (!global_video[encChn]->imp_framesource)
    {
        if (encChn == 1 && global_video[encChn]->encGrp == 0 && global_video[0] && global_video[0]->imp_framesource)
        {
            global_video[encChn]->imp_framesource = global_video[0]->imp_framesource;
            global_video[encChn]->owns_framesource = false;
        }
        else
        {
            global_video[encChn]->owns_framesource = true;
            global_video[encChn]->imp_framesource = IMPFramesource::createNew(global_video[encChn]->stream,
                                                                              &cfg->sensor,
                                                                              global_video[encChn]->fsChn,
                                                                              global_video[encChn]->sourceChn);
        }
    }
    if (!global_video[encChn]->imp_encoder)
    {
        global_video[encChn]->imp_encoder = IMPEncoder::createNew(global_video[encChn]->stream,
                                                                  encChn,
                                                                  global_video[encChn]->encGrp,
                                                                  global_video[encChn]->fsChn,
                                                                  global_video[encChn]->name);
    }
    if (!pipeline_prepared)
    {
        global_video[encChn]->imp_framesource->enable();
    }
    global_video[encChn]->run_for_jpeg = false;
    global_video[encChn]->producer_has_keyframe = false;
    global_video[encChn]->last_timestamp_us.store(0, std::memory_order_relaxed);
    global_video[encChn]->timestamp_origin_raw.store(0, std::memory_order_relaxed);
    global_video[encChn]->last_frame_timestamp_raw.store(0, std::memory_order_relaxed);
    global_video[encChn]->presentation_origin_us.store(0, std::memory_order_relaxed);
    global_video[encChn]->last_encoder_wall_us.store(0, std::memory_order_relaxed);
    global_video[encChn]->last_encoder_wall_delta_us.store(0, std::memory_order_relaxed);
    global_video[encChn]->last_encoder_raw_delta_us.store(0, std::memory_order_relaxed);
    global_video[encChn]->observed_frame_rate.store(0, std::memory_order_relaxed);
    global_video[encChn]->bootstrap_requested.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(global_video[encChn]->parameterCache.mutex);
        global_video[encChn]->parameterCache.sps = H264NALUnit{};
        global_video[encChn]->parameterCache.pps = H264NALUnit{};
        global_video[encChn]->parameterCache.vps = H264NALUnit{};
        global_video[encChn]->parameterCache.latest_sync = H264NALUnit{};
        global_video[encChn]->parameterCache.have_sps = false;
        global_video[encChn]->parameterCache.have_pps = false;
        global_video[encChn]->parameterCache.have_vps = false;
        global_video[encChn]->parameterCache.have_latest_sync = false;
        global_video[encChn]->parameterCache.last_idr_us = 0;
        global_video[encChn]->parameterCache.profile_idc = 0;
        global_video[encChn]->parameterCache.level_idc = 0;
    }

    ret = IMP_Encoder_StartRecvPic(encChn);
    LOG_DEBUG_OR_ERROR(ret, "IMP_Encoder_StartRecvPic(" << encChn << ")");
    if (ret != 0)
        return 0;

    // Proactively request an IDR to ensure SPS/PPS are emitted promptly
    request_idr(*global_video[encChn]);
    LOG_DEBUG("IMP_Encoder_RequestIDR(" << encChn << ")");
    // Also schedule a couple more IDR requests in the first seconds, just in case
    global_video[encChn]->idr_fix = 2;

    /* 'active' indicates, the thread is activly polling and grabbing images
     * 'running' describes the runlevel of the thread, if this value is set to false
     *           the thread exits and cleanup all ressources
     */
    global_video[encChn]->active = true;
    global_video[encChn]->running = true;

    // Only tell the main thread that startup finished once the encoder is
    // actually receiving pictures and the worker loop can service bootstrap
    // requests immediately.
    sh->has_started.release();

    VideoWorker worker(encChn);
    worker.run();

    ret = IMP_Encoder_StopRecvPic(encChn);
    LOG_DEBUG_OR_ERROR(ret, "IMP_Encoder_StopRecvPic(" << encChn << ")");

    if (global_video[encChn]->imp_encoder)
    {
        global_video[encChn]->imp_encoder->deinit();
        delete global_video[encChn]->imp_encoder;
        global_video[encChn]->imp_encoder = nullptr;
    }

    if (global_video[encChn]->imp_framesource && global_video[encChn]->owns_framesource)
    {
        global_video[encChn]->imp_framesource->disable();
        delete global_video[encChn]->imp_framesource;
    }
    global_video[encChn]->imp_framesource = nullptr;

    return 0;
}
