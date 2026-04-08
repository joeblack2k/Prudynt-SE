# Prudynt-SE

**Prudynt-SE** is a native Thingino-oriented Ingenic camera streamer based on the original **[prudynt-v3](https://git.i386.io/wyze/prudynt-v3)** lineage and the `prudynt-t` codebase, with a rewritten stream core focused on stable dual-stream RTSP, clean timestamps, and multi-client fanout on real hardware.

## Based On

Prudynt-SE is directly based on:

- `prudynt-v3` as the historical upstream lineage
- `prudynt-t` as the immediate codebase foundation for this repo

This repository is not a clean-room rewrite from scratch. It is a focused continuation of `prudynt-t`, with the stream distribution layer, RTSP startup behavior, and deployment baseline pushed forward into a new known-good branch of work.

## What Changed From prudynt-t

Compared with the `prudynt-t` baseline, Prudynt-SE currently includes:

- a rewritten stream core built around producer/fanout semantics instead of fragile shared single-consumer queue behavior
- stabilized native `ch0` and `ch1` handling for real dual-stream camera operation
- RTSP startup fixes for clean `RTP-Info` and clean initial `ffprobe`/`ffmpeg` timestamps
- AAC startup timestamp fixes so audio does not begin with negative DTS/PTS offsets
- cleaner multi-client startup and connect/disconnect behavior under stress
- a validated reboot-proof service path on the physical camera used for milestone testing
- updated public branding, README, tag, and release baseline under the Prudynt-SE name

## What This Repo Is

Prudynt-SE keeps the camera-first architecture:

- Native `ch0` and `ch1`
- Native audio, snapshots, JPEG, OSD, motion, and backchannel
- Native RTSP behavior for direct clients such as VLC, ffmpeg, Scrypted, HomeKit bridges, and NVRs
- No fallback dependency on go2rtc, plugin-side workarounds, or top-layer transcoding as the primary fix

## Known-Good Baseline

The current baseline was validated on a physical camera with the M2A, M2B, and M2C milestones completed:

- `ffprobe` on `ch0` and `ch1` starts with clean `video=0` and `audio=0`
- `ffmpeg` runs for 20 seconds on both channels over TCP and UDP without timestamp or decoder warnings
- VLC plays both channels for 20+ seconds with steadily increasing playback time
- repeated connect/disconnect and mixed multi-client stress completed cleanly
- reboot proof completed: the camera comes back with Prudynt-SE started automatically and streams available without manual intervention

## Hardware Basis

Prudynt-SE is intended as a drop-in replacement for the same concrete camera models and board targets currently listed on the Thingino supported hardware page:

[Thingino supported hardware list](https://thingino.com/)

Important compatibility rule:

- the exact SoC, image sensor, Wi-Fi module, Ethernet presence, and flash size must match the Thingino-supported hardware entry for your device
- the same retail model name can ship with different internals, so model name alone is not enough

### Supported camera and board targets

Indoor IP cameras:

- 360 AP1PA3
- AJCloud T-CP2011-W32A
- AJCloud T-CP8010TF-W3M
- Aobocam A12
- ATOM Cam 1
- ATOM Cam 2
- Cinnado D1
- Edison WK1MPx
- Eufy C120 (T8400X)
- Eufy E220 (T8410C/X)
- eLife ET-N3431H-DW
- Galayou G2
- Galayou G7 2K
- GNCC GC2
- GNCC P5
- Hualai HL-JDPAN01
- iFlytek XFP301-M
- Imou Ranger 2
- Jooan A6M
- Jooan C9TS
- Jooan Q3H
- LongPlus X07
- LSC 3215672
- Mercusys MC200
- Neos SmartCam 2
- Xiaomi HL-CAM04
- Xiaomi Xiaofang iSC5
- Xiaomi SXJ02ZM
- Xiaomi MJSXJ03HL
- ZTE K540

Bulb IP cameras:

- AJCloud T-CP8040LF-W3M
- Jooan T2R
- LaView L2
- Wansview G6

Outdoor IP cameras:

- Aoqee C1
- AOSU C5L
- Dekco DC5L
- Overtech OV-59WB
- Sonoff Outdoor B1P
- TP-Link Tapo C500
- Wansview W5
- Wansview W6
- Wansview W7
- Wyze Cam Floodlight 1
- Wyze Video Doorbell 1
- Xiaomi MJSXJ05HL
- XVIM IPCAM-100

IPC modules:

- Enzhi / Vanhua AK54
- Enzhi / Vanhua H33
- Enzhi / Vanhua L34
- Enzhi / Vanhua S37i
- Enzhi / Vanhua Z55
- Enzhi / Vanhua Z55I

Web cameras:

- Hamedal C20
- Wansview 106

Development boards:

- Teacup

### Conditionally supported targets

These are listed by Thingino as conditionally supported because secure boot may be present on tested units:

- Roku Indoor Camera SE
- Wyze Cam Pan 3
- Wyze Video Doorbell 2

For these, Prudynt-SE should be treated as conditional rather than guaranteed drop-in until the secure-boot state of the exact unit is confirmed.

### Not included in the drop-in list

- mystery-box models from the Thingino page are not treated here as reliable drop-in targets
- unsupported hardware on the Thingino page is not a Prudynt-SE target list

## Building

The most binary-compatible way to build Prudynt-SE is inside a Thingino build environment using `buildroot_dev.sh`.

You can also build with Docker:

```bash
git clone https://github.com/joeblack2k/Prudynt-SE.git
cd Prudynt-SE

git submodule update --init

docker build \
  --build-arg TARGET=T31 \
  --build-arg BUILD_TYPE=dynamic \
  -t prudynt-se-builder .

docker run --rm -v "$(pwd):/src" prudynt-se-builder
```

Build output is written to `bin/`.

## Runtime Notes

- The shipped stream core is designed for true producer/fanout behavior rather than shared single-consumer queues.
- The RTSP path includes live555 startup fixes for clean `RTP-Info`, AAC startup timestamps, and stable multi-client session startup.
- Current runtime identifiers and file paths still use `prudynt` internally where needed for compatibility with deployed camera service layouts.

## Contributing

Contributions to Prudynt-SE are welcome. If you are changing the RTSP path, please include validation notes for:

- `ffprobe`
- `ffmpeg`
- VLC
- multi-client behavior
- reboot behavior when the change touches service startup or deployment
