#include "AudioWorker.hpp"

#include "Config.hpp"
#include "Logger.hpp"
#include "WorkerUtils.hpp"
#include "TimestampManager.hpp"
#include "globals.hpp"
#include "RTSPStatus.hpp"
#include <chrono>
#include <sys/time.h>

#define MODULE "AudioWorker"

#if defined(AUDIO_SUPPORT)

namespace
{
uint64_t normalize_audio_timestamp_us(int64_t raw_timestamp)
{
    if (raw_timestamp > 0)
        return static_cast<uint64_t>(raw_timestamp);

    return 0;
}

uint64_t input_frame_duration_us(const audio_stream &stream_state, const IMPAudioFrame &frame)
{
    const int channels = std::max(1, stream_state.imp_audio->outChnCnt);
    const int bytes_per_sample = static_cast<int>(sizeof(int16_t));
    const int sample_rate = std::max(1, stream_state.imp_audio->sample_rate);
    const int samples_per_channel = frame.len / (bytes_per_sample * channels);
    if (samples_per_channel <= 0)
        return 1;

    return static_cast<uint64_t>(samples_per_channel) * 1000000ULL
        / static_cast<uint64_t>(sample_rate);
}

uint64_t pick_audio_anchor_us(int encChn, uint64_t frame_duration_us)
{
    uint64_t anchor_us = frame_duration_us;

    for (int videoChn = 0; videoChn < NUM_VIDEO_CHANNELS; ++videoChn)
    {
        if (!global_video[videoChn])
            continue;

        const uint64_t video_ts =
            global_video[videoChn]->last_timestamp_us.load(std::memory_order_relaxed);
        if (video_ts > anchor_us)
            anchor_us = video_ts;
    }

    return anchor_us;
}
}

AudioWorker::AudioWorker(int chn)
    : encChn(chn)
{
    next_clogged_log_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    LOG_DEBUG("AudioWorker created for channel " << encChn);
}

AudioWorker::~AudioWorker()
{
    LOG_DEBUG("AudioWorker destroyed for channel " << encChn);
}

void AudioWorker::reset_reframer()
{
    if (global_audio[encChn]->imp_audio->format != IMPAudioFormat::AAC)
    {
        reframer.reset();
        return;
    }

    reframer = std::make_unique<AudioReframer>(
        global_audio[encChn]->imp_audio->sample_rate,
        /* inputSamplesPerFrame */ global_audio[encChn]->imp_audio->sample_rate * 0.040,
        /* outputSamplesPerFrame */ 1024,
        global_audio[encChn]->imp_audio->outChnCnt);
}

