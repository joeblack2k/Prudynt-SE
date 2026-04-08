#include "AudioReframer.hpp"
#include <algorithm>
#include <stdexcept>

AudioReframer::AudioReframer(unsigned int inputSampleRate, unsigned int inputSamplesPerFrame, unsigned int outputSamplesPerFrame, unsigned int channelCount)
    : inputSampleRate(inputSampleRate),
      inputSamplesPerFrame(inputSamplesPerFrame),
      outputSamplesPerFrame(outputSamplesPerFrame),
      channelCount(channelCount),
      currentTimestamp(0),
      samplesAccumulated(0),
      buffer(2 * std::max(inputSamplesPerFrame, outputSamplesPerFrame) * channelCount * sizeof(uint16_t))
{
    if (inputSamplesPerFrame == 0 || outputSamplesPerFrame == 0 || channelCount == 0)
    {
        throw std::invalid_argument("Frame sample counts and channel count must be greater than zero.");
    }
}

void AudioReframer::addFrame(const uint8_t* frameData, int64_t timestamp)
{
    if (frameData == nullptr)
    {
        throw std::invalid_argument("Frame data cannot be null.");
    }

    size_t inputFrameSize = inputSamplesPerFrame * channelCount * sizeof(uint16_t);
    buffer.push(frameData, inputFrameSize);

    if (samplesAccumulated == 0)
    {
        currentTimestamp = timestamp; // Initialize timestamp with the first frame
    }

    samplesAccumulated += inputSamplesPerFrame;
}

void AudioReframer::getReframedFrame(uint8_t* frameData, int64_t& timestamp)
{
    if (!hasMoreFrames())
    {
        throw std::runtime_error("Insufficient samples to generate a reframed output.");
    }

    if (frameData == nullptr) {
        throw std::invalid_argument("Output frame cannot be null.");
    }

    size_t outputFrameSize = outputSamplesPerFrame * channelCount * sizeof(uint16_t);
    buffer.fetch(frameData, outputFrameSize);
    samplesAccumulated -= outputSamplesPerFrame;

    timestamp = currentTimestamp;
    currentTimestamp += (static_cast<int64_t>(outputSamplesPerFrame) * 1000000) / inputSampleRate;
}

bool AudioReframer::hasMoreFrames() const
{
    return samplesAccumulated >= outputSamplesPerFrame;
}
