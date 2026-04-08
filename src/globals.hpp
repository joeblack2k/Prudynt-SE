#ifndef GLOBALS_HPP
#define GLOBALS_HPP

#include <memory>
#include <functional>
#include <atomic>
#include <optional>
#include "liveMedia.hh"

#include "MsgChannel.hpp"
#include "IMPAudio.hpp"
#include "IMPEncoder.hpp"
#include "IMPFramesource.hpp"
#include "IMPBackchannel.hpp"
#include "StreamCore.hpp"

#define VIDEO_MSG_CHANNEL_SIZE 512
#define AUDIO_MSG_CHANNEL_SIZE 96
#define BACKCHANNEL_MSG_CHANNEL_SIZE 20
#define NUM_AUDIO_CHANNELS 1
#define NUM_VIDEO_CHANNELS 2
#define NUM_JPEG_CHANNELS 2

using namespace std::chrono;

struct FrameSourceBinding
{
    int fsChn;
    int sourceChn;
};

inline constexpr int kMainPhysicalFrameSourceChannel = 0;
inline constexpr int kSubPhysicalFrameSourceChannel = 1;
inline constexpr int kExtSubFrameSourceChannel = 2;

inline bool uses_shared_primary_encoder(int video_channel, const _sensor &sensor, const _stream &stream)
{
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
    (void)video_channel;
    (void)sensor;
    (void)stream;
    return false;
#else
    (void)video_channel;
    (void)sensor;
    (void)stream;
    return false;
#endif
}

inline FrameSourceBinding framesource_binding_for_video(int video_channel, const _sensor &sensor, const _stream &stream)
{
    (void)sensor;
    (void)stream;

    if (video_channel == 1)
        return {kSubPhysicalFrameSourceChannel, kSubPhysicalFrameSourceChannel};

    if (uses_shared_primary_encoder(video_channel, sensor, stream))
        return {kMainPhysicalFrameSourceChannel, kMainPhysicalFrameSourceChannel};

    return {video_channel, video_channel};
}

inline int encoder_group_for_video(int video_channel, const _sensor &sensor, const _stream &stream)
{
    return uses_shared_primary_encoder(video_channel, sensor, stream) ? 0 : video_channel;
}

extern std::mutex mutex_main; // coordinates restart signaling between control and worker threads

struct AudioFrame
{
	std::vector<uint8_t> data;
	struct timeval time;
    uint64_t duration_us{0};
};

struct H264NALUnit
{
    std::vector<uint8_t> data;
    struct timeval time;
    bool is_sync{false};
    bool is_parameter_set{false};
    bool is_keyframe{false};
    uint8_t nal_type{0};
};

template <>
struct StreamCoreTraits<AudioFrame>
{
    static bool is_sync(const AudioFrame &)
    {
        return false;
    }
};

template <>
struct StreamCoreTraits<H264NALUnit>
{
    static bool is_sync(const H264NALUnit &nalu)
    {
        return nalu.is_keyframe;
    }
};

struct video_parameter_cache
{
    std::mutex mutex;
    std::condition_variable cv;
    H264NALUnit sps;
    H264NALUnit pps;
    H264NALUnit vps;
    H264NALUnit latest_sync;
    bool have_sps{false};
    bool have_pps{false};
    bool have_vps{false};
    bool have_latest_sync{false};
    uint64_t last_idr_us{0};
    uint8_t profile_idc{0};
    uint8_t level_idc{0};
};

struct BackchannelFrame
{
    std::vector<uint8_t> payload;
    IMPBackchannelFormat format;
    unsigned int clientSessionId;
};

struct jpeg_stream
{
    static uint64_t monotonic_now_us()
    {
        return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
    }

    int encChn;
    int streamChn;
    _stream *stream;
    std::atomic<bool> running; // set to false to make jpeg_grabber thread exit
    std::atomic<bool> active{false};
    pthread_t thread;
    IMPEncoder *imp_encoder;
    std::condition_variable should_grab_frames;
    std::binary_semaphore is_activated{0};
    std::vector<unsigned char> snapshot_buf;
    std::atomic<int> quality_override{-1};
    std::atomic<uint32_t> frame_seq{0};
    std::atomic<int> req_width{-1};
    std::atomic<int> req_height{-1};
    std::atomic<int> req_fps{-1};
    std::atomic<bool> reconfig{false};

    std::atomic<uint64_t> last_image_us{0};
    std::atomic<uint64_t> last_subscriber_us{0};

    void request()
    {
        last_subscriber_us.store(monotonic_now_us(), std::memory_order_relaxed);
    }

    bool request_or_overrun() const
    {
        const uint64_t last = last_subscriber_us.load(std::memory_order_relaxed);
        return last != 0 && (monotonic_now_us() - last) < 1000000ULL;
    }

    uint64_t micros_since_last_image() const
    {
        const uint64_t last = last_image_us.load(std::memory_order_relaxed);
        if (last == 0)
            return UINT64_MAX;
        return monotonic_now_us() - last;
    }

    void mark_image_produced()
    {
        last_image_us.store(monotonic_now_us(), std::memory_order_relaxed);
    }

    jpeg_stream(int encChn, _stream *stream)
        : encChn(encChn), stream(stream), running(false), imp_encoder(nullptr) {}
};