void AudioWorker::process_audio_frame_direct(IMPAudioFrame &frame)
{
    AudioFrame af;
    const uint64_t frame_duration_us =
        std::max<uint64_t>(1, input_frame_duration_us(*global_audio[encChn], frame));
    af.duration_us = frame_duration_us;

    uint64_t presentation_origin_us =
        global_audio[encChn]->presentation_origin_us.load(std::memory_order_relaxed);
    if (presentation_origin_us == 0)
    {
        const uint64_t fallback_origin_us = pick_audio_anchor_us(encChn, frame_duration_us);
        uint64_t expected = 0;
        if (global_audio[encChn]->presentation_origin_us.compare_exchange_strong(
                expected, fallback_origin_us, std::memory_order_relaxed))
        {
            presentation_origin_us = fallback_origin_us;
        }
        else
        {
            presentation_origin_us = expected;
        }
    }

    uint64_t ts_us = sampleTimelineUs;
    if (ts_us == 0)
        ts_us = presentation_origin_us;

    uint64_t last_ts = global_audio[encChn]->last_timestamp_us.load(std::memory_order_relaxed);
    if (last_ts != 0 && ts_us <= last_ts)
        ts_us = last_ts + frame_duration_us;
    sampleTimelineUs = ts_us + frame_duration_us;
    global_audio[encChn]->last_timestamp_us.store(ts_us, std::memory_order_relaxed);
    global_audio[encChn]->timestamp_origin_raw.store(
        static_cast<int64_t>(normalize_audio_timestamp_us(frame.timeStamp)),
        std::memory_order_relaxed);

    af.time.tv_sec = ts_us / 1000000ULL;
    af.time.tv_usec = ts_us % 1000000ULL;

    uint8_t *start = (uint8_t *) frame.virAddr;
    uint8_t *end = start + frame.len;

    IMPAudioStream stream;
    bool got_stream = false;
    if (global_audio[encChn]->imp_audio->directEncode)
    {
        int outLen = 0;
        if (IMPAudio::encodeDirect(&frame, directEncBuf.data(), &outLen) == 0 && outLen > 0)
        {
            start = directEncBuf.data();
            end = start + outLen;
        }
        else
        {
            end = start;
        }
    }
    else if (global_audio[encChn]->imp_audio->format != IMPAudioFormat::PCM)
    {
        if (IMP_AENC_SendFrame(global_audio[encChn]->aeChn, &frame) != 0)
        {
            LOG_ERROR("IMP_AENC_SendFrame(" << global_audio[encChn]->devId << ", "
                                            << global_audio[encChn]->aeChn << ") failed");
        }
        else if (IMP_AENC_PollingStream(global_audio[encChn]->aeChn,
                                        cfg->general.imp_polling_timeout)
                 != 0)
        {
            LOG_ERROR("IMP_AENC_PollingStream(" << global_audio[encChn]->devId << ", "
                                                << global_audio[encChn]->aeChn << ") failed");
        }
        else if (IMP_AENC_GetStream(global_audio[encChn]->aeChn, &stream, IMPBlock::BLOCK) != 0)
        {
            LOG_ERROR("IMP_AENC_GetStream(" << global_audio[encChn]->devId << ", "
                                            << global_audio[encChn]->aeChn << ") failed");
        }
        else
        {
            got_stream = true;
            start = (uint8_t *) stream.stream;
            end = start + stream.len;
        }
    }

    if (end > start)
    {
        af.data.insert(af.data.end(), start, end);
    }

    if (!af.data.empty()
        && global_audio[encChn]->streamCore->subscriberCount() > 0)
    {
        global_audio[encChn]->streamCore->publish(std::move(af));
    }

    if (got_stream
        && IMP_AENC_ReleaseStream(global_audio[encChn]->aeChn, &stream) < 0)
    {
        LOG_ERROR("IMP_AENC_ReleaseStream(" << global_audio[encChn]->devId << ", "
                                            << global_audio[encChn]->aeChn << ", &stream) failed");
    }
}

