#ifndef AUDIO_WORKER_HPP
#define AUDIO_WORKER_HPP

#include "AudioReframer.hpp"
#include "IMPAudio.hpp"

#include <memory>
#include <vector>
#include <atomic>
#include <chrono>

#if defined(AUDIO_SUPPORT)

class AudioWorker
{
public:
    explicit AudioWorker(int encChn);
    ~AudioWorker();

    static void *thread_entry(void *arg);

private:
    void run();
    void reset_reframer();
    void process_audio_frame_direct(IMPAudioFrame &frame);
    void process_frame(IMPAudioFrame &frame);

    int encChn;
    std::unique_ptr<AudioReframer> reframer;
    std::vector<uint8_t> directEncBuf;
    uint32_t clogged_drop_count_ = 0;
    std::chrono::steady_clock::time_point next_clogged_log_{};

    // Frame accumulator for Opus
    std::vector<int16_t> frameBuffer;
    int64_t bufferStartTimestamp = 0;
    int targetSamplesPerChannel = 0;

    // Buffer safety controls (computed from targetSamplesPerChannel)
    int maxBufferSamplesPerChannel = 0;     // e.g., 5 * targetSamplesPerChannel
    int warnBufferSamplesPerChannel = 0;    // e.g., 3 * targetSamplesPerChannel

    // Diagnostics / metrics
    std::atomic<uint32_t> bufferDropCount{0};
    uint64_t sampleTimelineUs{0};
};

#endif // AUDIO_SUPPORT
#endif // AUDIO_WORKER_HPP