struct audio_stream
{
    int devId;
    int aiChn;
    int aeChn;
    bool running;
    bool active{false};
    pthread_t thread;
    IMPAudio *imp_audio;
    std::shared_ptr<MsgChannel<AudioFrame>> msgChannel;
    std::function<void(void)> onDataCallback;
    /* Check whether onDataCallback is not null in a data race free manner.
     * Returns a momentary value that may be stale by the time it is returned.
     * Use only for optimizations, i.e., to skip work if no data callback
     * is registered right now.
     */
    std::atomic<bool> hasDataCallback;
    std::mutex onDataCallbackLock; // protects onDataCallback from deallocation
    std::condition_variable should_grab_frames;
    std::binary_semaphore is_activated{0};
    std::atomic<int64_t> timestamp_origin_raw{0};
    std::atomic<uint64_t> last_timestamp_us{0};
    std::atomic<uint64_t> presentation_origin_us{0};
    std::shared_ptr<StreamCore<AudioFrame>> streamCore;

    StreamReplicator *streamReplicator = nullptr;

    audio_stream(int devId, int aiChn, int aeChn)
        : devId(devId), aiChn(aiChn), aeChn(aeChn), running(false), imp_audio(nullptr),
          msgChannel(std::make_shared<MsgChannel<AudioFrame>>(AUDIO_MSG_CHANNEL_SIZE)),
          streamCore(std::make_shared<StreamCore<AudioFrame>>(AUDIO_MSG_CHANNEL_SIZE)),
          onDataCallback{nullptr}, hasDataCallback{false} {}
};

struct video_stream
{
    int encChn;
    int encGrp;
    int fsChn;
    int sourceChn;
    _stream *stream;
    const char *name;
    bool running;
    pthread_t thread;
    bool producer_has_keyframe;
    int idr_fix;
    bool active{false};
    IMPEncoder *imp_encoder;
    IMPFramesource *imp_framesource;
    std::shared_ptr<MsgChannel<H264NALUnit>> msgChannel;
    std::shared_ptr<StreamCore<H264NALUnit>> streamCore;
    video_parameter_cache parameterCache;
    std::function<void(void)> onDataCallback;
    bool run_for_jpeg;                 // see comment in audio_stream
    std::atomic<bool> hasDataCallback; // see comment in audio_stream
    std::atomic<uint64_t> last_timestamp_us{0};
    std::atomic<uint64_t> timestamp_origin_raw{0};
    std::atomic<uint64_t> last_frame_timestamp_raw{0};
    std::atomic<uint64_t> presentation_origin_us{0};
    std::atomic<uint64_t> last_idr_request_us{0};
    std::atomic<uint64_t> last_encoder_wall_us{0};
    std::atomic<uint64_t> last_encoder_wall_delta_us{0};
    std::atomic<uint64_t> last_encoder_raw_delta_us{0};
    std::atomic<uint32_t> observed_frame_rate{0};
    std::atomic<bool> bootstrap_requested{false};
    bool owns_framesource{true};
    std::mutex onDataCallbackLock;     // protects onDataCallback from deallocation
    std::condition_variable should_grab_frames;
    std::binary_semaphore is_activated{0};
    FramedSource *rtspSource{nullptr};
    StreamReplicator *streamReplicator{nullptr};

    video_stream(int encChn, int encGrp, int fsChn, int sourceChn, _stream *stream, const char *name)
        : encChn(encChn), encGrp(encGrp), fsChn(fsChn), sourceChn(sourceChn), stream(stream), name(name), running(false), producer_has_keyframe(false), idr_fix(0), imp_encoder(nullptr), imp_framesource(nullptr),
          msgChannel(std::make_shared<MsgChannel<H264NALUnit>>(VIDEO_MSG_CHANNEL_SIZE)),
          streamCore(std::make_shared<StreamCore<H264NALUnit>>(VIDEO_MSG_CHANNEL_SIZE)),
          onDataCallback(nullptr),  run_for_jpeg{false},
          hasDataCallback{false} {}
};

struct backchannel_stream
{
    std::shared_ptr<MsgChannel<BackchannelFrame>> inputQueue;
    IMPBackchannel* imp_backchannel;
    bool running;
    pthread_t thread;
    std::mutex mutex;
    std::condition_variable should_grab_frames;
    std::atomic<unsigned int> is_sending{0};

    backchannel_stream()
        : inputQueue(std::make_shared<MsgChannel<BackchannelFrame>>(BACKCHANNEL_MSG_CHANNEL_SIZE)),
        imp_backchannel(nullptr),
        running(false) {}
};


extern std::condition_variable global_cv_worker_restart;

extern std::atomic<bool> global_restart;
extern std::atomic<bool> global_restart_rtsp;
extern std::atomic<bool> global_restart_video;
extern std::atomic<bool> global_restart_audio;

extern std::atomic<bool> global_osd_thread_signal;
extern std::atomic<bool> global_main_thread_signal;
extern std::atomic<bool> global_motion_thread_signal;
extern std::atomic<char> global_rtsp_thread_signal;
extern volatile char global_rtsp_thread_watch;

extern std::shared_ptr<jpeg_stream> global_jpeg[NUM_JPEG_CHANNELS];
extern std::shared_ptr<audio_stream> global_audio[NUM_AUDIO_CHANNELS];
extern std::shared_ptr<video_stream> global_video[NUM_VIDEO_CHANNELS];
extern std::shared_ptr<backchannel_stream> global_backchannel;

#endif // GLOBALS_HPP
