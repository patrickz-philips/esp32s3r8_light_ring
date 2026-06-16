# ESP32-S3R8 Light Ring WLED-Style Starter

This project creates a WLED-style ESP-IDF application for the `esp32s3r8` platform.
It is structured like the local `lvgl_ppt` project, but the runtime is focused on a 27-pixel light ring connected to GPIO16.

## Hardware

- Target: ESP32-S3R8
- LED type: WS2812 / NeoPixel compatible light ring
- LED count: 27
- Data pin: GPIO16
- Power: `VDD`, `GND`

## Current Feature Set

- ESP-IDF native project layout
- RMT-based WS2812 output without external managed components
- Failsafe AP mode and optional STA mode
- Web UI at `/`
- WLED-style control endpoints:
  - `GET /json/info`
  - `GET /json/state`
  - `POST /json/state`
  - `GET /win?T=1&A=160&R=255&G=80&B=0&FX=2&SX=128&FP=4`
- Built-in effects: solid, breathe, rainbow, chase, color_wipe, twinkle, scanner, sparkle
- WLED-style palette selection via JSON `pal` / `palette` and legacy `FP` query parameter
- OTA-ready partition table

## Configuration

Project defaults live in `main/Kconfig.projbuild`.

- `LIGHT_RING_LED_GPIO`: defaults to `16`
- `LIGHT_RING_LED_COUNT`: defaults to `27`
- `LIGHT_RING_AP_PASSWORD`: fallback AP password
- `LIGHT_RING_STA_SSID` / `LIGHT_RING_STA_PASSWORD`: optional station credentials

If `LIGHT_RING_STA_SSID` is empty, the device boots in AP-only mode.

## Build

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

## API Notes

`POST /json/state` accepts both a simple payload and a WLED-like segment payload.

Supported string effect names: `solid`, `breathe`, `rainbow`, `chase`, `color_wipe`, `twinkle`, `scanner`, `sparkle`.

Built-in palette IDs:

- `0`: Primary
- `1`: Party
- `2`: Cloud
- `3`: Lava
- `4`: Ocean
- `5`: Forest
- `6`: Sunset
- `7`: Fire
- `8`: Ice
- `9`: Rainbow

When `pal` / `palette` is `0`, the current primary color (`color` or `seg[0].col[0]`) is used instead of a built-in palette.

Simple example:

```json
{
  "on": true,
  "bri": 180,
  "effect": "solid",
  "speed": 120,
  "palette": 6,
  "color": [255, 100, 10]
}
```

WLED-style example:

```json
{
  "on": true,
  "bri": 200,
  "seg": [
    {
      "fx": 3,
      "sx": 180,
      "pal": 4,
      "col": [[0, 180, 255]]
    }
  ]
}
```

## Scope

This is a clean ESP-IDF starter for a future WLED port on `esp32s3r8`.
It does not attempt to duplicate the full upstream WLED feature surface in one step, but it preserves the core control model and extension points.