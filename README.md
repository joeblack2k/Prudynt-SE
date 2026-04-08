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

Prudynt-SE is based on the current supported hardware list published by Thingino:

[Thingino supported hardware list](https://thingino.com/)

That means this repo is intended for the same general camera and board families that Thingino targets today, including:

- Indoor cameras
- Outdoor cameras
- Doorbells
- Floodlights
- Body cameras
- Webcams
- Modules and development boards

Examples shown on the current Thingino list include camera families from Wyze, Xiaomi, Eufy, Wansview, Teckin, Sonoff, Aqara, Gwell, and OpenIPC-related boards/modules, alongside Ingenic development platforms such as T20, T21, T23, T30, T31, T40, and T41 based boards.

Support still depends on the exact SoC, sensor, flash layout, Wi-Fi chipset, and Thingino target profile matching a supported device on the Thingino side. If a camera is not represented by the current Thingino hardware list, treat Prudynt-SE support as experimental until verified.

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
