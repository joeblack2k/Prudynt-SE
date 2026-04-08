# Integration Configs

These files keep the external integrations intentionally small:

- `go2rtc.yaml`: only the native RTSP camera streams
- `frigate.yaml`: only the Frigate fields required for restream, record, detect, audio, and ONVIF
- `scrypted-onvif.md`: the exact ONVIF values to enter in Scrypted

They are meant to stay close to native camera behavior rather than layering extra transcoding or workaround settings on top.

## Native endpoints

- Main stream: `rtsp://<user>:<password>@<camera-ip>:554/ch0`
- Sub stream: `rtsp://<user>:<password>@<camera-ip>:554/ch1`
- Snapshot: `http://<camera-ip>/image.jpg`
- ONVIF service: `http://<camera-ip>/onvif/device_service`

## Stream assumptions

- `ch0`: main stream
- `ch1`: sub stream
- ONVIF motion events are expected from the native motion pipeline