void AudioWorker::process_frame(IMPAudioFrame &frame)
{
    // Handle Opus frame accumulation (320 -> 960 samples) to fix timing drift
    if (global_audio[encChn]->imp_audio->format == IMPAudioFormat::OPUS && targetSamplesPerChannel > 0) {
        int samplesPerChannel = (frame.len / sizeof(int16_t)) / global_audio[encChn]->imp_audio->outChnCnt;

        // If this is the first frame in the buffer, save the timestamp
        if (frameBuffer.empty()) {
            // SINGLE SOURCE OF TRUTH: Use TimestampManager timestamp
            bufferStartTimestamp = TimestampManager::getInstance().getTimestampUs();
            // Only log if verbose audio debugging is enabled
            if (cfg->general.audio_debug_verbose) {
                static int accumulationLogCount = 0;
                if (accumulationLogCount < 3) {
                    LOG_DEBUG("Starting new Opus frame accumulation: " << samplesPerChannel << " samples per channel");
                    accumulationLogCount++;
                }
            }
        }

        // Buffer safety: bound growth and drop oldest on overflow
        int outCh = global_audio[encChn]->imp_audio->outChnCnt;
        int16_t *samples = (int16_t*)frame.virAddr;
        int totalSamples = frame.len / sizeof(int16_t);
        int currentSamplesPerChannel = frameBuffer.size() / outCh;
        int incomingSamplesPerChannel = totalSamples / outCh;
        int predictedSamplesPerChannel = currentSamplesPerChannel + incomingSamplesPerChannel;

        if (warnBufferSamplesPerChannel > 0 && predictedSamplesPerChannel >= warnBufferSamplesPerChannel) {
            // Rate-limit buffer capacity warnings to avoid log spam
            static auto lastWarnTime = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            auto timeSinceLastWarn = std::chrono::duration_cast<std::chrono::seconds>(now - lastWarnTime);

            if (timeSinceLastWarn.count() >= 5) { // Warn at most every 5 seconds
                LOG_WARN("AudioWorker buffer nearing capacity: " << predictedSamplesPerChannel
                         << "/" << maxBufferSamplesPerChannel << " samples/ch (" << bufferDropCount.load() << " drops so far)");
                lastWarnTime = now;
            }
        }

        // Drop oldest samples to keep within capacity
        while (maxBufferSamplesPerChannel > 0 && predictedSamplesPerChannel > maxBufferSamplesPerChannel && !frameBuffer.empty()) {
            int dropSamplesPerChannel = std::max(targetSamplesPerChannel, predictedSamplesPerChannel - maxBufferSamplesPerChannel);
            int dropTotalSamples = dropSamplesPerChannel * outCh;
            dropTotalSamples = std::min(dropTotalSamples, (int)frameBuffer.size());
            frameBuffer.erase(frameBuffer.begin(), frameBuffer.begin() + dropTotalSamples);
            predictedSamplesPerChannel -= dropSamplesPerChannel;
            bufferDropCount.fetch_add(1);
            // Advance buffer start PTS accordingly
            bufferStartTimestamp += (int64_t) ( (dropSamplesPerChannel * 1000000LL) / global_audio[encChn]->imp_audio->sample_rate );
            // Expose metrics via RTSPStatus
            {
                std::string streamName = std::string("audio") + std::to_string(encChn);
                RTSPStatus::writeCustomParameter(streamName, "buffer_drop_count", std::to_string(bufferDropCount.load()));
                RTSPStatus::writeCustomParameter(streamName, "buffer_level_samples_per_channel", std::to_string(predictedSamplesPerChannel));
            }
            LOG_WARN("AudioWorker dropped " << dropSamplesPerChannel << " samples/ch to bound buffer");
        }

        // Add samples to buffer
        frameBuffer.insert(frameBuffer.end(), samples, samples + totalSamples);

        currentSamplesPerChannel = frameBuffer.size() / outCh;

        // Check if we have enough samples for a complete Opus frame
        // CRITICAL FIX: Use while loop to process multiple accumulated frames
        while (currentSamplesPerChannel >= targetSamplesPerChannel) {
            // Create a new frame with exactly the right number of samples
            int targetTotalSamples = targetSamplesPerChannel * global_audio[encChn]->imp_audio->outChnCnt;
            int targetBytes = targetTotalSamples * sizeof(int16_t);

            IMPAudioFrame opusFrame = frame;
            opusFrame.virAddr = (uint32_t*)frameBuffer.data();
            opusFrame.len = targetBytes;
            // Keep original timestamp - the monotonic PTS will be applied in process_audio_frame_direct
            opusFrame.timeStamp = bufferStartTimestamp;

            // Only log if verbose audio debugging is enabled
            if (cfg->general.audio_debug_verbose) {
                static int readyLogCount = 0;
                if (readyLogCount < 3) {
                    LOG_DEBUG("Opus frame ready: accumulated " << currentSamplesPerChannel
                             << " samples per channel, sending " << targetSamplesPerChannel);
                    readyLogCount++;
                }
            }

            // Analyze raw PCM data for corruption patterns
            static int analysis_count = 0;
            if (analysis_count < 5) {
                int16_t *samples = (int16_t*)frameBuffer.data();
                int16_t min_val = samples[0], max_val = samples[0];
                int zero_count = 0, clip_count = 0;

                for (int i = 0; i < targetTotalSamples; i++) {
                    if (samples[i] == 0) zero_count++;
                    if (samples[i] >= 32767 || samples[i] <= -32768) clip_count++;
                    if (samples[i] < min_val) min_val = samples[i];
                    if (samples[i] > max_val) max_val = samples[i];
                }

                LOG_DEBUG("Raw PCM analysis " << analysis_count << ": min=" << min_val
                         << ", max=" << max_val << ", zeros=" << zero_count
                         << ", clipped=" << clip_count << "/" << targetTotalSamples);
                analysis_count++;
            }

            // Process the accumulated frame
            process_audio_frame_direct(opusFrame);

            // Remove processed samples from buffer
            frameBuffer.erase(frameBuffer.begin(), frameBuffer.begin() + targetTotalSamples);

            // Update timestamp for next frame
            bufferStartTimestamp += (targetSamplesPerChannel * 1000000LL) / global_audio[encChn]->imp_audio->sample_rate;

            // Recalculate remaining samples for next iteration
            currentSamplesPerChannel = frameBuffer.size() / global_audio[encChn]->imp_audio->outChnCnt;
        }

        return; // Don't process the original frame
    }

    if (global_audio[encChn]->imp_audio->outChnCnt == 2 && frame.soundmode == AUDIO_SOUND_MODE_MONO)
    {
        size_t sample_size = frame.bitwidth / 8;
        size_t num_samples = frame.len / sample_size;
        size_t stereo_size = frame.len * 2;
        uint8_t *stereo_buffer = new uint8_t[stereo_size];

        for (size_t i = 0; i < num_samples; i++)
        {
            uint8_t *mono_sample = ((uint8_t *) frame.virAddr) + (i * sample_size);
            uint8_t *stereo_left = stereo_buffer + (i * sample_size * 2);
            uint8_t *stereo_right = stereo_left + sample_size;
            memcpy(stereo_left, mono_sample, sample_size);
            memcpy(stereo_right, mono_sample, sample_size);
        }

        IMPAudioFrame stereo_frame = frame;
        stereo_frame.virAddr = (uint32_t *) stereo_buffer;
        stereo_frame.len = stereo_size;
        stereo_frame.soundmode = AUDIO_SOUND_MODE_STEREO;

        process_audio_frame_direct(stereo_frame);
        delete[] stereo_buffer;
    }
    else
    {
        process_audio_frame_direct(frame);
    }
}

