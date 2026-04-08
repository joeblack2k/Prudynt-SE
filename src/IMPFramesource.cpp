#include "Logger.hpp"
#include "IMPFramesource.hpp"

#include <algorithm>

#define MODULE "IMP_FRAMESOURCE"

#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
#define IMPEncoderCHNAttr IMPEncoderChnAttr
#define IMPEncoderCHNStat IMPEncoderChnStat
#endif

namespace
{
const char *framesource_state_name(IMPFSChannelState state)
{
    switch (state)
    {
    case IMP_FSCHANNEL_STATE_CLOSE:
        return "close";
    case IMP_FSCHANNEL_STATE_OPEN:
        return "open";
    case IMP_FSCHANNEL_STATE_RUN:
        return "run";
    default:
        return "unknown";
    }
}

void log_framesource_state(const char *stage, int chnNr)
{
    IMPFSChannelState state = IMP_FSCHANNEL_STATE_CLOSE;
    const int ret = IMP_FrameSource_ChnStatQuery(chnNr, &state);
    if (ret == 0)
    {
        LOG_INFO("IMPFramesource state: chn=" << chnNr
                 << " stage=" << stage
                 << " state=" << framesource_state_name(state));
    }
    else
    {
        LOG_ERROR("IMP_FrameSource_ChnStatQuery(" << chnNr
                  << ") during " << stage << " = " << ret);
    }
}
}

IMPFramesource *IMPFramesource::createNew(
    _stream *stream,
    _sensor *sensor,
    int chnNr,
    int sourceChn)
{
    return new IMPFramesource(stream, sensor, chnNr, sourceChn);
}

