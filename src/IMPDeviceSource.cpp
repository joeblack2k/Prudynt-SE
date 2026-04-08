#include "IMPDeviceSource.hpp"
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <type_traits>

namespace
{
struct timeval timeval_from_us(uint64_t tsUs)
{
    struct timeval tv;
    tv.tv_sec = tsUs / 1000000ULL;
    tv.tv_usec = tsUs % 1000000ULL;
    return tv;
}

std::mutex gSessionVideoReadyMutex;
std::unordered_map<unsigned, bool> gSessionVideoReady;

bool is_session_video_ready(unsigned clientSessionId)
{
    if (clientSessionId == 0)
        return true;

    std::lock_guard<std::mutex> lock(gSessionVideoReadyMutex);
    const auto it = gSessionVideoReady.find(clientSessionId);
    return it != gSessionVideoReady.end() && it->second;
}

void set_session_video_ready(unsigned clientSessionId, bool ready)
{
    if (clientSessionId == 0)
        return;

    std::lock_guard<std::mutex> lock(gSessionVideoReadyMutex);
    gSessionVideoReady[clientSessionId] = ready;
}

void clear_session_video_ready(unsigned clientSessionId)
{
    if (clientSessionId == 0)
        return;

    std::lock_guard<std::mutex> lock(gSessionVideoReadyMutex);
    gSessionVideoReady.erase(clientSessionId);
}
}

// explicit instantiation
template class IMPDeviceSource<H264NALUnit, video_stream>;
template class IMPDeviceSource<AudioFrame, audio_stream>;

template<typename FrameType, typename Stream>
IMPDeviceSource<FrameType, Stream> *IMPDeviceSource<FrameType, Stream>::createNew(UsageEnvironment &env, int encChn, std::shared_ptr<Stream> stream, const char *name, bool eagerActivate, unsigned clientSessionId)
{
    return new IMPDeviceSource<FrameType, Stream>(env, encChn, stream, name, eagerActivate, clientSessionId);
}

template<typename FrameType, typename Stream>
IMPDeviceSource<FrameType, Stream>::IMPDeviceSource(UsageEnvironment &env, int encChn, std::shared_ptr<Stream> stream, const char *name, bool eagerActivate, unsigned clientSessionId)
    : FramedSource(env), encChn(encChn), clientSessionId(clientSessionId), stream{stream}, name{name}, eventTriggerId(0), eagerActivate(eagerActivate)
{
    eventTriggerId = envir().taskScheduler().createEventTrigger(deliverFrame0);
    if (eagerActivate)
        setCaptureEnabled(true);
    LOG_DEBUG("IMPDeviceSource " << name << " constructed, encoder channel:" << encChn);
}

template<typename FrameType, typename Stream>
void IMPDeviceSource<FrameType, Stream>::deinit()
{
    setCaptureEnabled(false);
    envir().taskScheduler().deleteEventTrigger(eventTriggerId);
    if constexpr (std::is_same_v<FrameType, H264NALUnit>)
    {
        clear_session_video_ready(clientSessionId);
    }
    LOG_DEBUG("IMPDeviceSource " << name << " destructed, encoder channel:" << encChn);
}

template<typename FrameType, typename Stream>
IMPDeviceSource<FrameType, Stream>::~IMPDeviceSource()
{
    deinit();
}

template <typename FrameType, typename Stream>
uint64_t IMPDeviceSource<FrameType, Stream>::normalizePresentationTimeUs(uint64_t sourceFrameUs,
                                                                         uint64_t durationUs)
{
    const uint64_t fallbackDurationUs = std::max<uint64_t>(durationUs, 1);

    if (sourceFrameUs != 0 && lastSourceFrameUs != 0
        && sourceFrameUs == lastSourceFrameUs && lastPresentationFrameUs != 0)
    {
        return lastPresentationFrameUs;
    }

    uint64_t normalizedUs = 0;
    if (sourceFrameUs != 0)
    {
        if (presentationAnchorUs == 0)
        {
            if constexpr (std::is_same_v<FrameType, H264NALUnit>)
            {
                presentationAnchorUs =
                    sourceFrameUs > fallbackDurationUs ? sourceFrameUs - fallbackDurationUs : 0;
            }
            else
            {
                // AAC demuxers commonly derive stream start_time as first_pts - frame_duration.
                // Anchor audio one frame earlier so the first emitted packet lands at +duration,
                // which yields a clean zero start_time instead of -0.064 on 16 kHz AAC.
                presentationAnchorUs =
                    sourceFrameUs > fallbackDurationUs ? sourceFrameUs - fallbackDurationUs : 0;
            }
        }
        if (sourceFrameUs >= presentationAnchorUs)
            normalizedUs = sourceFrameUs - presentationAnchorUs;
    }

    if (lastPresentationFrameUs != 0 && normalizedUs <= lastPresentationFrameUs)
        normalizedUs = lastPresentationFrameUs + fallbackDurationUs;

    lastSourceFrameUs = sourceFrameUs;
    lastPresentationFrameUs = normalizedUs;
    return normalizedUs;
}

template<typename FrameType, typename Stream>
void IMPDeviceSource<FrameType, Stream>::doGetNextFrame()
{
    if (!captureEnabled)
        setCaptureEnabled(true);
    deliverFrame();
}

template <typename FrameType, typename Stream>
void IMPDeviceSource<FrameType, Stream>::doStopGettingFrames()
{
    FramedSource::doStopGettingFrames();
    if (!eagerActivate && captureEnabled)
        setCaptureEnabled(false);
}

template <typename FrameType, typename Stream>
void IMPDeviceSource<FrameType, Stream>::deliverFrame0(void *clientData)
{
    ((IMPDeviceSource<FrameType, Stream> *)clientData)->deliverFrame();
}

