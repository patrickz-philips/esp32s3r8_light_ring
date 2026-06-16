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
  - `GET /json/palettes`
  - `POST /json/palettes`
  - `GET /win?T=1&A=160&R=255&G=80&B=0&FX=2&SX=128&FP=4`
- Built-in effects: solid, breathe, rainbow, chase, color_wipe, twinkle, scanner, sparkle, swoosh
- WLED-style palette selection via JSON `pal` / `palette` and legacy `FP` query parameter
- Built-in palette gallery plus 8 custom palette slots with NVS persistence
- Visual custom palette editor in the embedded web UI, including new custom palette creation and custom naming
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

Supported string effect names: `solid`, `breathe`, `rainbow`, `chase`, `color_wipe`, `twinkle`, `scanner`, `sparkle`, `swoosh`.

The `swoosh` effect uses three palettes:

- `bgPal`: background palette, rendered constantly across the ring
- `leftPal`: palette used by the left-moving swoosh from LED 27 toward LED 14
- `rightPal`: palette used by the right-moving swoosh from LED 27 toward LED 13 following `27 -> 1 -> 2 -> 3 ...`
- `leftStops`: how many LEDs the left swoosh occupies
- `rightStops`: how many LEDs the right swoosh occupies

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

Custom palette IDs:

- `10`: Custom 1
- `11`: Custom 2
- `12`: Custom 3
- `13`: Custom 4
- `14`: Custom 5
- `15`: Custom 6
- `16`: Custom 7
- `17`: Custom 8

When `pal` / `palette` is `0`, the current primary color (`color` or `seg[0].col[0]`) is used instead of a built-in palette.

The embedded web UI now exposes a palette gallery and a custom editor. Built-in palettes are read-only. Custom palettes are stored as up to 27 WLED-style color stops in flash and restored on boot. Each custom palette can also enable `circle` mode so LED 27 wraps back to LED 1 for the final transition.
Unused custom slots stay hidden from the active palette picker until you create one with `New Custom Palette`.

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

Swoosh example:

```json
{
  "on": true,
  "bri": 200,
  "effect": "swoosh",
  "speed": 160,
  "bgPal": 2,
  "leftPal": 7,
  "rightPal": 8,
  "leftStops": 6,
  "rightStops": 5
}
```

Custom palette example:

`POST /json/palettes` accepts `id`, optional `name`, optional `circle`, and a palette definition in `stops`, `palette`, or `colors`.
Each stop can be either `[index, r, g, b]` or `[index, "#RRGGBB"]`.

```json
{
  "id": 10,
  "name": "Aurora",
  "circle": true,
  "stops": [
    [0, 8, 16, 40],
    [96, 0, 180, 120],
    [180, 120, 255, 80],
    [255, "#fff2c0"]
  ]
}
```

`GET /json/palettes` returns a WLED-inspired palette catalog with:

- `items`: palette cards for the UI, including `editable`, `colors`, custom `stops`, and the custom `circle` flag
- `selected`: currently active palette id
- `customStart` / `customCount`: custom palette slot range
- `p`: palette map keyed by palette id

## Scope

This is a clean ESP-IDF starter for a future WLED port on `esp32s3r8`.
It does not attempt to duplicate the full upstream WLED feature surface in one step, but it preserves the core control model and extension points.