void AudioWorker::run()
{
    LOG_DEBUG("Start audio processing run loop for channel " << encChn);
    bool had_subscribers = false;

    // Using global TimestampManager for unified audio/video timeline
    LOG_DEBUG("AudioWorker using TimestampManager for unified timeline");

    // Initialize AudioReframer only if needed, store in member variable
    if (global_audio[encChn]->imp_audio->format == IMPAudioFormat::AAC)
    {
        reset_reframer();
        LOG_DEBUG("AudioReframer created for channel " << encChn);
    }
    else
    {
        LOG_DEBUG("AudioReframer not needed or imp_audio not ready for channel " << encChn);
    }

    if (global_audio[encChn]->imp_audio->directEncode)
    {
        directEncBuf.resize(8192);
        LOG_DEBUG("Direct encode buffer allocated (8192 bytes) for channel " << encChn);
    }

    // Initialize frame accumulator for Opus
    // RFC 7587 COMPLIANCE NOTE: OPUS RTP timestamps MUST always use 48kHz clock rate
    // for signaling purposes, but the actual input sampling rate can be different
    // (8kHz, 16kHz, 24kHz, 48kHz, etc.). The frame accumulator must collect samples
    // based on the ACTUAL input sample rate, not the RTP clock rate.
    // For 20ms frames: required_samples = actual_input_rate * 0.020
    if (global_audio[encChn]->imp_audio->format == IMPAudioFormat::OPUS) {
        // Calculate samples for 20ms at the actual input sample rate
        targetSamplesPerChannel = global_audio[encChn]->imp_audio->sample_rate * 0.020;
        frameBuffer.clear();
        // Compute buffer bounds (configurable via cfg->audio.* if provided)
        int warnFrames = 3;
        int capFrames  = 5;
        // Optional config overrides
        if (cfg && cfg->config_loaded) {
            warnFrames = std::max(1, cfg->audio.buffer_warn_frames);
            capFrames  = std::max(warnFrames + 1, cfg->audio.buffer_cap_frames);
        }
        warnBufferSamplesPerChannel = targetSamplesPerChannel * warnFrames;
        maxBufferSamplesPerChannel  = targetSamplesPerChannel * capFrames;
        LOG_DEBUG("Opus frame accumulator initialized: target=" << targetSamplesPerChannel
                 << " samples per channel (20ms at " << global_audio[encChn]->imp_audio->sample_rate << "Hz), "
                 << "warn@" << warnBufferSamplesPerChannel << ", cap@" << maxBufferSamplesPerChannel);
        // Expose initial metrics and thresholds
        {
            std::string streamName = std::string("audio") + std::to_string(encChn);
            RTSPStatus::writeCustomParameter(streamName, "buffer_warn_samples_per_channel", std::to_string(warnBufferSamplesPerChannel));
            RTSPStatus::writeCustomParameter(streamName, "buffer_cap_samples_per_channel", std::to_string(maxBufferSamplesPerChannel));
            RTSPStatus::writeCustomParameter(streamName, "buffer_drop_count", std::to_string(bufferDropCount.load()));
        }
    }

    while (global_audio[encChn]->running)
    {
        const bool has_subscribers =
            global_audio[encChn]->streamCore->subscriberCount() > 0;
        if (has_subscribers && !had_subscribers)
        {
            sampleTimelineUs = 0;
            bufferStartTimestamp = 0;
            frameBuffer.clear();
            reset_reframer();
            global_audio[encChn]->last_timestamp_us.store(0, std::memory_order_relaxed);
            global_audio[encChn]->timestamp_origin_raw.store(0, std::memory_order_relaxed);
            global_audio[encChn]->presentation_origin_us.store(0, std::memory_order_relaxed);
        }
        had_subscribers = has_subscribers;

        if (cfg->audio.input_enabled)
        {
            if (IMP_AI_PollingFrame(global_audio[encChn]->devId,
                                    global_audio[encChn]->aiChn,
                                    cfg->general.imp_polling_timeout)
                == 0)
            {
                IMPAudioFrame frame;
                if (IMP_AI_GetFrame(global_audio[encChn]->devId,
                                    global_audio[encChn]->aiChn,
                                    &frame,
                                    IMPBlock::BLOCK)
                    != 0)
                {
                    LOG_ERROR("IMP_AI_GetFrame(" << global_audio[encChn]->devId << ", "
                                                 << global_audio[encChn]->aiChn << ") failed");
                }

                // Hardware timestamps are already monotonic (from IMP_System_RebaseTimeStamp)
                // Use them directly - no conversion needed with 64-bit approach
                // DeepSeek's recommendation: use hardware timestamps as-is since they're already monotonic

                if (has_subscribers && reframer)
                {
                    try
                    {
                        reframer->addFrame(reinterpret_cast<uint8_t *>(frame.virAddr),
                                           static_cast<int64_t>(normalize_audio_timestamp_us(frame.timeStamp)));
                        while (reframer->hasMoreFrames())
                        {
                            size_t frameLen = 1024 * sizeof(uint16_t)
                                              * global_audio[encChn]->imp_audio->outChnCnt;
                            std::vector<uint8_t> frameData(frameLen, 0);
                            int64_t audio_ts;
                            reframer->getReframedFrame(frameData.data(), audio_ts);
                            IMPAudioFrame reframed = {.bitwidth = frame.bitwidth,
                                                      .soundmode = frame.soundmode,
                                                      .virAddr = reinterpret_cast<uint32_t *>(
                                                          frameData.data()),
                                                      .phyAddr = frame.phyAddr,
                                                      .timeStamp = audio_ts,
                                                      .seq = frame.seq,
                                                      .len = static_cast<int>(frameLen)};
                            process_frame(reframed);
                        }
                    }
                    catch (const std::exception &e)
                    {
                        LOG_WARN("Audio reframer reset after exception: " << e.what());
                        reset_reframer();
                    }
                }
                else if (has_subscribers)
                {
                    process_frame(frame);
                }

                if (IMP_AI_ReleaseFrame(global_audio[encChn]->devId,
                                        global_audio[encChn]->aiChn,
                                        &frame)
                    < 0)
                {
                    LOG_ERROR("IMP_AI_ReleaseFrame(" << global_audio[encChn]->devId << ", "
                                                     << global_audio[encChn]->aiChn
                                                     << ", &frame) failed");
                }
            }
            else
            {
                LOG_DEBUG(global_audio[encChn]->devId << ", " << global_audio[encChn]->aiChn
                                                      << " POLLING TIMEOUT");
            }
        }
        else
        {
            /* to prevent clogging on startup or while restarting the threads
             * we wait for 250ms
            */
            usleep(250 * 1000);
        }
    }
}