template <typename FrameType, typename Stream>
void IMPDeviceSource<FrameType, Stream>::deliverFrame()
{
    if (!isCurrentlyAwaitingData())
        return;

    if constexpr (std::is_same_v<FrameType, AudioFrame>)
    {
        if (!is_session_video_ready(clientSessionId))
            return;
    }

    FrameType nal;
    const bool haveFrame = cursor.has_value() && stream->streamCore->read(*cursor, &nal);

    if (haveFrame)
    {
        if (nal.data.size() > fMaxSize)
        {
            fFrameSize = fMaxSize;
            fNumTruncatedBytes = nal.data.size() - fMaxSize;
        }
        else
        {
            fFrameSize = nal.data.size();
        }

        const uint64_t source_frame_us =
            static_cast<uint64_t>(nal.time.tv_sec) * 1000000ULL
            + static_cast<uint64_t>(nal.time.tv_usec);
        uint64_t duration_us = 0;

        if (lastSourceFrameUs != 0 && source_frame_us > lastSourceFrameUs)
        {
            duration_us = source_frame_us - lastSourceFrameUs;
        }
        else if constexpr (std::is_same_v<FrameType, H264NALUnit>)
        {
            const int fps = (stream && stream->stream && stream->stream->fps > 0)
                ? stream->stream->fps
                : 25;
            duration_us = 1000000ULL / static_cast<uint64_t>(fps);
        }
        else if constexpr (std::is_same_v<FrameType, AudioFrame>)
        {
            duration_us = nal.duration_us > 0 ? nal.duration_us : 40000ULL;
        }
        const uint64_t presentation_us =
            normalizePresentationTimeUs(source_frame_us, duration_us);
        fPresentationTime = timeval_from_us(presentation_us);
        fDurationInMicroseconds = duration_us;

        if constexpr (std::is_same_v<FrameType, H264NALUnit>)
        {
            set_session_video_ready(clientSessionId, true);
        }

        if (debugFramesLogged < 8)
        {
            const char *frameKind =
                std::is_same_v<FrameType, H264NALUnit> ? "video" : "audio";
            LOG_INFO("IMPDeviceSource[" << frameKind << "] session=" << clientSessionId
                     << " source_us=" << source_frame_us
                     << " duration_us=" << duration_us
                     << " presentation_us=" << presentation_us
                     << " last_source_us=" << lastSourceFrameUs
                     << " last_presentation_us=" << lastPresentationFrameUs);
            ++debugFramesLogged;
        }

        if (fFrameSize > 0)
        {
            std::memcpy(fTo, nal.data.data(), fFrameSize);
            FramedSource::afterGetting(this);
        }
    }
    else
    {
        fFrameSize = 0;
    }
}

template <typename FrameType, typename Stream>
void IMPDeviceSource<FrameType, Stream>::queueBootstrapFramesForTimestamp(const struct timeval &timestamp)
{
    bootstrapFrames.clear();

    if constexpr (std::is_same_v<FrameType, H264NALUnit>)
    {
        std::lock_guard<std::mutex> lock(stream->parameterCache.mutex);
        auto queueFrame = [&](const H264NALUnit &cached) {
            H264NALUnit frame = cached;
            frame.time = timestamp;
            bootstrapFrames.push_back(std::move(frame));
        };
        if (strcmp(stream->stream->format, "H265") == 0)
        {
            if (stream->parameterCache.have_vps)
                queueFrame(stream->parameterCache.vps);
        }
        if (stream->parameterCache.have_sps)
            queueFrame(stream->parameterCache.sps);
        if (stream->parameterCache.have_pps)
            queueFrame(stream->parameterCache.pps);
    }
}

template <typename FrameType, typename Stream>
void IMPDeviceSource<FrameType, Stream>::setCaptureEnabled(bool enabled)
{
    std::lock_guard lock_stream {mutex_main};

    if (captureEnabled == enabled)
        return;

    if (enabled)
    {
        bootstrapPendingForFirstSync = false;
        presentationAnchorUs = 0;
        lastSourceFrameUs = 0;
        lastPresentationFrameUs = 0;
        debugFramesLogged = 0;
        if constexpr (std::is_same_v<FrameType, H264NALUnit>)
        {
            set_session_video_ready(clientSessionId, false);
        }
        const auto startPolicy = StreamStartPolicy::LiveEdge;
        cursor = stream->streamCore->registerSubscriber(
            [this]() { this->on_data_available(); },
            startPolicy);
        if constexpr (std::is_same_v<FrameType, H264NALUnit>)
        {
            // Start new RTSP viewers on a freshly produced sync frame instead of
            // replaying a possibly stale buffered GOP head. That keeps UDP startup
            // camera-like and avoids first-frame reference warnings in clients.
            cursor->nextSequence += 1;
            cursor->requireSync = true;
            cursor->started = false;
            bootstrapPendingForFirstSync = false;
        }
        stream->hasDataCallback.store(stream->streamCore->subscriberCount() > 0,
                                      std::memory_order_relaxed);
        stream->should_grab_frames.notify_one();
        if constexpr (std::is_same_v<FrameType, H264NALUnit>)
        {
            IMP_Encoder_RequestIDR(encChn);
        }
    }
    else
    {
        if (cursor.has_value())
            stream->streamCore->unregisterSubscriber(*cursor);
        cursor.reset();
        bootstrapFrames.clear();
        bootstrapPendingForFirstSync = false;
        presentationAnchorUs = 0;
        lastSourceFrameUs = 0;
        lastPresentationFrameUs = 0;
        debugFramesLogged = 0;
        stream->hasDataCallback.store(stream->streamCore->subscriberCount() > 0,
                                      std::memory_order_relaxed);
    }

    captureEnabled = enabled;
}
