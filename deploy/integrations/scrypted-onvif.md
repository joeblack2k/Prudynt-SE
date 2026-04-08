# Scrypted ONVIF Setup

Use the Scrypted ONVIF plugin as the primary integration for Prudynt-SE cameras.

## Minimal settings

- Host: `<camera-ip>`
- Port: `80`
- Username: `<user>`
- Password: `<password>`
- ONVIF service URL: `http://<camera-ip>/onvif/device_service`

## Expected streams

- Main stream: `rtsp://<user>:<password>@<camera-ip>:554/ch0`
- Sub stream: `rtsp://<user>:<password>@<camera-ip>:554/ch1`

## Motion

- ONVIF event support is expected.
- Motion topics are exposed from the camera's native motion pipeline.
- Do not add a synthetic motion shim in Scrypted unless you are intentionally overriding native motion behavior.