void *AudioWorker::thread_entry(void *arg)
{
    StartHelper *sh = static_cast<StartHelper *>(arg);
    int encChn = sh->encChn;

    LOG_DEBUG("Start audio_grabber thread for device "
              << global_audio[encChn]->devId << " and channel " << global_audio[encChn]->aiChn
              << " and encoder " << global_audio[encChn]->aeChn);

    try {
        global_audio[encChn]->imp_audio = IMPAudio::createNew(global_audio[encChn]->devId,
                                                              global_audio[encChn]->aiChn,
                                                              global_audio[encChn]->aeChn);
        // Keep global devId in sync with actual initialized device (IMPAudio may adjust devId on some platforms)
        if (global_audio[encChn]->imp_audio && global_audio[encChn]->devId != global_audio[encChn]->imp_audio->devId) {
            LOG_INFO("AudioWorker: remapping devId from " << global_audio[encChn]->devId
                     << " to " << global_audio[encChn]->imp_audio->devId << " based on hardware init");
            global_audio[encChn]->devId = global_audio[encChn]->imp_audio->devId;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to initialize audio: " << e.what());
        global_audio[encChn]->imp_audio = nullptr;
        global_audio[encChn]->running = false;
        // inform main that initialization is complete (even though it failed)
        sh->has_started.release();
        return nullptr;
    }

    // inform main that initialization is complete
    sh->has_started.release();

    /* 'active' indicates, the thread is activly polling and grabbing images
     * 'running' describes the runlevel of the thread, if this value is set to false
     *           the thread exits and cleanup all ressources
     */
    global_audio[encChn]->active = true;
    global_audio[encChn]->running = true;
    global_audio[encChn]->last_timestamp_us.store(0, std::memory_order_relaxed);
    global_audio[encChn]->timestamp_origin_raw.store(0, std::memory_order_relaxed);
    global_audio[encChn]->presentation_origin_us.store(0, std::memory_order_relaxed);

    AudioWorker worker(encChn);
    worker.run();

    if (global_audio[encChn]->imp_audio)
    {
        delete global_audio[encChn]->imp_audio;
    }

    return 0;
}

#endif // AUDIO_SUPPORT
