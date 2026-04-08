#ifndef IMPDeviceSource_hpp
#define IMPDeviceSource_hpp

#include "FramedSource.hh"
#include <optional>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <deque>
#include <cstdint>
#include <sys/time.h>
#include "globals.hpp"

template <typename FrameType, typename Stream>
class IMPDeviceSource : public FramedSource
{
public:
    static IMPDeviceSource *createNew(UsageEnvironment &env, int encChn, std::shared_ptr<Stream> stream, const char *name, bool eagerActivate = false, unsigned clientSessionId = 0);

    void on_data_available()
    {
        if (eventTriggerId != 0)
        {
            envir().taskScheduler().triggerEvent(eventTriggerId, this);
        }
    }
    IMPDeviceSource(UsageEnvironment &env, int encChn, std::shared_ptr<Stream> stream, const char *name, bool eagerActivate, unsigned clientSessionId);
    virtual ~IMPDeviceSource();

private:
    virtual void doGetNextFrame() override;
    virtual void doStopGettingFrames() override;
    static void deliverFrame0(void *clientData);
    void deliverFrame();
    void deinit();
    void setCaptureEnabled(bool enabled);
    void queueBootstrapFramesForTimestamp(const struct timeval &timestamp);
    uint64_t normalizePresentationTimeUs(uint64_t sourceFrameUs, uint64_t durationUs);
    int encChn;
    unsigned clientSessionId;
    std::shared_ptr<Stream> stream;
    std::string name;   // for printing
    EventTriggerId eventTriggerId;
    bool captureEnabled {false};
    bool eagerActivate {false};
    bool bootstrapPendingForFirstSync {false};
    uint64_t presentationAnchorUs {0};
    uint64_t lastSourceFrameUs {0};
    uint64_t lastPresentationFrameUs {0};
    unsigned debugFramesLogged {0};
    std::optional<typename StreamCore<FrameType>::Cursor> cursor;
    std::deque<FrameType> bootstrapFrames;
};

#endif