int IMPFramesource::init()
{
    LOG_DEBUG("IMPFramesource::init()");

    int ret = 0, scale = 0;
    const bool use_ext_channel = (sourceChn != chnNr);

    IMPFSChnAttr chnAttr;
    memset(&chnAttr, 0, sizeof(IMPFSChnAttr));

    (void)IMP_FrameSource_GetChnAttr(chnNr, &chnAttr);

    // That's a great idea but it does not work as intended. Needs more investigation.
    //if ((sensor->width != stream->width) || (sensor->height != stream->height)) {
    //    scale = 1;
    //} else {
    //    scale = 0;
    //}
    scale = 1;

    // Set required base attributes
    chnAttr.picWidth = stream->width;
    chnAttr.picHeight = stream->height;
    chnAttr.pixFmt = PIX_FMT_NV12;
    chnAttr.outFrmRateNum = stream->fps;
    chnAttr.outFrmRateDen = 1;
    // Respect the configured EXT buffer depth. On this T31 camera, forcing the
    // virtual substream above one VB exhausts the ISP pool before fs2 can even
    // enable, so the allocator policy must stay camera-safe first.
    int configured_buffers = (stream->buffers > 0 ? stream->buffers : (use_ext_channel ? 1 : 2));
    if (use_ext_channel && configured_buffers < 2)
        configured_buffers = 2;
    if (!use_ext_channel && chnNr == 0 && configured_buffers > 3)
        configured_buffers = 3;
    if (!use_ext_channel && chnNr == 1 && configured_buffers < 2)
        configured_buffers = 2;
    chnAttr.nrVBs = configured_buffers;
    chnAttr.type = use_ext_channel ? FS_EXT_CHANNEL : FS_PHY_CHANNEL;

    chnAttr.crop.enable = 0;
    chnAttr.crop.top = 0;
    chnAttr.crop.left = 0;
    chnAttr.crop.width = sensor->width;
    chnAttr.crop.height = sensor->height;

#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
    // Initialize frame-crop (fcrop) explicitly (only available on newer platforms)
    chnAttr.fcrop.enable = 0;
    chnAttr.fcrop.top = 0;
    chnAttr.fcrop.left = 0;
    chnAttr.fcrop.width = chnAttr.picWidth;
    chnAttr.fcrop.height = chnAttr.picHeight;
#endif

    // Scaler config (only if dimensions differ)
    chnAttr.scaler.enable = scale ? 1 : 0;
    if (stream->rotation != 0) {
        chnAttr.scaler.outwidth = stream->height;
        chnAttr.scaler.outheight = stream->width;
        chnAttr.picWidth = stream->height;
        chnAttr.picHeight = stream->width;
// Breaks OSD
//        chnAttr.picWidth = stream->width;
//        chnAttr.picHeight = stream->height;
    } else {
        chnAttr.scaler.outwidth = stream->width;
        chnAttr.scaler.outheight = stream->height;
        chnAttr.picWidth = stream->width;
        chnAttr.picHeight = stream->height;
    }

    LOG_DEBUG("Channel " << chnNr << " configuration (post-attr):");
    LOG_DEBUG("  type=" << (use_ext_channel ? "ext" : "phy")
              << " source=" << sourceChn);
    LOG_DEBUG("  pic: " << chnAttr.picWidth << "x" << chnAttr.picHeight);
    LOG_DEBUG("  crop.enable=" << chnAttr.crop.enable << " crop=" << chnAttr.crop.width << "x" << chnAttr.crop.height);
    LOG_DEBUG("  scaler.enable=" << chnAttr.scaler.enable << " out=" << chnAttr.scaler.outwidth << "x" << chnAttr.scaler.outheight);
    LOG_DEBUG("  fps=" << chnAttr.outFrmRateNum << "/" << chnAttr.outFrmRateDen << " nrVBs=" << chnAttr.nrVBs << " pixFmt=" << chnAttr.pixFmt);

#if !defined(KERNEL_VERSION_4)
#if defined(PLATFORM_T31) && !defined(PLATFORM_C100)

    int rot_rotation = stream->rotation;
    int rot_height = stream->height;
    int rot_width = stream->width;

    // Set rotate before FS creation
    // IMP_Encoder_SetFisheyeEnableStatus(0, 1);
    // IMP_Encoder_SetFisheyeEnableStatus(1, 1);

    if (stream->rotation != 0) {
       ret = IMP_FrameSource_SetChnRotate(chnNr, rot_rotation, rot_height, rot_width);
       LOG_DEBUG_OR_ERROR(ret, "IMP_FrameSource_SetChnRotate(0, rotation, rot_height, rot_width)");
    }

#endif
#endif

    ret = IMP_FrameSource_CreateChn(chnNr, &chnAttr);
    LOG_DEBUG_OR_ERROR(ret, "IMP_FrameSource_CreateChn(" << chnNr << ", &chnAttr)");
    if (ret != 0)
        return ret;
    created = true;
    log_framesource_state("created", chnNr);

    ret = IMP_FrameSource_SetChnAttr(chnNr, &chnAttr);
    LOG_DEBUG_OR_ERROR(ret, "IMP_FrameSource_SetChnAttr(" << chnNr << ", &chnAttr)");

    if (use_ext_channel)
    {
        ret = IMP_FrameSource_SetSource(chnNr, sourceChn);
        LOG_DEBUG_OR_ERROR(ret, "IMP_FrameSource_SetSource(" << chnNr << ", " << sourceChn << ")");
    }

    ret = IMP_FrameSource_SetMaxDelay(chnNr, 0);
    LOG_DEBUG_OR_ERROR(ret, "IMP_FrameSource_SetMaxDelay(" << chnNr << ", 0)");

    ret = IMP_FrameSource_SetDelay(chnNr, 0);
    LOG_DEBUG_OR_ERROR(ret, "IMP_FrameSource_SetDelay(" << chnNr << ", 0)");

    int max_delay = -1;
    ret = IMP_FrameSource_GetMaxDelay(chnNr, &max_delay);
    LOG_DEBUG_OR_ERROR(ret, "IMP_FrameSource_GetMaxDelay(" << chnNr << ", &max_delay)");
    if (ret == 0)
        LOG_DEBUG("  max_delay=" << max_delay);

    int delay = -1;
    ret = IMP_FrameSource_GetDelay(chnNr, &delay);
    LOG_DEBUG_OR_ERROR(ret, "IMP_FrameSource_GetDelay(" << chnNr << ", &delay)");
    if (ret == 0)
        LOG_DEBUG("  delay=" << delay);

#if !defined(NO_FIFO)
    IMPFSChnFifoAttr fifo;
    ret = IMP_FrameSource_GetChnFifoAttr(chnNr, &fifo);
    LOG_DEBUG_OR_ERROR(ret, "IMP_FrameSource_GetChnFifoAttr(" << chnNr << ", &fifo)");

    fifo.maxdepth = 0;
    ret = IMP_FrameSource_SetChnFifoAttr(chnNr, &fifo);
    LOG_DEBUG_OR_ERROR(ret, "IMP_FrameSource_SetChnFifoAttr(" << chnNr << ", &fifo)");

    ret = IMP_FrameSource_SetFrameDepth(chnNr, 0);
    LOG_DEBUG_OR_ERROR(ret, "IMP_FrameSource_SetFrameDepth(" << chnNr << ", 0)");
#endif

    //ret = IMP_FrameSource_EnableChn(chnNr);
    //LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_FrameSource_EnableChn(" << chnNr << ")");

    LOG_INFO("IMPFramesource ready: chn=" << chnNr
             << " type=" << (use_ext_channel ? "ext" : "phy")
             << " source=" << sourceChn
             << " size=" << chnAttr.picWidth << "x" << chnAttr.picHeight
             << " out=" << chnAttr.scaler.outwidth << "x" << chnAttr.scaler.outheight
             << " fps=" << chnAttr.outFrmRateNum << "/" << chnAttr.outFrmRateDen
             << " nrVBs=" << chnAttr.nrVBs);

    return ret;
}

int IMPFramesource::enable()
{
    if (enabled)
        return 0;
    int ret;

    ret = IMP_FrameSource_EnableChn(chnNr);
    LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_FrameSource_EnableChn(" << chnNr << ")");
    if (ret == 0)
    {
        enabled = true;
        LOG_INFO("IMPFramesource enabled: chn=" << chnNr);
        log_framesource_state("enabled", chnNr);
    }

    return 0;
}

int IMPFramesource::disable()
{
    if (!enabled)
        return 0;
    int ret;

    ret = IMP_FrameSource_DisableChn(chnNr);
    LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_FrameSource_DisableChn(" << chnNr << ")");
    if (ret == 0)
        enabled = false;

    return 0;
}

int IMPFramesource::destroy()
{
    if (!created)
        return 0;
    int ret;

    ret = IMP_FrameSource_DestroyChn(chnNr);
    LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_FrameSource_DestroyChn(" << chnNr << ")");
    if (ret == 0)
        created = false;

    return 0;
}
