#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "led_strip_encoder.h"

#define LED_STRIP_RESOLUTION_HZ     10000000U
#define LED_STRIP_MEM_BLOCK_SYMBOLS 128U
#define HTTP_RECV_BUFFER_SIZE       2048
#define AP_MAX_STA_CONNECTIONS      4
#define PALETTE_ENTRY_COUNT         16U
#define BUILTIN_PALETTE_COUNT       9U
#define CUSTOM_PALETTE_SLOT_COUNT   8U
#define CUSTOM_PALETTE_MAX_STOPS    27U
#define PALETTE_NAME_LENGTH         24U
#define CUSTOM_PALETTE_START_ID     (1U + BUILTIN_PALETTE_COUNT)
#define PALETTE_COUNT               (CUSTOM_PALETTE_START_ID + CUSTOM_PALETTE_SLOT_COUNT)
#define PALETTE_NVS_NAMESPACE       "palettes"
#define PALETTE_NVS_KEY             "catalog"
#define CUSTOM_PALETTE_CIRCULAR_FLAG 0x80U
#define CUSTOM_PALETTE_STOP_COUNT_MASK 0x7FU
#define SWOOSH_PATH_LENGTH          ((CONFIG_LIGHT_RING_LED_COUNT + 1U) / 2U)
#define SHRINK_BAR_COUNT            3U
#define SHRINK_SEGMENT_LENGTH       (CONFIG_LIGHT_RING_LED_COUNT / SHRINK_BAR_COUNT)
#define SHRINK_BAR_LENGTH           (SHRINK_SEGMENT_LENGTH)
#define ROTATION_CORNER_COUNT       3U
#define ROTATION_SEGMENT_LENGTH     (CONFIG_LIGHT_RING_LED_COUNT / ROTATION_CORNER_COUNT)
#define ROTATION_MIN_LENGTH         2U
#define ROTATION_MAX_LENGTH         (ROTATION_SEGMENT_LENGTH)
#define DNS_SERVER_PORT             53
#define DNS_PACKET_MAX_SIZE         512
#define STRINGIFY_HELPER(value)     #value
#define STRINGIFY(value)            STRINGIFY_HELPER(value)

typedef enum {
    EFFECT_SOLID = 0,
    EFFECT_BREATHE,
    EFFECT_RAINBOW,
    EFFECT_CHASE,
    EFFECT_COLOR_WIPE,
    EFFECT_TWINKLE,
    EFFECT_SCANNER,
    EFFECT_SPARKLE,
    EFFECT_SWOOSH,
    EFFECT_SWOOSH_REVERSE,
    EFFECT_SHRINK,
    EFFECT_ROTATION,
    EFFECT_COUNT,
} effect_id_t;

typedef struct {
    const char *name;
    uint8_t colors[PALETTE_ENTRY_COUNT][3];
} builtin_palette_t;

typedef struct {
    uint8_t index;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} palette_stop_t;

typedef struct {
    uint8_t stop_count;
    char name[PALETTE_NAME_LENGTH];
    palette_stop_t stops[CUSTOM_PALETTE_MAX_STOPS];
    uint8_t colors[PALETTE_ENTRY_COUNT][3];
} custom_palette_t;

typedef struct {
    bool on;
    bool paused;
    uint8_t brightness;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t effect;
    uint8_t speed;
    uint8_t palette;
    uint8_t swoosh_background_palette;
    uint8_t swoosh_left_palette;
    uint8_t swoosh_right_palette;
    uint8_t swoosh_left_stops;
    uint8_t swoosh_right_stops;
    uint8_t shrink_background_palette;
    uint8_t shrink_bar_palette;
    uint8_t shrink_length;
    uint8_t shrink_gap;
    uint8_t rotation_background_palette;
    uint8_t rotation_palette;
    uint8_t rotation_length;
    uint8_t rotation_gap;
    bool rotation_counterclockwise;
    bool has_palette_cache;
    uint8_t palette_stop_count;
    bool palette_circular;
    palette_stop_t palette_stops[CUSTOM_PALETTE_MAX_STOPS];
    char palette_label[PALETTE_NAME_LENGTH];
    uint8_t palette_cache[PALETTE_ENTRY_COUNT][3];
} light_state_t;

static const char *TAG = "light_ring_wled";

static SemaphoreHandle_t s_state_lock;
static httpd_handle_t s_http_server;
static rmt_channel_handle_t s_led_channel;
static rmt_encoder_handle_t s_led_encoder;
static uint8_t s_led_pixels[CONFIG_LIGHT_RING_LED_COUNT * 3];
static char s_device_name[32];
static char s_ap_ssid[33];
static char s_sta_ip[16];
static bool s_sta_connected;
static esp_ip4_addr_t s_ap_ip;
static custom_palette_t s_custom_palettes[CUSTOM_PALETTE_SLOT_COUNT];

// WLED-inspired fixed palettes sampled with a 16-entry blended lookup.
static const builtin_palette_t s_builtin_palettes[BUILTIN_PALETTE_COUNT] = {
    {
        .name = "Party",
        .colors = {
            {85, 0, 171}, {132, 0, 124}, {171, 0, 85}, {255, 0, 128},
            {255, 0, 0}, {255, 96, 0}, {255, 192, 0}, {192, 255, 0},
            {96, 255, 0}, {0, 255, 0}, {0, 255, 128}, {0, 255, 255},
            {0, 128, 255}, {0, 0, 255}, {96, 0, 255}, {192, 0, 255},
        },
    },
    {
        .name = "Cloud",
        .colors = {
            {16, 24, 48}, {32, 48, 80}, {48, 72, 112}, {72, 104, 144},
            {96, 136, 176}, {120, 168, 200}, {152, 200, 224}, {184, 224, 240},
            {216, 240, 248}, {240, 248, 255}, {255, 236, 224}, {255, 220, 200},
            {248, 208, 184}, {232, 196, 176}, {208, 180, 168}, {184, 160, 160},
        },
    },
    {
        .name = "Lava",
        .colors = {
            {0, 0, 0}, {32, 0, 0}, {64, 0, 0}, {96, 0, 0},
            {128, 8, 0}, {160, 24, 0}, {192, 48, 0}, {224, 72, 0},
            {255, 96, 0}, {255, 128, 0}, {255, 160, 0}, {255, 192, 0},
            {255, 224, 32}, {255, 240, 96}, {255, 255, 160}, {255, 255, 224},
        },
    },
    {
        .name = "Ocean",
        .colors = {
            {0, 8, 32}, {0, 16, 64}, {0, 32, 96}, {0, 48, 128},
            {0, 72, 160}, {0, 96, 192}, {0, 120, 224}, {0, 144, 255},
            {0, 176, 255}, {0, 208, 255}, {0, 232, 255}, {32, 248, 255},
            {96, 255, 255}, {144, 255, 240}, {192, 255, 224}, {224, 255, 240},
        },
    },
    {
        .name = "Forest",
        .colors = {
            {4, 16, 4}, {8, 32, 8}, {12, 48, 12}, {16, 64, 16},
            {24, 88, 20}, {32, 112, 24}, {48, 136, 24}, {64, 160, 24},
            {80, 176, 20}, {104, 192, 24}, {128, 208, 32}, {152, 192, 48},
            {112, 144, 32}, {80, 104, 24}, {48, 72, 16}, {24, 40, 8},
        },
    },
    {
        .name = "Sunset",
        .colors = {
            {20, 0, 32}, {48, 0, 64}, {80, 0, 96}, {112, 0, 128},
            {144, 16, 128}, {176, 32, 112}, {208, 48, 96}, {224, 72, 80},
            {240, 96, 64}, {255, 120, 48}, {255, 144, 40}, {255, 168, 56},
            {255, 192, 88}, {255, 216, 128}, {255, 232, 176}, {255, 244, 224},
        },
    },
    {
        .name = "Fire",
        .colors = {
            {0, 0, 0}, {16, 0, 0}, {48, 0, 0}, {96, 0, 0},
            {160, 0, 0}, {208, 24, 0}, {255, 48, 0}, {255, 72, 0},
            {255, 104, 0}, {255, 136, 0}, {255, 168, 0}, {255, 200, 24},
            {255, 224, 80}, {255, 240, 144}, {255, 248, 208}, {255, 255, 255},
        },
    },
    {
        .name = "Ice",
        .colors = {
            {0, 0, 24}, {0, 8, 48}, {0, 16, 80}, {0, 32, 112},
            {0, 48, 160}, {0, 72, 208}, {0, 104, 240}, {32, 136, 255},
            {64, 176, 255}, {96, 208, 255}, {128, 232, 255}, {160, 244, 255},
            {192, 248, 255}, {224, 252, 255}, {240, 255, 255}, {255, 255, 255},
        },
    },
    {
        .name = "Rainbow",
        .colors = {
            {255, 0, 0}, {255, 64, 0}, {255, 128, 0}, {255, 191, 0},
            {255, 255, 0}, {128, 255, 0}, {0, 255, 0}, {0, 255, 128},
            {0, 255, 255}, {0, 128, 255}, {0, 0, 255}, {96, 0, 255},
            {160, 0, 255}, {224, 0, 255}, {255, 0, 160}, {255, 0, 96},
        },
    },
};

static const custom_palette_t s_default_custom_palettes[CUSTOM_PALETTE_SLOT_COUNT] = {
    {
        .stop_count = 5,
        .name = "Cotton Candy",
        .stops = {
            {0, 48, 11, 94},
            {64, 114, 9, 183},
            {128, 255, 0, 110},
            {192, 255, 190, 11},
            {255, 255, 244, 214},
        },
    },
    {
        .stop_count = 5,
        .name = "Aurora Mist",
        .stops = {
            {0, 0, 24, 64},
            {64, 0, 104, 160},
            {128, 0, 208, 160},
            {192, 96, 255, 196},
            {255, 236, 255, 248},
        },
    },
    {
        .stop_count = 5,
        .name = "Ember Trail",
        .stops = {
            {0, 12, 4, 24},
            {64, 96, 8, 32},
            {128, 220, 44, 0},
            {192, 255, 140, 0},
            {255, 255, 236, 180},
        },
    },
    {
        .stop_count = 5,
        .name = "Neon Mint",
        .stops = {
            {0, 4, 8, 32},
            {64, 0, 72, 96},
            {128, 0, 255, 170},
            {192, 160, 255, 96},
            {255, 250, 255, 224},
        },
    },
};

static light_state_t s_light_state = {
    .on = true,
    .paused = false,
    .brightness = CONFIG_LIGHT_RING_DEFAULT_BRIGHTNESS,
    .red = 255,
    .green = 120,
    .blue = 16,
    .effect = EFFECT_SOLID,
    .speed = 128,
    .palette = 0,
    .swoosh_background_palette = 0,
    .swoosh_left_palette = 7,
    .swoosh_right_palette = 8,
    .swoosh_left_stops = 5,
    .swoosh_right_stops = 5,
    .shrink_background_palette = 0,
    .shrink_bar_palette = 7,
    .shrink_length = 4,
    .shrink_gap = 6,
    .rotation_background_palette = 0,
    .rotation_palette = 7,
    .rotation_length = 2,
    .rotation_gap = 6,
    .rotation_counterclockwise = false,
};

static uint8_t custom_palette_stop_count(const custom_palette_t *palette)
{
    return (uint8_t) (palette->stop_count & CUSTOM_PALETTE_STOP_COUNT_MASK);
}

static bool custom_palette_is_circular(const custom_palette_t *palette)
{
    return (palette->stop_count & CUSTOM_PALETTE_CIRCULAR_FLAG) != 0U;
}

static void custom_palette_set_stop_count(custom_palette_t *palette, uint8_t stop_count)
{
    palette->stop_count = (uint8_t) ((palette->stop_count & CUSTOM_PALETTE_CIRCULAR_FLAG) |
        (stop_count & CUSTOM_PALETTE_STOP_COUNT_MASK));
}

static void custom_palette_set_circular(custom_palette_t *palette, bool circular)
{
    if (circular) {
        palette->stop_count |= CUSTOM_PALETTE_CIRCULAR_FLAG;
    } else {
        palette->stop_count &= CUSTOM_PALETTE_STOP_COUNT_MASK;
    }
}

static void sort_palette_stops(palette_stop_t *stops, size_t count)
{
    for (size_t index = 1; index < count; ++index) {
        palette_stop_t current = stops[index];
        size_t offset = index;
        while (offset > 0U && stops[offset - 1U].index > current.index) {
            stops[offset] = stops[offset - 1U];
            --offset;
        }
        stops[offset] = current;
    }
}

static void sample_palette_stop_gradient(const palette_stop_t *left, const palette_stop_t *right,
                                         uint16_t span, uint16_t distance,
                                         uint8_t *red, uint8_t *green, uint8_t *blue)
{
    if (span == 0U) {
        *red = right->red;
        *green = right->green;
        *blue = right->blue;
        return;
    }

    uint16_t progress = (uint16_t) ((distance * 255U) / span);
    uint16_t remain = 255U - progress;

    *red = (uint8_t) ((((uint16_t) left->red * remain) + ((uint16_t) right->red * progress)) / 255U);
    *green = (uint8_t) ((((uint16_t) left->green * remain) + ((uint16_t) right->green * progress)) / 255U);
    *blue = (uint8_t) ((((uint16_t) left->blue * remain) + ((uint16_t) right->blue * progress)) / 255U);
}

static void sample_custom_palette_stops(const palette_stop_t *stops, size_t count, bool circular,
                                        uint8_t target, uint8_t *red, uint8_t *green, uint8_t *blue)
{
    if (count == 0U) {
        *red = 0U;
        *green = 0U;
        *blue = 0U;
        return;
    }

    if (count == 1U) {
        *red = stops[0].red;
        *green = stops[0].green;
        *blue = stops[0].blue;
        return;
    }

    if (!circular) {
        const palette_stop_t *left = &stops[0];
        const palette_stop_t *right = &stops[count - 1U];

        if (target <= left->index) {
            *red = left->red;
            *green = left->green;
            *blue = left->blue;
            return;
        }

        if (target >= right->index) {
            *red = right->red;
            *green = right->green;
            *blue = right->blue;
            return;
        }

        for (size_t stop = 1; stop < count; ++stop) {
            right = &stops[stop];
            if (target > right->index) {
                left = right;
                continue;
            }

            sample_palette_stop_gradient(left, right, (uint16_t) (right->index - left->index),
                                         (uint16_t) (target - left->index), red, green, blue);
            return;
        }

        *red = stops[count - 1U].red;
        *green = stops[count - 1U].green;
        *blue = stops[count - 1U].blue;
        return;
    }

    for (size_t stop = 0; stop < count; ++stop) {
        const palette_stop_t *left = &stops[stop];
        const palette_stop_t *right = &stops[(stop + 1U) % count];
        uint16_t span = (right->index >= left->index) ?
            (uint16_t) (right->index - left->index) :
            (uint16_t) (256U + right->index - left->index);
        uint16_t distance = (target >= left->index) ?
            (uint16_t) (target - left->index) :
            (uint16_t) (256U + target - left->index);

        if (distance <= span) {
            sample_palette_stop_gradient(left, right, span, distance, red, green, blue);
            return;
        }
    }

    *red = stops[count - 1U].red;
    *green = stops[count - 1U].green;
    *blue = stops[count - 1U].blue;
}

static void rebuild_custom_palette_colors(custom_palette_t *palette)
{
    uint8_t stop_count = custom_palette_stop_count(palette);

    if (stop_count == 0U) {
        memset(palette->colors, 0, sizeof(palette->colors));
        return;
    }

    if (stop_count > CUSTOM_PALETTE_MAX_STOPS) {
        stop_count = CUSTOM_PALETTE_MAX_STOPS;
        custom_palette_set_stop_count(palette, stop_count);
    }

    sort_palette_stops(palette->stops, stop_count);

    for (size_t entry = 0; entry < PALETTE_ENTRY_COUNT; ++entry) {
        uint8_t target = (entry == (PALETTE_ENTRY_COUNT - 1U)) ? 255U : (uint8_t) (entry << 4);
        sample_custom_palette_stops(palette->stops, stop_count, custom_palette_is_circular(palette), target,
                                    &palette->colors[entry][0], &palette->colors[entry][1], &palette->colors[entry][2]);
    }
}

static void load_default_custom_palettes(void)
{
    memcpy(s_custom_palettes, s_default_custom_palettes, sizeof(s_custom_palettes));
    for (size_t index = 0; index < CUSTOM_PALETTE_SLOT_COUNT; ++index) {
        rebuild_custom_palette_colors(&s_custom_palettes[index]);
    }
}

static esp_err_t load_custom_palettes_from_nvs(void)
{
    load_default_custom_palettes();

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(PALETTE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "open palette namespace failed");

    size_t size = sizeof(s_custom_palettes);
    ret = nvs_get_blob(handle, PALETTE_NVS_KEY, s_custom_palettes, &size);
    nvs_close(handle);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        load_default_custom_palettes();
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "read custom palettes failed");

    if (size > sizeof(s_custom_palettes) || (size % sizeof(custom_palette_t)) != 0U) {
        ESP_LOGW(TAG, "Ignoring stored palettes with unexpected size %u", (unsigned) size);
        load_default_custom_palettes();
        return ESP_OK;
    }

    for (size_t index = 0; index < CUSTOM_PALETTE_SLOT_COUNT; ++index) {
        if (s_custom_palettes[index].name[0] == '\0') {
            strlcpy(s_custom_palettes[index].name, s_default_custom_palettes[index].name, sizeof(s_custom_palettes[index].name));
        }
        rebuild_custom_palette_colors(&s_custom_palettes[index]);
    }

    return ESP_OK;
}

static esp_err_t save_custom_palettes_to_nvs(void)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(PALETTE_NVS_NAMESPACE, NVS_READWRITE, &handle), TAG, "open palette namespace failed");

    esp_err_t ret = nvs_set_blob(handle, PALETTE_NVS_KEY, s_custom_palettes, sizeof(s_custom_palettes));
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    return ret;
}

static const char INDEX_HTML[] =
    "<!doctype html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "  <meta charset=\"utf-8\">\n"
    "  <meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "  <title>Light Ring Control</title>\n"
    "  <style>\n"
    "    :root { color-scheme: light; --bg:#f6efe3; --card:rgba(255,250,243,0.88); --ink:#1f1b16; --accent:#d95f39; --accent-2:#0d7b77; --line:#dcc8b0; --muted:#6f6355; }\n"
    "    * { box-sizing:border-box; }\n"
    "    body { margin:0; font-family:'Segoe UI',sans-serif; background:radial-gradient(circle at top,#fffaf4, #f2e4d1 52%, #dfc9aa); color:var(--ink); }\n"
    "    main { max-width:1120px; margin:0 auto; padding:28px 18px 44px; }\n"
    "    .page { display:grid; gap:20px; grid-template-columns:minmax(0, 1.2fr) minmax(320px, 0.8fr); align-items:start; }\n"
    "    .stack { display:grid; gap:16px; }\n"
    "    .card { background:var(--card); backdrop-filter:blur(14px); border:1px solid var(--line); border-radius:24px; padding:24px; box-shadow:0 18px 50px rgba(70,40,20,0.12); }\n"
    "    h1 { margin:0 0 10px; font-size:clamp(28px,5vw,44px); }\n"
    "    h2 { margin:0 0 10px; font-size:20px; }\n"
    "    p { line-height:1.5; }\n"
    "    label { display:block; font-size:14px; margin-bottom:8px; text-transform:uppercase; letter-spacing:0.08em; }\n"
    "    input, select, button { width:100%; border-radius:14px; border:1px solid var(--line); padding:12px 14px; font-size:16px; }\n"
    "    input[type=range] { padding:0; }\n"
    "    input[type=color] { min-height:52px; padding:6px; }\n"
    "    input[type=checkbox] { width:20px; height:20px; min-height:20px; padding:0; accent-color:var(--accent); }\n"
    "    button { background:linear-gradient(135deg,var(--accent),#f08b54); color:white; border:none; font-weight:600; cursor:pointer; }\n"
    "    button.secondary { background:linear-gradient(135deg,var(--accent-2),#35a6a2); }\n"
    "    button.ghost { background:white; color:var(--ink); border:1px solid var(--line); }\n"
    "    button:disabled { opacity:0.55; cursor:not-allowed; }\n"
    "    .row { display:flex; gap:12px; align-items:center; }\n"
    "    .row > * { flex:1; }\n"
    "    .grid { display:grid; gap:16px; grid-template-columns:repeat(auto-fit,minmax(220px,1fr)); margin-top:20px; }\n"
    "    .pill { display:inline-flex; align-items:center; gap:8px; margin-top:12px; padding:8px 12px; border-radius:999px; background:#fff; border:1px solid var(--line); font-size:14px; }\n"
    "    code { background:#fff; padding:2px 6px; border-radius:8px; }\n"
    "    .note { margin:0; color:var(--muted); font-size:14px; }\n"
    "    .palette-grid { display:grid; gap:12px; grid-template-columns:repeat(auto-fit,minmax(150px,1fr)); }\n"
    "    .palette-card { position:relative; width:100%; padding:0; overflow:hidden; text-align:left; background:#fff; color:var(--ink); border:1px solid var(--line); border-radius:18px; transition:transform .18s ease, box-shadow .18s ease, border-color .18s ease; }\n"
    "    .palette-card:hover { transform:translateY(-2px); box-shadow:0 10px 24px rgba(40,24,12,0.12); }\n"
    "    .palette-card.active { border-color:var(--accent); box-shadow:0 0 0 2px rgba(217,95,57,0.18); }\n"
    "    .palette-card.editable::after { content:'Custom'; position:absolute; top:10px; right:10px; padding:4px 8px; border-radius:999px; background:rgba(255,255,255,0.92); color:var(--ink); font-size:11px; }\n"
    "    .palette-preview { min-height:68px; display:grid; grid-template-columns:repeat(16,1fr); overflow:hidden; border-radius:16px; }\n"
    "    .palette-preview > div { min-height:68px; }\n"
    "    .palette-meta { padding:12px 14px 14px; }\n"
    "    .palette-meta strong { display:block; font-size:15px; }\n"
    "    .palette-meta span { color:var(--muted); font-size:13px; }\n"
    "    .editor-shell { display:grid; gap:16px; }\n"
    "    .editor-options { align-items:stretch; }\n"
    "    .editor-options > .toggle-row { flex:0 0 220px; }\n"
    "    .editor-header { display:flex; gap:12px; align-items:center; justify-content:space-between; }\n"
    "    .editor-header > * { flex:1; }\n"
    "    .actions { display:flex; gap:10px; justify-content:flex-end; }\n"
    "    .actions button, .toolbar button { width:auto; }\n"
    "    .toolbar { display:flex; gap:10px; flex-wrap:wrap; }\n"
    "    .stop-list { display:grid; gap:10px; }\n"
    "    .stop-row { display:grid; gap:10px; grid-template-columns:88px minmax(0,1fr) auto; align-items:end; padding:12px; border:1px solid var(--line); border-radius:16px; background:rgba(255,255,255,0.76); }\n"
    "    .mini-label { font-size:12px; color:var(--muted); margin-bottom:6px; text-transform:uppercase; letter-spacing:0.08em; }\n"
    "    .color-chip { min-height:52px; border-radius:14px; border:1px solid var(--line); background:#fff; cursor:pointer; box-shadow:inset 0 0 0 1px rgba(255,255,255,0.45); }\n"
    "    dialog.color-dialog { border:none; border-radius:22px; padding:0; max-width:420px; width:min(92vw, 420px); box-shadow:0 24px 60px rgba(20,10,4,0.22); }\n"
    "    dialog.color-dialog::backdrop { background:rgba(20,10,4,0.32); backdrop-filter:blur(2px); }\n"
    "    .dialog-body { padding:22px; display:grid; gap:14px; background:#fffaf4; }\n"
    "    .dialog-grid { display:grid; gap:12px; grid-template-columns:repeat(3, minmax(0,1fr)); }\n"
    "    .dialog-grid .wide { grid-column:1 / -1; }\n"
    "    .dialog-actions { display:flex; gap:10px; justify-content:flex-end; }\n"
    "    .dialog-actions button { width:auto; }\n"
    "    .dialog-body input[type=number], .dialog-body input[type=text], .dialog-body input[type=color] { width:100%; }\n"
    "    .dialog-body input[type=text] { text-transform:uppercase; }\n"
    "    label.toggle-row { display:flex; align-items:center; gap:10px; margin:0; padding:14px 16px; border:1px solid var(--line); border-radius:16px; background:rgba(255,255,255,0.76); }\n"
    "    label.toggle-row span { font-size:14px; text-transform:uppercase; letter-spacing:0.08em; }\n"
    "    .stop-row button { width:auto; }\n"
    "    @media (max-width: 880px) { .page { grid-template-columns:1fr; } .actions { justify-content:flex-start; } }\n"
    "    @media (max-width: 640px) { .row.editor-options { flex-direction:column; } .editor-options > .toggle-row { flex:1 1 auto; } .stop-row { grid-template-columns:1fr; } .dialog-grid { grid-template-columns:1fr; } }\n"
    "  </style>\n"
    "</head>\n"
    "<body>\n"
    "  <main>\n"
    "    <div class=\"page\">\n"
    "      <div class=\"stack\">\n"
    "        <div class=\"card\">\n"
    "          <h1>ESP32-S3R8 Light Ring</h1>\n"
    "          <p>WLED-style ESP-IDF control surface with built-in and custom palettes for a 27-pixel ring on GPIO16.</p>\n"
    "          <div class=\"row\">\n"
    "            <button id=\"toggleBtn\">Toggle Power</button>\n"
    "            <button id=\"suspendBtn\" class=\"secondary\">Suspend</button>\n"
    "          </div>\n"
    "          <div class=\"grid\">\n"
    "            <div><label for=\"color\">Primary Color</label><input id=\"color\" type=\"color\" value=\"#ff7810\"></div>\n"
    "            <div><label for=\"brightness\">Brightness</label><input id=\"brightness\" type=\"range\" min=\"0\" max=\"255\" value=\"160\"></div>\n"
    "            <div><label for=\"effect\">Effect</label><select id=\"effect\"><option value=\"0\">Solid</option><option value=\"1\">Breathe</option><option value=\"2\">Rainbow</option><option value=\"3\">Chase</option><option value=\"4\">Color Wipe</option><option value=\"5\">Twinkle</option><option value=\"6\">Scanner</option><option value=\"7\">Sparkle</option><option value=\"8\">Swoosh</option><option value=\"9\">Swoosh (Reverse)</option><option value=\"10\">Shrink</option><option value=\"11\">Rotation</option></select></div>\n"
    "            <div id=\"paletteRow\"><label for=\"palette\">Palette</label><select id=\"palette\"></select></div>\n"
    "            <div><label for=\"speed\">Speed</label><input id=\"speed\" type=\"range\" min=\"1\" max=\"255\" value=\"128\"></div>\n"
    "            <div id=\"swooshBgRow\" style=\"display:none\"><label for=\"swooshBgPalette\">Background Palette</label><select id=\"swooshBgPalette\"></select></div>\n"
    "            <div id=\"swooshLeftRow\" style=\"display:none\"><label for=\"swooshLeftPalette\">Left Palette</label><select id=\"swooshLeftPalette\"></select></div>\n"
    "            <div id=\"swooshRightRow\" style=\"display:none\"><label for=\"swooshRightPalette\">Right Palette</label><select id=\"swooshRightPalette\"></select></div>\n"
    "            <div id=\"sideLengthRow\" style=\"display:none\"><label for=\"sidePaletteLength\">Side Palette Length</label><input id=\"sidePaletteLength\" type=\"number\" min=\"1\" max=\"14\" value=\"5\"></div>\n"
    "            <div id=\"shrinkBgRow\" style=\"display:none\"><label for=\"shrinkBgPalette\">Background Palette</label><select id=\"shrinkBgPalette\"></select></div>\n"
    "            <div id=\"shrinkBarRow\" style=\"display:none\"><label for=\"shrinkBarPalette\">Bar Palette</label><select id=\"shrinkBarPalette\"></select></div>\n"
    "            <div id=\"shrinkLenRow\" style=\"display:none\"><label for=\"shrinkLength\">Shrink Length</label><input id=\"shrinkLength\" type=\"number\" min=\"1\" max=\"9\" value=\"4\"></div>\n"
    "            <div id=\"shrinkGapRow\" style=\"display:none\"><label for=\"shrinkGap\">Shrink Gap</label><input id=\"shrinkGap\" type=\"range\" min=\"0\" max=\"60\" value=\"6\"></div>\n"
    "            <div id=\"rotationBgRow\" style=\"display:none\"><label for=\"rotationBgPalette\">Background Palette</label><select id=\"rotationBgPalette\"></select></div>\n"
    "            <div id=\"rotationPaletteRow\" style=\"display:none\"><label for=\"rotationPalette\">Rotation Palette</label><select id=\"rotationPalette\"></select></div>\n"
    "            <div id=\"rotationLenRow\" style=\"display:none\"><label for=\"rotationLength\">Rotation Length</label><input id=\"rotationLength\" type=\"number\" min=\"2\" max=\"9\" value=\"2\"></div>\n"
    "            <div id=\"rotationGapRow\" style=\"display:none\"><label for=\"rotationGap\">Rotation Gap</label><input id=\"rotationGap\" type=\"range\" min=\"0\" max=\"60\" value=\"6\"></div>\n"
    "            <div id=\"rotationCcwRow\" style=\"display:none\"><label for=\"rotationCcw\">Counterclockwise</label><input id=\"rotationCcw\" type=\"checkbox\"></div>\n"
    "          </div>\n"
    "          <div class=\"grid\">\n"
    "            <button id=\"applyBtn\">Apply State</button>\n"
    "            <button id=\"refreshBtn\" class=\"secondary\">Refresh State</button>\n"
    "          </div>\n"
    "          <p id=\"deviceInfo\" class=\"pill\">Loading device info...</p>\n"
    "          <p>API: <code>/json/state</code>, <code>/json/info</code>, <code>/json/palettes</code>, <code>/win</code></p>\n"
    "        </div>\n"
    "        <div class=\"card\">\n"
    "          <div class=\"editor-header\">\n"
    "            <div>\n"
    "              <h2>Palette Gallery</h2>\n"
    "              <p class=\"note\">Built-ins are read-only. Eight custom slots can be edited, saved to flash, and optionally wrapped as a ring.</p>\n"
    "            </div>\n"
    "            <div class=\"actions\">\n"
    "              <button id=\"newPaletteBtn\" type=\"button\">New Custom Palette</button>\n"
    "              <button id=\"reloadPalettesBtn\" class=\"ghost\" type=\"button\">Reload</button>\n"
    "            </div>\n"
    "          </div>\n"
    "          <div id=\"paletteGallery\" class=\"palette-grid\"></div>\n"
    "        </div>\n"
    "      </div>\n"
    "      <div class=\"stack\">\n"
    "        <div class=\"card\">\n"
    "          <h2>Custom Palette Editor</h2>\n"
    "          <p id=\"editorHint\" class=\"note\">Select a custom palette card to edit its color stops.</p>\n"
    "          <div class=\"editor-shell\">\n"
    "            <div class=\"row editor-options\">\n"
    "              <div>\n"
    "                <label for=\"customPaletteName\">Palette Name</label>\n"
    "                <input id=\"customPaletteName\" type=\"text\" maxlength=\"23\" placeholder=\"Custom palette name\">\n"
    "              </div>\n"
    "              <label class=\"toggle-row\" for=\"circlePaletteInput\"><input id=\"circlePaletteInput\" type=\"checkbox\"><span>Circle Palette</span></label>\n"
    "            </div>\n"
    "            <div id=\"editorPreview\" class=\"palette-preview\"></div>\n"
    "            <div class=\"toolbar\">\n"
    "              <button id=\"addStopBtn\" class=\"ghost\" type=\"button\">Add Stop</button>\n"
    "              <button id=\"savePaletteBtn\" type=\"button\">Save Palette</button>\n"
    "            </div>\n"
    "            <div id=\"stopList\" class=\"stop-list\"></div>\n"
    "          </div>\n"
    "        </div>\n"
    "      </div>\n"
    "    </div>\n"
    "    <dialog id=\"colorDialog\" class=\"color-dialog\">\n"
    "      <div class=\"dialog-body\">\n"
    "        <div>\n"
    "          <h2>Edit Stop Color</h2>\n"
    "          <p id=\"colorDialogHint\" class=\"note\">Tune this stop using the picker, HEX, or RGB values.</p>\n"
    "        </div>\n"
    "        <div class=\"dialog-grid\">\n"
    "          <div class=\"wide\"><label for=\"dialogColorPicker\">Color</label><input id=\"dialogColorPicker\" type=\"color\"></div>\n"
    "          <div class=\"wide\"><label for=\"dialogHexInput\">HEX</label><input id=\"dialogHexInput\" type=\"text\" maxlength=\"7\" placeholder=\"#RRGGBB\" spellcheck=\"false\"></div>\n"
    "          <div><label for=\"dialogRedInput\">R</label><input id=\"dialogRedInput\" type=\"number\" min=\"0\" max=\"255\"></div>\n"
    "          <div><label for=\"dialogGreenInput\">G</label><input id=\"dialogGreenInput\" type=\"number\" min=\"0\" max=\"255\"></div>\n"
    "          <div><label for=\"dialogBlueInput\">B</label><input id=\"dialogBlueInput\" type=\"number\" min=\"0\" max=\"255\"></div>\n"
    "        </div>\n"
    "        <div class=\"dialog-actions\">\n"
    "          <button id=\"dialogCancelBtn\" class=\"ghost\" type=\"button\">Cancel</button>\n"
    "          <button id=\"dialogApplyBtn\" type=\"button\">Apply Color</button>\n"
    "        </div>\n"
    "      </div>\n"
    "    </dialog>\n"
    "  </main>\n"
    "  <script>\n"
    "    const colorInput = document.getElementById('color');\n"
    "    const brightnessInput = document.getElementById('brightness');\n"
    "    const speedInput = document.getElementById('speed');\n"
    "    const effectInput = document.getElementById('effect');\n"
    "    const paletteInput = document.getElementById('palette');\n"
    "    const swooshBgPaletteInput = document.getElementById('swooshBgPalette');\n"
    "    const swooshLeftPaletteInput = document.getElementById('swooshLeftPalette');\n"
    "    const swooshRightPaletteInput = document.getElementById('swooshRightPalette');\n"
    "    const sideLengthInput = document.getElementById('sidePaletteLength');\n"
    "    const shrinkBgPaletteInput = document.getElementById('shrinkBgPalette');\n"
    "    const shrinkBarPaletteInput = document.getElementById('shrinkBarPalette');\n"
    "    const shrinkLenInput = document.getElementById('shrinkLength');\n"
    "    const shrinkGapInput = document.getElementById('shrinkGap');\n"
    "    const rotationBgPaletteInput = document.getElementById('rotationBgPalette');\n"
    "    const rotationPaletteInput = document.getElementById('rotationPalette');\n"
    "    const rotationLenInput = document.getElementById('rotationLength');\n"
    "    const rotationGapInput = document.getElementById('rotationGap');\n"
    "    const rotationCcwInput = document.getElementById('rotationCcw');\n"
    "    const paletteRow = document.getElementById('paletteRow');\n"
    "    const swooshBgRow = document.getElementById('swooshBgRow');\n"
    "    const swooshLeftRow = document.getElementById('swooshLeftRow');\n"
    "    const swooshRightRow = document.getElementById('swooshRightRow');\n"
    "    const sideLengthRow = document.getElementById('sideLengthRow');\n"
    "    const shrinkBgRow = document.getElementById('shrinkBgRow');\n"
    "    const shrinkBarRow = document.getElementById('shrinkBarRow');\n"
    "    const shrinkLenRow = document.getElementById('shrinkLenRow');\n"
    "    const shrinkGapRow = document.getElementById('shrinkGapRow');\n"
    "    const rotationBgRow = document.getElementById('rotationBgRow');\n"
    "    const rotationPaletteRow = document.getElementById('rotationPaletteRow');\n"
    "    const rotationLenRow = document.getElementById('rotationLenRow');\n"
    "    const rotationGapRow = document.getElementById('rotationGapRow');\n"
    "    const rotationCcwRow = document.getElementById('rotationCcwRow');\n"
    "    const suspendBtn = document.getElementById('suspendBtn');\n"
    "    const deviceInfo = document.getElementById('deviceInfo');\n"
    "    const paletteGallery = document.getElementById('paletteGallery');\n"
    "    const editorHint = document.getElementById('editorHint');\n"
    "    const customPaletteName = document.getElementById('customPaletteName');\n"
    "    const circlePaletteInput = document.getElementById('circlePaletteInput');\n"
    "    const stopList = document.getElementById('stopList');\n"
    "    const editorPreview = document.getElementById('editorPreview');\n"
    "    const addStopBtn = document.getElementById('addStopBtn');\n"
    "    const savePaletteBtn = document.getElementById('savePaletteBtn');\n"
    "    const newPaletteBtn = document.getElementById('newPaletteBtn');\n"
    "    const reloadPalettesBtn = document.getElementById('reloadPalettesBtn');\n"
    "    const colorDialog = document.getElementById('colorDialog');\n"
    "    const colorDialogHint = document.getElementById('colorDialogHint');\n"
    "    const dialogColorPicker = document.getElementById('dialogColorPicker');\n"
    "    const dialogHexInput = document.getElementById('dialogHexInput');\n"
    "    const dialogRedInput = document.getElementById('dialogRedInput');\n"
    "    const dialogGreenInput = document.getElementById('dialogGreenInput');\n"
    "    const dialogBlueInput = document.getElementById('dialogBlueInput');\n"
    "    const dialogCancelBtn = document.getElementById('dialogCancelBtn');\n"
    "    const dialogApplyBtn = document.getElementById('dialogApplyBtn');\n"
    "    let lastOn = true;\n"
    "    let lastPaused = false;\n"
    "    let paletteCatalog = [];\n"
    "    let selectedPaletteId = 0;\n"
    "    let editingPaletteId = null;\n"
    "    let editingStops = [];\n"
    "    let editingPaletteCircle = false;\n"
    "    let editingColorIndex = null;\n"
    "    const ledCount = " STRINGIFY(CONFIG_LIGHT_RING_LED_COUNT) ";\n"
    "    function rgbToHex(rgb) { return '#' + rgb.map(v => Number(v).toString(16).padStart(2, '0')).join(''); }\n"
    "    function hexToRgb(hex) { const value = hex.replace('#', ''); return [parseInt(value.slice(0,2),16), parseInt(value.slice(2,4),16), parseInt(value.slice(4,6),16)]; }\n"
    "    function normalizeHexInput(value) { const raw = String(value || '').trim().replace(/^#/, '').toUpperCase(); if (!/^[0-9A-F]{0,6}$/.test(raw)) return null; return '#' + raw; }\n"
    "    function isCompleteHex(value) { return /^#[0-9A-F]{6}$/.test(String(value || '').toUpperCase()); }\n"
    "    function clampLedIndex(value) { return Math.max(1, Math.min(ledCount, Number(value) || 1)); }\n"
    "    function paletteIndexToLedIndex(value, circular = editingPaletteCircle) { const clamped = Math.max(0, Math.min(255, Number(value) || 0)); if (ledCount <= 1) return 1; if (circular) return 1 + Math.floor((clamped * ledCount) / 256); return 1 + Math.round((clamped * (ledCount - 1)) / 255); }\n"
    "    function ledIndexToPaletteIndex(value, circular = editingPaletteCircle) { const clamped = clampLedIndex(value); if (ledCount <= 1) return 0; if (circular) return Math.floor(((clamped - 1) * 256) / ledCount); return Math.round(((clamped - 1) * 255) / (ledCount - 1)); }\n"
    "    function normalizeStops(stops) { return (Array.isArray(stops) ? stops : []).map(stop => ({ index: Number(stop[0] ?? stop.index ?? 0), rgb: Array.isArray(stop) ? [Number(stop[1] ?? 0), Number(stop[2] ?? 0), Number(stop[3] ?? 0)] : [Number(stop.red ?? 0), Number(stop.green ?? 0), Number(stop.blue ?? 0)] })).sort((a, b) => a.index - b.index); }\n"
    "    function seedPaletteStops() { return [{ index: 0, rgb: hexToRgb(colorInput ? colorInput.value : '#ff7810') }, { index: 255, rgb: [255, 255, 255] }]; }\n"
    "    function nextEmptyCustomPalette() { return paletteCatalog.find(item => item.editable && item.empty); }\n"
    "    function clampChannel(value) { return Math.max(0, Math.min(255, Number(value) || 0)); }\n"
    "    function sampleStopGradient(left, right, span, distance) { if (span <= 0) return [...right.rgb]; const progress = Math.max(0, Math.min(255, Math.round((distance * 255) / span))); const remain = 255 - progress; return [0, 1, 2].map(channel => Math.round(((left.rgb[channel] * remain) + (right.rgb[channel] * progress)) / 255)); }\n"
    "    function sampleStops(stops, circular, target) { if (!stops.length) return [0, 0, 0]; if (stops.length === 1) return [...stops[0].rgb]; if (!circular) { const first = stops[0]; const last = stops[stops.length - 1]; if (target <= first.index) return [...first.rgb]; if (target >= last.index) return [...last.rgb]; for (let index = 1; index < stops.length; index += 1) { const right = stops[index]; const left = stops[index - 1]; if (target > right.index) continue; return sampleStopGradient(left, right, right.index - left.index, target - left.index); } return [...last.rgb]; } for (let index = 0; index < stops.length; index += 1) { const left = stops[index]; const right = stops[(index + 1) % stops.length]; const span = right.index >= left.index ? (right.index - left.index) : (256 + right.index - left.index); const distance = target >= left.index ? (target - left.index) : (256 + target - left.index); if (distance <= span) return sampleStopGradient(left, right, span, distance); } return [...stops[stops.length - 1].rgb]; }\n"
    "    function previewColorsFromStops(stops, circular) { return Array.from({ length: 16 }, (_, entry) => { const target = entry === 15 ? 255 : (entry << 4); return [target, ...sampleStops(stops, circular, target)]; }); }\n"
    "    function syncDialogFromRgb(rgb) { const safe = [clampChannel(rgb[0]), clampChannel(rgb[1]), clampChannel(rgb[2])]; dialogColorPicker.value = rgbToHex(safe); dialogHexInput.value = rgbToHex(safe).toUpperCase(); dialogRedInput.value = String(safe[0]); dialogGreenInput.value = String(safe[1]); dialogBlueInput.value = String(safe[2]); }\n"
    "    function dialogRgb() { return [clampChannel(dialogRedInput.value), clampChannel(dialogGreenInput.value), clampChannel(dialogBlueInput.value)]; }\n"
    "    function openColorDialog(index) { if (Number.isNaN(index) || !editingStops[index] || !colorDialog) return; editingColorIndex = index; colorDialogHint.textContent = `Editing stop ${index + 1}. Use color, HEX, or RGB.`; syncDialogFromRgb(editingStops[index].rgb); if (typeof colorDialog.showModal === 'function') { colorDialog.showModal(); } }\n"
    "    function closeColorDialog() { if (colorDialog && colorDialog.open) colorDialog.close(); editingColorIndex = null; }\n"
    "    function applyDialogColor() { if (editingColorIndex === null || !editingStops[editingColorIndex]) { closeColorDialog(); return; } editingStops[editingColorIndex].rgb = dialogRgb(); renderPreview(editorPreview, editingStops.map(stop => [stop.index, ...stop.rgb])); renderStopEditor(); closeColorDialog(); }\n"
    "    function renderPreview(target, colors) { if (!target) return; target.innerHTML = ''; (colors || []).forEach(entry => { const swatch = document.createElement('div'); const rgb = Array.isArray(entry) ? entry.slice(1,4) : entry.rgb; swatch.style.background = `rgb(${rgb[0]}, ${rgb[1]}, ${rgb[2]})`; target.appendChild(swatch); }); }\n"
    "    function renderPaletteSelect() { if (!paletteInput || !swooshBgPaletteInput || !swooshLeftPaletteInput || !swooshRightPaletteInput) return; const current = String(selectedPaletteId); const options = paletteCatalog.filter(item => !item.empty).map(item => `<option value=\"${item.id}\">${item.name}</option>`).join(''); paletteInput.innerHTML = options; swooshBgPaletteInput.innerHTML = options; swooshLeftPaletteInput.innerHTML = options; swooshRightPaletteInput.innerHTML = options; if (shrinkBgPaletteInput) shrinkBgPaletteInput.innerHTML = options; if (shrinkBarPaletteInput) shrinkBarPaletteInput.innerHTML = options; if (rotationBgPaletteInput) rotationBgPaletteInput.innerHTML = options; if (rotationPaletteInput) rotationPaletteInput.innerHTML = options; if ([...paletteInput.options].some(option => option.value === current)) paletteInput.value = current; }\n"
    "    function activatePaletteCard(id) { if (!paletteGallery) return; [...paletteGallery.children].forEach(card => card.classList.toggle('active', Number(card.dataset.id) === Number(id))); }\n"
    "    function renderStopEditor() { if (!stopList || !editorPreview || !editorHint || !customPaletteName || !circlePaletteInput) return; stopList.innerHTML = ''; const current = paletteCatalog.find(item => Number(item.id) === Number(editingPaletteId)); if (!current) { editorHint.textContent = 'Select a custom palette card or create a new one.'; customPaletteName.value = ''; circlePaletteInput.checked = false; circlePaletteInput.disabled = true; renderPreview(editorPreview, []); return; } circlePaletteInput.disabled = false; circlePaletteInput.checked = editingPaletteCircle; const flowHint = editingPaletteCircle ? 'Circle mode wraps LED 27 back to LED 1.' : 'Linear mode clamps the two ends.'; editorHint.textContent = current.empty ? `Creating ${current.name}. Give it a name and tune its color stops. ${flowHint}` : `Editing custom palette ${current.name}. Stops are saved as WLED-style [index,r,g,b] entries. ${flowHint}`; if (!current.empty && customPaletteName.value === '') customPaletteName.value = current.name; const previewColors = previewColorsFromStops(editingStops, editingPaletteCircle); renderPreview(editorPreview, previewColors.length ? previewColors : (current.colors || [])); editingStops.forEach((stop, index) => { const row = document.createElement('div'); row.className = 'stop-row'; row.innerHTML = `<div><div class=\"mini-label\">Index</div><input type=\"number\" min=\"0\" max=\"255\" value=\"${stop.index}\" data-role=\"index\" data-index=\"${index}\"></div><div><div class=\"mini-label\">Color</div><button type=\"button\" class=\"color-chip\" style=\"background:${rgbToHex(stop.rgb)}\" data-role=\"edit-color\" data-index=\"${index}\" aria-label=\"Edit stop color ${index + 1}\"></button></div><button type=\"button\" class=\"ghost\" data-role=\"remove\" data-index=\"${index}\">Remove</button>`; stopList.appendChild(row); }); }\n"
    "    function renderPaletteGallery() { if (!paletteGallery) return; paletteGallery.innerHTML = ''; paletteCatalog.filter(item => !item.empty).forEach(item => { const card = document.createElement('button'); card.type = 'button'; card.className = `palette-card${item.editable ? ' editable' : ''}${Number(item.id) === Number(selectedPaletteId) ? ' active' : ''}`; card.dataset.id = item.id; const preview = document.createElement('div'); preview.className = 'palette-preview'; (item.colors || []).forEach(entry => { const swatch = document.createElement('div'); const rgb = Array.isArray(entry) ? entry.slice(1,4) : entry.rgb; swatch.style.background = `rgb(${rgb[0]}, ${rgb[1]}, ${rgb[2]})`; preview.appendChild(swatch); }); const meta = document.createElement('div'); meta.className = 'palette-meta'; meta.innerHTML = `<strong>${item.name}</strong><span>${item.editable ? 'Custom editable palette' : 'Built-in palette'}</span>`; card.append(preview, meta); card.addEventListener('click', () => { selectedPaletteId = Number(item.id); if (paletteInput) paletteInput.value = String(selectedPaletteId); activatePaletteCard(selectedPaletteId); if (item.editable) { loadPaletteEditor(item.id); } else { editingPaletteId = null; editingStops = []; renderStopEditor(); } }); paletteGallery.appendChild(card); }); if (newPaletteBtn) { const next = nextEmptyCustomPalette(); newPaletteBtn.disabled = !next; newPaletteBtn.textContent = next ? 'New Custom Palette' : 'Custom Slots Full'; } }\n"
    "    function loadPaletteEditor(id) { const item = paletteCatalog.find(entry => Number(entry.id) === Number(id)); if (!item || !item.editable) return; editingPaletteId = Number(id); customPaletteName.value = item.empty ? '' : item.name; editingPaletteCircle = !!(item.circle ?? item.circular ?? false); editingStops = item.empty ? seedPaletteStops() : normalizeStops(item.stops && item.stops.length ? item.stops : item.colors).slice(0, 27); renderStopEditor(); }\n"
    "    const baseRenderStopEditor = renderStopEditor;\n"
    "    renderStopEditor = function() { baseRenderStopEditor(); stopList.querySelectorAll('[data-role=index]').forEach(input => { input.dataset.role = 'index-led'; input.min = '1'; input.max = String(ledCount); input.step = '1'; input.value = String(clampLedIndex(paletteIndexToLedIndex(input.value, editingPaletteCircle))); const container = input.parentElement; const label = container ? container.querySelector('.mini-label') : null; if (label) label.textContent = 'LED'; }); };\n"
    "    function startNewPalette() { const item = nextEmptyCustomPalette(); if (!item) { editorHint.textContent = 'All custom palette slots are already in use.'; return; } loadPaletteEditor(item.id); }\n"
    "    function updateEffectControls() { const effectId = Number(effectInput.value); const isSwoosh = effectId === 8 || effectId === 9; const isShrink = effectId === 10; const isRotation = effectId === 11; paletteRow.style.display = (isSwoosh || isShrink || isRotation) ? 'none' : ''; swooshBgRow.style.display = isSwoosh ? '' : 'none'; swooshLeftRow.style.display = isSwoosh ? '' : 'none'; swooshRightRow.style.display = isSwoosh ? '' : 'none'; sideLengthRow.style.display = isSwoosh ? '' : 'none'; shrinkBgRow.style.display = isShrink ? '' : 'none'; shrinkBarRow.style.display = isShrink ? '' : 'none'; shrinkLenRow.style.display = isShrink ? '' : 'none'; shrinkGapRow.style.display = isShrink ? '' : 'none'; if (rotationBgRow) rotationBgRow.style.display = isRotation ? '' : 'none'; if (rotationPaletteRow) rotationPaletteRow.style.display = isRotation ? '' : 'none'; if (rotationLenRow) rotationLenRow.style.display = isRotation ? '' : 'none'; if (rotationGapRow) rotationGapRow.style.display = isRotation ? '' : 'none'; if (rotationCcwRow) rotationCcwRow.style.display = isRotation ? '' : 'none'; }\n"
    "    async function loadInfo() { const response = await fetch('/json/info'); const info = await response.json(); deviceInfo.textContent = `${info.name} | AP ${info.wifi.ap_ssid} | LEDs ${info.led.count} @ GPIO${info.led.gpio}`; }\n"
    "    async function loadPalettes() { const response = await fetch('/json/palettes'); const json = await response.json(); paletteCatalog = Array.isArray(json.items) ? json.items : []; renderPaletteSelect(); renderPaletteGallery(); if (editingPaletteId !== null) { loadPaletteEditor(editingPaletteId); } else { renderStopEditor(); } }\n"
    "    function updateSuspendButtonLabel() { suspendBtn.textContent = lastPaused ? 'Resume' : 'Suspend'; }\n"
    "    async function loadState() { const response = await fetch('/json/state'); const state = await response.json(); const seg = state.seg && state.seg[0] ? state.seg[0] : {}; const color = seg.col && seg.col[0] ? seg.col[0] : state.color; lastOn = !!state.on; lastPaused = !!state.paused; selectedPaletteId = Number(seg.pal ?? state.pal ?? state.palette ?? 0); updateSuspendButtonLabel(); brightnessInput.value = state.bri ?? 0; speedInput.value = seg.sx ?? state.speed ?? 128; effectInput.value = seg.fx ?? state.fx ?? 0; swooshBgPaletteInput.value = String(seg.bgPal ?? state.bgPal ?? 0); swooshLeftPaletteInput.value = String(seg.leftPal ?? state.leftPal ?? 0); swooshRightPaletteInput.value = String(seg.rightPal ?? state.rightPal ?? 0); sideLengthInput.value = String(seg.leftStops ?? state.leftStops ?? 5); if (shrinkBgPaletteInput) shrinkBgPaletteInput.value = String(seg.shrinkBg ?? state.shrinkBg ?? 0); if (shrinkBarPaletteInput) shrinkBarPaletteInput.value = String(seg.shrinkBar ?? state.shrinkBar ?? 0); if (shrinkLenInput) shrinkLenInput.value = String(seg.shrinkLen ?? state.shrinkLen ?? 4); if (shrinkGapInput) shrinkGapInput.value = String(seg.shrinkGap ?? state.shrinkGap ?? 6); if (rotationBgPaletteInput) rotationBgPaletteInput.value = String(seg.rotBg ?? state.rotBg ?? 0); if (rotationPaletteInput) rotationPaletteInput.value = String(seg.rotPal ?? state.rotPal ?? 0); if (rotationLenInput) rotationLenInput.value = String(seg.rotLen ?? state.rotLen ?? 2); if (rotationGapInput) rotationGapInput.value = String(seg.rotGap ?? state.rotGap ?? 6); if (rotationCcwInput) rotationCcwInput.checked = !!(seg.rotCcw ?? state.rotCcw ?? false); updateEffectControls(); if (paletteInput) paletteInput.value = String(selectedPaletteId); activatePaletteCard(selectedPaletteId); const selected = paletteCatalog.find(item => Number(item.id) === Number(selectedPaletteId)); if (selected && selected.editable) { if (editingPaletteId !== selected.id) loadPaletteEditor(selected.id); } else { editingPaletteId = null; editingStops = []; renderStopEditor(); } if (Array.isArray(color)) { colorInput.value = rgbToHex(color); } }\n"
    "    async function applyState() { const [r, g, b] = hexToRgb(colorInput.value); const payload = { on: true, paused: false, bri: Number(brightnessInput.value), color: [r, g, b], effect: Number(effectInput.value), speed: Number(speedInput.value), palette: Number(paletteInput.value), bgPal: Number(swooshBgPaletteInput.value), leftPal: Number(swooshLeftPaletteInput.value), rightPal: Number(swooshRightPaletteInput.value), leftStops: Number(sideLengthInput.value), rightStops: Number(sideLengthInput.value), shrinkBg: Number(shrinkBgPaletteInput.value), shrinkBar: Number(shrinkBarPaletteInput.value), shrinkLen: Number(shrinkLenInput.value), shrinkGap: Number(shrinkGapInput.value), rotBg: Number(rotationBgPaletteInput.value), rotPal: Number(rotationPaletteInput.value), rotLen: Number(rotationLenInput.value), rotGap: Number(rotationGapInput.value), rotCcw: !!rotationCcwInput.checked }; await fetch('/json/state', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) }); await loadState(); }\n"
    "    async function savePalette() { if (editingPaletteId === null) { editorHint.textContent = 'Select a custom palette before saving.'; return; } if (!editingStops.length) { editorHint.textContent = 'Add at least one color stop before saving.'; return; } const payload = { id: Number(editingPaletteId), name: customPaletteName.value.trim() || 'Custom Palette', circle: !!circlePaletteInput.checked, stops: editingStops.map(stop => [Number(stop.index), ...stop.rgb]) }; await fetch('/json/palettes', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) }); await loadPalettes(); await loadState(); loadPaletteEditor(editingPaletteId); }\n"
    "    document.getElementById('applyBtn').addEventListener('click', applyState);\n"
    "    document.getElementById('refreshBtn').addEventListener('click', loadState);\n"
    "    document.getElementById('toggleBtn').addEventListener('click', async () => { await fetch(`/win?T=${lastOn ? 0 : 1}`); await loadState(); });\n"
    "    suspendBtn.addEventListener('click', async () => { await fetch('/json/state', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ paused: !lastPaused }) }); await loadState(); });\n"
    "    effectInput.addEventListener('change', updateEffectControls);\n"
    "    paletteInput.addEventListener('change', () => { selectedPaletteId = Number(paletteInput.value); activatePaletteCard(selectedPaletteId); const selected = paletteCatalog.find(item => Number(item.id) === Number(selectedPaletteId)); if (selected && selected.editable) { loadPaletteEditor(selected.id); } else { editingPaletteId = null; editingStops = []; renderStopEditor(); } });\n"
    "    stopList.addEventListener('input', event => { const target = event.target; const index = Number(target.dataset.index); if (Number.isNaN(index) || !editingStops[index]) return; if (target.dataset.role === 'index') { editingStops[index].index = Math.max(0, Math.min(255, Number(target.value) || 0)); editingStops.sort((a, b) => a.index - b.index); renderStopEditor(); } });\n"
    "    stopList.addEventListener('input', event => { const target = event.target; if (target.dataset.role !== 'index-led') return; const index = Number(target.dataset.index); if (Number.isNaN(index) || !editingStops[index]) return; const ledIndex = clampLedIndex(target.value); editingStops[index].index = ledIndexToPaletteIndex(ledIndex, editingPaletteCircle); renderPreview(editorPreview, previewColorsFromStops(editingStops, editingPaletteCircle)); });\n"
    "    stopList.addEventListener('change', event => { const target = event.target; if (target.dataset.role !== 'index-led') return; const index = Number(target.dataset.index); if (Number.isNaN(index) || !editingStops[index]) return; const ledIndex = clampLedIndex(target.value); target.value = String(ledIndex); editingStops[index].index = ledIndexToPaletteIndex(ledIndex, editingPaletteCircle); editingStops.sort((a, b) => a.index - b.index); renderStopEditor(); });\n"
    "    stopList.addEventListener('click', event => { const target = event.target; if (target.dataset.role === 'edit-color') { const index = Number(target.dataset.index); if (!Number.isNaN(index)) openColorDialog(index); return; } if (target.dataset.role !== 'remove') return; const index = Number(target.dataset.index); if (!Number.isNaN(index)) { editingStops.splice(index, 1); renderStopEditor(); } });\n"
    "    addStopBtn.addEventListener('click', () => { if (editingPaletteId === null) { editorHint.textContent = 'Select a custom palette first.'; return; } if (editingStops.length >= 27) return; editingStops.push({ index: editingStops.length ? Math.min(255, editingStops[editingStops.length - 1].index + 32) : 0, rgb: [255, 255, 255] }); renderStopEditor(); });\n"
    "    circlePaletteInput.addEventListener('change', () => { editingPaletteCircle = !!circlePaletteInput.checked; renderStopEditor(); });\n"
    "    savePaletteBtn.addEventListener('click', savePalette);\n"
    "    newPaletteBtn.addEventListener('click', startNewPalette);\n"
    "    dialogCancelBtn.addEventListener('click', closeColorDialog);\n"
    "    dialogApplyBtn.addEventListener('click', applyDialogColor);\n"
    "    dialogColorPicker.addEventListener('input', () => syncDialogFromRgb(hexToRgb(dialogColorPicker.value)));\n"
    "    dialogHexInput.addEventListener('input', () => { const normalized = normalizeHexInput(dialogHexInput.value); if (normalized === null) return; dialogHexInput.value = normalized.toUpperCase(); if (!isCompleteHex(normalized)) return; syncDialogFromRgb(hexToRgb(normalized)); });\n"
    "    [dialogRedInput, dialogGreenInput, dialogBlueInput].forEach(input => input.addEventListener('input', () => syncDialogFromRgb(dialogRgb())));\n"
    "    if (colorDialog) colorDialog.addEventListener('cancel', event => { event.preventDefault(); closeColorDialog(); });\n"
    "    reloadPalettesBtn.addEventListener('click', () => loadPalettes().then(loadState).catch(error => { deviceInfo.textContent = error.message; }));\n"
    "    Promise.all([loadInfo(), loadPalettes()]).then(loadState).catch(error => { deviceInfo.textContent = error.message; });\n"
    "  </script>\n"
    "</body>\n"
    "</html>\n";

static const char *effect_name(effect_id_t effect)
{
    switch (effect) {
    case EFFECT_SOLID:
        return "solid";
    case EFFECT_BREATHE:
        return "breathe";
    case EFFECT_RAINBOW:
        return "rainbow";
    case EFFECT_CHASE:
        return "chase";
    case EFFECT_COLOR_WIPE:
        return "color_wipe";
    case EFFECT_TWINKLE:
        return "twinkle";
    case EFFECT_SCANNER:
        return "scanner";
    case EFFECT_SPARKLE:
        return "sparkle";
    case EFFECT_SWOOSH:
        return "swoosh";
    case EFFECT_SWOOSH_REVERSE:
        return "swoosh_reverse";
    case EFFECT_SHRINK:
        return "shrink";
    case EFFECT_ROTATION:
        return "rotation";
    default:
        return "solid";
    }
}

static const char *palette_name(uint8_t palette)
{
    if (palette == 0U) {
        return "Primary";
    }
    if (palette >= CUSTOM_PALETTE_START_ID && palette < PALETTE_COUNT) {
        return s_custom_palettes[palette - CUSTOM_PALETTE_START_ID].name[0] != '\0' ?
            s_custom_palettes[palette - CUSTOM_PALETTE_START_ID].name : "Custom Palette";
    }
    if (palette > BUILTIN_PALETTE_COUNT) {
        return "Primary";
    }
    return s_builtin_palettes[palette - 1U].name;
}

static bool is_custom_palette_id(uint8_t palette)
{
    return palette >= CUSTOM_PALETTE_START_ID && palette < PALETTE_COUNT;
}

static bool custom_palette_is_empty(const custom_palette_t *palette)
{
    return custom_palette_stop_count(palette) == 0U && palette->name[0] == '\0';
}

static size_t custom_palette_slot_from_id(uint8_t palette)
{
    return (size_t) (palette - CUSTOM_PALETTE_START_ID);
}

static uint8_t clamp_u8(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (uint8_t) value;
}

static uint8_t clamp_palette(int value)
{
    if (value < 0) {
        return 0U;
    }
    if (value >= (int) PALETTE_COUNT) {
        return (uint8_t) (PALETTE_COUNT - 1U);
    }
    return (uint8_t) value;
}

static effect_id_t clamp_effect(int value)
{
    if (value < 0) {
        return EFFECT_SOLID;
    }
    if (value >= EFFECT_COUNT) {
        return (effect_id_t) (EFFECT_COUNT - 1);
    }
    return (effect_id_t) value;
}

static effect_id_t parse_effect_name(const char *value)
{
    if (value == NULL) {
        return EFFECT_SOLID;
    }
    if (strcasecmp(value, "solid") == 0) {
        return EFFECT_SOLID;
    }
    if (strcasecmp(value, "breathe") == 0) {
        return EFFECT_BREATHE;
    }
    if (strcasecmp(value, "rainbow") == 0) {
        return EFFECT_RAINBOW;
    }
    if (strcasecmp(value, "chase") == 0) {
        return EFFECT_CHASE;
    }
    if (strcasecmp(value, "color_wipe") == 0 || strcasecmp(value, "color wipe") == 0 ||
        strcasecmp(value, "wipe") == 0) {
        return EFFECT_COLOR_WIPE;
    }
    if (strcasecmp(value, "twinkle") == 0) {
        return EFFECT_TWINKLE;
    }
    if (strcasecmp(value, "scanner") == 0 || strcasecmp(value, "larson") == 0) {
        return EFFECT_SCANNER;
    }
    if (strcasecmp(value, "sparkle") == 0 || strcasecmp(value, "glitter") == 0) {
        return EFFECT_SPARKLE;
    }
    if (strcasecmp(value, "swoosh") == 0) {
        return EFFECT_SWOOSH;
    }
    if (strcasecmp(value, "swoosh_reverse") == 0 || strcasecmp(value, "swoosh reverse") == 0 ||
        strcasecmp(value, "swoosh(reverse)") == 0 || strcasecmp(value, "reverse_swoosh") == 0) {
        return EFFECT_SWOOSH_REVERSE;
    }
    if (strcasecmp(value, "shrink") == 0) {
        return EFFECT_SHRINK;
    }
    if (strcasecmp(value, "rotation") == 0 || strcasecmp(value, "rotate") == 0) {
        return EFFECT_ROTATION;
    }
    return clamp_effect(atoi(value));
}

static uint8_t scale_component(uint8_t value, uint8_t brightness)
{
    return (uint8_t) (((uint16_t) value * (uint16_t) brightness) / 255U);
}

static uint8_t clamp_swoosh_span(int value)
{
    if (value < 1) {
        return 1U;
    }
    if (value > (int) SWOOSH_PATH_LENGTH) {
        return SWOOSH_PATH_LENGTH;
    }
    return (uint8_t) value;
}

static uint8_t clamp_shrink_length(int value)
{
    if (value < 1) {
        return 1U;
    }
    if (value > (int) SHRINK_BAR_LENGTH) {
        return (uint8_t) SHRINK_BAR_LENGTH;
    }
    return (uint8_t) value;
}

static uint8_t clamp_shrink_gap(int value)
{
    if (value < 0) {
        return 0U;
    }
    if (value > 60) {
        return 60U;
    }
    return (uint8_t) value;
}

static uint8_t clamp_rotation_length(int value)
{
    if (value < (int) ROTATION_MIN_LENGTH) {
        return (uint8_t) ROTATION_MIN_LENGTH;
    }
    if (value > (int) ROTATION_MAX_LENGTH) {
        return (uint8_t) ROTATION_MAX_LENGTH;
    }
    return (uint8_t) value;
}

static uint8_t clamp_rotation_gap(int value)
{
    if (value < 0) {
        return 0U;
    }
    if (value > 60) {
        return 60U;
    }
    return (uint8_t) value;
}

static void hsv_to_rgb(uint16_t hue, uint8_t saturation, uint8_t value,
                       uint8_t *red, uint8_t *green, uint8_t *blue)
{
    uint8_t region = (uint8_t) ((hue % 360U) / 60U);
    uint16_t remainder = (uint16_t) (((hue % 360U) - (region * 60U)) * 255U / 60U);
    uint8_t p = (uint8_t) (((uint16_t) value * (255U - saturation)) / 255U);
    uint8_t q = (uint8_t) (((uint16_t) value * (255U - ((uint16_t) saturation * remainder) / 255U)) / 255U);
    uint8_t t = (uint8_t) (((uint16_t) value * (255U - ((uint16_t) saturation * (255U - remainder)) / 255U)) / 255U);

    switch (region) {
    case 0:
        *red = value;
        *green = t;
        *blue = p;
        break;
    case 1:
        *red = q;
        *green = value;
        *blue = p;
        break;
    case 2:
        *red = p;
        *green = value;
        *blue = t;
        break;
    case 3:
        *red = p;
        *green = q;
        *blue = value;
        break;
    case 4:
        *red = t;
        *green = p;
        *blue = value;
        break;
    default:
        *red = value;
        *green = p;
        *blue = q;
        break;
    }
}

static void set_pixel_rgb(uint16_t index, uint8_t red, uint8_t green, uint8_t blue)
{
    size_t offset = index * 3U;
    s_led_pixels[offset + 0] = red;
    s_led_pixels[offset + 1] = green;
    s_led_pixels[offset + 2] = blue;
}

static void clear_pixels(void)
{
    memset(s_led_pixels, 0, sizeof(s_led_pixels));
}

static void fill_pixels_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    for (uint16_t index = 0; index < CONFIG_LIGHT_RING_LED_COUNT; ++index) {
        set_pixel_rgb(index, red, green, blue);
    }
}

static void sample_palette_entries(const uint8_t colors[PALETTE_ENTRY_COUNT][3], uint8_t index,
                                   bool wrap, uint8_t *red, uint8_t *green, uint8_t *blue)
{
    uint8_t hi4 = (uint8_t) (index >> 4);
    uint8_t lo4 = (uint8_t) (index & 0x0FU);
    const uint8_t *from = colors[hi4];

    if (lo4 == 0U) {
        *red = from[0];
        *green = from[1];
        *blue = from[2];
        return;
    }

    const uint8_t *to = wrap ? colors[(hi4 + 1U) & 0x0FU] : colors[(hi4 < (PALETTE_ENTRY_COUNT - 1U)) ? (hi4 + 1U) : hi4];
    uint16_t blend_to = ((uint16_t) lo4) << 4;
    uint16_t blend_from = 256U - blend_to;

    *red = (uint8_t) ((((uint16_t) from[0] * blend_from) + ((uint16_t) to[0] * blend_to)) >> 8);
    *green = (uint8_t) ((((uint16_t) from[1] * blend_from) + ((uint16_t) to[1] * blend_to)) >> 8);
    *blue = (uint8_t) ((((uint16_t) from[2] * blend_from) + ((uint16_t) to[2] * blend_to)) >> 8);
}

static uint8_t led_index_to_palette_index_circular(uint16_t index)
{
    return (uint8_t) ((((uint32_t) index) * 256U) / CONFIG_LIGHT_RING_LED_COUNT);
}

static uint8_t led_index_to_palette_index_linear(uint16_t index)
{
    if (CONFIG_LIGHT_RING_LED_COUNT <= 1U) {
        return 0U;
    }

    return (uint8_t) ((((uint32_t) index) * 255U + ((CONFIG_LIGHT_RING_LED_COUNT - 1U) / 2U)) /
        (CONFIG_LIGHT_RING_LED_COUNT - 1U));
}

static uint8_t state_led_index_to_palette_index(uint16_t index, const light_state_t *state)
{
    if (state->palette_stop_count > 0U && !state->palette_circular) {
        return led_index_to_palette_index_linear(index);
    }

    return led_index_to_palette_index_circular(index);
}

static void sample_builtin_palette(uint8_t palette, uint8_t index,
                                   uint8_t *red, uint8_t *green, uint8_t *blue)
{
    uint8_t palette_id = palette;
    if (palette_id == 0U) {
        palette_id = 1U;
    } else if (palette_id > BUILTIN_PALETTE_COUNT) {
        palette_id = BUILTIN_PALETTE_COUNT;
    }

    sample_palette_entries(s_builtin_palettes[palette_id - 1U].colors, index, true, red, green, blue);
}

static void resolve_state_rgb(const light_state_t *state, uint8_t palette_index, uint8_t brightness,
                              uint8_t *red, uint8_t *green, uint8_t *blue)
{
    if (state->palette == 0U) {
        *red = scale_component(state->red, brightness);
        *green = scale_component(state->green, brightness);
        *blue = scale_component(state->blue, brightness);
        return;
    }

    if (state->palette_stop_count > 0U) {
        uint8_t cached_red;
        uint8_t cached_green;
        uint8_t cached_blue;
        sample_custom_palette_stops(state->palette_stops, state->palette_stop_count, state->palette_circular,
                                    palette_index, &cached_red, &cached_green, &cached_blue);
        *red = scale_component(cached_red, brightness);
        *green = scale_component(cached_green, brightness);
        *blue = scale_component(cached_blue, brightness);
        return;
    }

    if (state->has_palette_cache) {
        uint8_t cached_red;
        uint8_t cached_green;
        uint8_t cached_blue;
        sample_palette_entries(state->palette_cache, palette_index, false, &cached_red, &cached_green, &cached_blue);
        *red = scale_component(cached_red, brightness);
        *green = scale_component(cached_green, brightness);
        *blue = scale_component(cached_blue, brightness);
        return;
    }

    uint8_t base_red;
    uint8_t base_green;
    uint8_t base_blue;
    sample_builtin_palette(state->palette, palette_index, &base_red, &base_green, &base_blue);
    *red = scale_component(base_red, brightness);
    *green = scale_component(base_green, brightness);
    *blue = scale_component(base_blue, brightness);
}

static void set_state_palette_level(uint16_t index, const light_state_t *state,
                                    uint8_t palette_index, uint8_t level)
{
    uint8_t pixel_brightness = scale_component(state->brightness, level);
    uint8_t red;
    uint8_t green;
    uint8_t blue;

    resolve_state_rgb(state, palette_index, pixel_brightness, &red, &green, &blue);
    set_pixel_rgb(index, red, green, blue);
}

static void set_state_pixel_level(uint16_t index, const light_state_t *state, uint8_t level)
{
    set_state_palette_level(index, state, state_led_index_to_palette_index(index, state), level);
}

static uint32_t speed_to_delay(uint8_t speed, uint32_t min_delay, uint32_t max_delay)
{
    return max_delay - (((uint32_t) speed * (max_delay - min_delay)) / 255U);
}

static uint8_t ease8_in_out(uint8_t value)
{
    uint32_t linear = value;
    uint32_t square = (linear * linear + 127U) / 255U;
    uint32_t cube = (square * linear + 127U) / 255U;
    uint32_t eased = (3U * square) - (2U * cube);
    return clamp_u8((int) eased);
}

static uint32_t hash32(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    value ^= value >> 16;
    return value;
}

static uint16_t abs_distance_u16(uint16_t left, uint16_t right)
{
    return (left > right) ? (left - right) : (right - left);
}

static void snapshot_light_state(light_state_t *state)
{
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    *state = s_light_state;

    state->has_palette_cache = false;
    state->palette_stop_count = 0U;
    state->palette_circular = false;
    memset(state->palette_stops, 0, sizeof(state->palette_stops));
    memset(state->palette_cache, 0, sizeof(state->palette_cache));
    memset(state->palette_label, 0, sizeof(state->palette_label));

    if (is_custom_palette_id(state->palette)) {
        size_t slot = custom_palette_slot_from_id(state->palette);
        state->palette_stop_count = custom_palette_stop_count(&s_custom_palettes[slot]);
        state->palette_circular = custom_palette_is_circular(&s_custom_palettes[slot]);
        memcpy(state->palette_stops, s_custom_palettes[slot].stops, sizeof(state->palette_stops));
        memcpy(state->palette_cache, s_custom_palettes[slot].colors, sizeof(state->palette_cache));
        strlcpy(state->palette_label, s_custom_palettes[slot].name, sizeof(state->palette_label));
        state->has_palette_cache = state->palette_stop_count > 0U;
    } else {
        strlcpy(state->palette_label, palette_name(state->palette), sizeof(state->palette_label));
    }

    xSemaphoreGive(s_state_lock);
}

static void store_light_state(const light_state_t *state)
{
    light_state_t sanitized = *state;
    sanitized.has_palette_cache = false;
    sanitized.palette_stop_count = 0U;
    sanitized.palette_circular = false;
    memset(sanitized.palette_stops, 0, sizeof(sanitized.palette_stops));
    memset(sanitized.palette_cache, 0, sizeof(sanitized.palette_cache));
    memset(sanitized.palette_label, 0, sizeof(sanitized.palette_label));

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_light_state = sanitized;
    xSemaphoreGive(s_state_lock);
}

static uint32_t effect_delay_ms(const light_state_t *state)
{
    switch (state->effect) {
    case EFFECT_SOLID:
        return 100U;
    case EFFECT_BREATHE:
        return speed_to_delay(state->speed, 18U, 42U);
    case EFFECT_RAINBOW:
        return speed_to_delay(state->speed, 12U, 34U);
    case EFFECT_CHASE:
        return speed_to_delay(state->speed, 18U, 50U);
    case EFFECT_COLOR_WIPE:
        return speed_to_delay(state->speed, 24U, 82U);
    case EFFECT_TWINKLE:
        return speed_to_delay(state->speed, 28U, 60U);
    case EFFECT_SCANNER:
        return speed_to_delay(state->speed, 16U, 46U);
    case EFFECT_SPARKLE:
        return speed_to_delay(state->speed, 26U, 58U);
    case EFFECT_SWOOSH:
    case EFFECT_SWOOSH_REVERSE:
        return speed_to_delay(state->speed, 40U, 180U);
    case EFFECT_SHRINK:
        return speed_to_delay(state->speed, 30U, 360U);
    case EFFECT_ROTATION:
        return speed_to_delay(state->speed, 30U, 360U);
    default:
        return 40U;
    }
}

static void render_solid(const light_state_t *state)
{
    if (state->palette != 0U) {
        for (uint16_t index = 0; index < CONFIG_LIGHT_RING_LED_COUNT; ++index) {
            set_state_pixel_level(index, state, 255U);
        }
        return;
    }

    uint8_t red = scale_component(state->red, state->brightness);
    uint8_t green = scale_component(state->green, state->brightness);
    uint8_t blue = scale_component(state->blue, state->brightness);

    fill_pixels_rgb(red, green, blue);
}

static void render_breathe(const light_state_t *state, uint32_t frame)
{
    const uint32_t inhale_frames = 40U;
    const uint32_t hold_high_frames = 10U;
    const uint32_t exhale_frames = 78U;
    const uint32_t hold_low_frames = 32U;
    const uint32_t cycle_frames = inhale_frames + hold_high_frames + exhale_frames + hold_low_frames;
    uint32_t phase = (frame * (1U + ((uint32_t) state->speed / 96U))) % cycle_frames;
    uint8_t breath_level = 0U;

    if (phase < inhale_frames) {
        breath_level = ease8_in_out((uint8_t) ((phase * 255U) / (inhale_frames - 1U)));
    } else if (phase < (inhale_frames + hold_high_frames)) {
        breath_level = 255U;
    } else if (phase < (inhale_frames + hold_high_frames + exhale_frames)) {
        uint32_t exhale_phase = phase - inhale_frames - hold_high_frames;
        breath_level = (uint8_t) (255U - ease8_in_out((uint8_t) ((exhale_phase * 255U) / (exhale_frames - 1U))));
    }

    uint8_t level = (uint8_t) (6U + (((uint32_t) breath_level * 249U) / 255U));
    for (uint16_t index = 0; index < CONFIG_LIGHT_RING_LED_COUNT; ++index) {
        set_state_pixel_level(index, state, level);
    }
}

static void render_rainbow(const light_state_t *state, uint32_t frame)
{
    if (state->palette != 0U) {
        uint8_t offset = (uint8_t) ((frame * (1U + ((uint32_t) state->speed / 28U)) * 4U) & 0xFFU);
        for (uint16_t index = 0; index < CONFIG_LIGHT_RING_LED_COUNT; ++index) {
            uint8_t palette_index = (uint8_t) (offset + state_led_index_to_palette_index(index, state));
            set_state_palette_level(index, state, palette_index, 255U);
        }
        return;
    }

    uint16_t offset = (uint16_t) ((frame * (1U + ((uint32_t) state->speed / 28U)) * 3U) % 360U);
    for (uint16_t index = 0; index < CONFIG_LIGHT_RING_LED_COUNT; ++index) {
        uint16_t hue = (uint16_t) ((offset + (index * 360U / CONFIG_LIGHT_RING_LED_COUNT)) % 360U);
        uint8_t red;
        uint8_t green;
        uint8_t blue;
        hsv_to_rgb(hue, 255, state->brightness, &red, &green, &blue);
        set_pixel_rgb(index, red, green, blue);
    }
}

static void render_chase(const light_state_t *state, uint32_t frame)
{
    uint16_t head = (uint16_t) ((frame * (1U + ((uint32_t) state->speed / 40U))) % CONFIG_LIGHT_RING_LED_COUNT);

    for (uint16_t index = 0; index < CONFIG_LIGHT_RING_LED_COUNT; ++index) {
        set_state_pixel_level(index, state, 12U);
    }

    set_state_pixel_level(head, state, 255U);
    set_state_pixel_level((uint16_t) ((head + CONFIG_LIGHT_RING_LED_COUNT - 1U) % CONFIG_LIGHT_RING_LED_COUNT), state, 144U);
    set_state_pixel_level((uint16_t) ((head + 1U) % CONFIG_LIGHT_RING_LED_COUNT), state, 144U);
    set_state_pixel_level((uint16_t) ((head + CONFIG_LIGHT_RING_LED_COUNT - 2U) % CONFIG_LIGHT_RING_LED_COUNT), state, 72U);
    set_state_pixel_level((uint16_t) ((head + 2U) % CONFIG_LIGHT_RING_LED_COUNT), state, 72U);
}

static void render_color_wipe(const light_state_t *state, uint32_t frame)
{
    uint32_t step = frame * (1U + ((uint32_t) state->speed / 48U));
    uint16_t cycle = CONFIG_LIGHT_RING_LED_COUNT * 2U;
    uint16_t progress = (uint16_t) (step % cycle);
    uint16_t lit_count = (progress < CONFIG_LIGHT_RING_LED_COUNT)
                             ? (progress + 1U)
                             : (CONFIG_LIGHT_RING_LED_COUNT - (progress - CONFIG_LIGHT_RING_LED_COUNT));

    clear_pixels();
    for (uint16_t index = 0; index < lit_count; ++index) {
        set_state_pixel_level(index, state, 255U);
    }
}

static void render_twinkle(const light_state_t *state, uint32_t frame)
{
    uint32_t tick = frame * (2U + ((uint32_t) state->speed / 48U));
    const uint8_t background_level = 10U;

    for (uint16_t index = 0; index < CONFIG_LIGHT_RING_LED_COUNT; ++index) {
        uint32_t seed = hash32(((uint32_t) index + 1U) * 0x9e3779b1U);
        uint8_t phase = (uint8_t) (tick + (seed & 0xFFU));
        uint8_t triangle = (phase < 128U) ? (uint8_t) (phase * 2U) : (uint8_t) ((255U - phase) * 2U);
        uint8_t sparkle = 0U;

        if (triangle > 228U) {
            sparkle = ease8_in_out((uint8_t) ((((uint16_t) (triangle - 228U)) * 255U) / 27U));
        }

        if (sparkle == 0U) {
            set_state_pixel_level(index, state, background_level);
            continue;
        }

        set_state_pixel_level(index, state,
                              (uint8_t) (background_level + (((uint32_t) sparkle * (255U - background_level)) / 255U)));
    }
}

static void render_scanner(const light_state_t *state, uint32_t frame)
{
    uint16_t span = (CONFIG_LIGHT_RING_LED_COUNT > 1U) ? ((CONFIG_LIGHT_RING_LED_COUNT - 1U) * 2U) : 1U;
    uint16_t phase = (uint16_t) ((frame * (1U + ((uint32_t) state->speed / 40U))) % span);
    uint16_t head = (phase < CONFIG_LIGHT_RING_LED_COUNT) ? phase : (span - phase);

    for (uint16_t index = 0; index < CONFIG_LIGHT_RING_LED_COUNT; ++index) {
        uint16_t distance = abs_distance_u16(index, head);
        uint8_t level = 4U;

        if (distance == 0U) {
            level = 255U;
        } else if (distance == 1U) {
            level = 168U;
        } else if (distance == 2U) {
            level = 80U;
        } else if (distance == 3U) {
            level = 28U;
        }

        set_state_pixel_level(index, state, level);
    }
}

static void render_sparkle(const light_state_t *state, uint32_t frame)
{
    uint32_t burst = (frame / 2U) * (1U + ((uint32_t) state->speed / 64U));
    uint16_t sparkle_a = (uint16_t) (hash32(burst) % CONFIG_LIGHT_RING_LED_COUNT);
    uint16_t sparkle_b = (uint16_t) (hash32(burst ^ 0xa511e9b3U) % CONFIG_LIGHT_RING_LED_COUNT);
    uint8_t white_level = state->brightness;

    for (uint16_t index = 0; index < CONFIG_LIGHT_RING_LED_COUNT; ++index) {
        set_state_pixel_level(index, state, 18U);
    }

    set_pixel_rgb(sparkle_a, white_level, white_level, white_level);
    if (sparkle_b != sparkle_a) {
        uint8_t accent_level = (uint8_t) ((white_level > 96U) ? (white_level - 48U) : white_level);
        set_pixel_rgb(sparkle_b, accent_level, accent_level, accent_level);
    }
}

static uint8_t swoosh_left_ring_index(uint8_t position)
{
    return (uint8_t) ((CONFIG_LIGHT_RING_LED_COUNT - 1U - position) % CONFIG_LIGHT_RING_LED_COUNT);
}

static uint8_t swoosh_right_ring_index(uint8_t position)
{
    return (uint8_t) ((CONFIG_LIGHT_RING_LED_COUNT - 1U + position) % CONFIG_LIGHT_RING_LED_COUNT);
}

static void load_swoosh_palette_context(light_state_t *ctx, const light_state_t *base, uint8_t palette_id)
{
    *ctx = *base;
    ctx->palette = palette_id;
    ctx->has_palette_cache = false;
    ctx->palette_stop_count = 0U;
    ctx->palette_circular = false;
    memset(ctx->palette_stops, 0, sizeof(ctx->palette_stops));
    memset(ctx->palette_cache, 0, sizeof(ctx->palette_cache));

    if (is_custom_palette_id(palette_id)) {
        size_t slot = custom_palette_slot_from_id(palette_id);
        const custom_palette_t *pal = &s_custom_palettes[slot];
        ctx->palette_stop_count = custom_palette_stop_count(pal);
        ctx->palette_circular = custom_palette_is_circular(pal);
        memcpy(ctx->palette_stops, pal->stops, sizeof(ctx->palette_stops));
        memcpy(ctx->palette_cache, pal->colors, sizeof(ctx->palette_cache));
        ctx->has_palette_cache = ctx->palette_stop_count > 0U;
    }
}

static void render_swoosh(const light_state_t *state, uint32_t frame)
{
    light_state_t background;
    load_swoosh_palette_context(&background, state, state->swoosh_background_palette);

    for (uint16_t index = 0; index < CONFIG_LIGHT_RING_LED_COUNT; ++index) {
        set_state_pixel_level(index, &background, 255U);
    }

    uint8_t left_span = clamp_swoosh_span(state->swoosh_left_stops);
    uint8_t right_span = clamp_swoosh_span(state->swoosh_right_stops);
    uint8_t head = (uint8_t) ((frame * (1U + ((uint32_t) state->speed / 96U))) % SWOOSH_PATH_LENGTH);
    light_state_t left_state;
    light_state_t right_state;

    load_swoosh_palette_context(&left_state, state, state->swoosh_left_palette);
    load_swoosh_palette_context(&right_state, state, state->swoosh_right_palette);

    for (uint8_t tail = 0; tail < left_span; ++tail) {
        uint8_t position = (uint8_t) ((head + SWOOSH_PATH_LENGTH - tail) % SWOOSH_PATH_LENGTH);
        uint8_t palette_position = (uint8_t) (SWOOSH_PATH_LENGTH - 1U - position);
        uint8_t led = swoosh_left_ring_index(position);
        uint8_t level = (uint8_t) (255U - ((uint16_t) tail * 196U) / left_span);
        set_state_palette_level(led, &left_state, led_index_to_palette_index_linear(palette_position), level);
    }

    for (uint8_t tail = 0; tail < right_span; ++tail) {
        uint8_t position = (uint8_t) ((head + SWOOSH_PATH_LENGTH - tail) % SWOOSH_PATH_LENGTH);
        uint8_t palette_position = (uint8_t) (SWOOSH_PATH_LENGTH - 1U - position);
        uint8_t led = swoosh_right_ring_index(position);
        uint8_t level = (uint8_t) (255U - ((uint16_t) tail * 196U) / right_span);
        set_state_palette_level(led, &right_state, led_index_to_palette_index_linear(palette_position), level);
    }
}

static void render_swoosh_reverse(const light_state_t *state, uint32_t frame)
{
    light_state_t background;
    load_swoosh_palette_context(&background, state, state->swoosh_background_palette);

    for (uint16_t index = 0; index < CONFIG_LIGHT_RING_LED_COUNT; ++index) {
        set_state_pixel_level(index, &background, 255U);
    }

    uint8_t left_span = clamp_swoosh_span(state->swoosh_left_stops);
    uint8_t right_span = clamp_swoosh_span(state->swoosh_right_stops);
    uint8_t head = (uint8_t) ((frame * (1U + ((uint32_t) state->speed / 96U))) % SWOOSH_PATH_LENGTH);
    light_state_t left_state;
    light_state_t right_state;

    load_swoosh_palette_context(&left_state, state, state->swoosh_left_palette);
    load_swoosh_palette_context(&right_state, state, state->swoosh_right_palette);

    for (uint8_t tail = 0; tail < left_span; ++tail) {
        uint8_t position = (uint8_t) ((head + SWOOSH_PATH_LENGTH - tail) % SWOOSH_PATH_LENGTH);
        uint8_t mirrored_position = (uint8_t) (SWOOSH_PATH_LENGTH - 1U - position);
        uint8_t led = swoosh_left_ring_index(mirrored_position);
        uint8_t level = (uint8_t) (255U - ((uint16_t) tail * 196U) / left_span);
        set_state_palette_level(led, &left_state, led_index_to_palette_index_linear(position), level);
    }

    for (uint8_t tail = 0; tail < right_span; ++tail) {
        uint8_t position = (uint8_t) ((head + SWOOSH_PATH_LENGTH - tail) % SWOOSH_PATH_LENGTH);
        uint8_t mirrored_position = (uint8_t) (SWOOSH_PATH_LENGTH - 1U - position);
        uint8_t led = swoosh_right_ring_index(mirrored_position);
        uint8_t level = (uint8_t) (255U - ((uint16_t) tail * 196U) / right_span);
        set_state_palette_level(led, &right_state, led_index_to_palette_index_linear(position), level);
    }
}

static void render_shrink(const light_state_t *state, uint32_t frame)
{
    light_state_t background;
    light_state_t bar;
    load_swoosh_palette_context(&background, state, state->shrink_background_palette);
    load_swoosh_palette_context(&bar, state, state->shrink_bar_palette);

    const uint16_t led_count = CONFIG_LIGHT_RING_LED_COUNT;
    const uint8_t bar_len = (uint8_t) SHRINK_BAR_LENGTH;
    uint8_t min_len = clamp_shrink_length(state->shrink_length);
    if (min_len > bar_len) {
        min_len = bar_len;
    }
    /* Shrink symmetrically: remove the same number of pixels from each end so
       both sides always move at an identical speed. */
    uint8_t half_range = (uint8_t) ((bar_len - min_len) / 2U);

    uint8_t removed_per_side = 0U;
    if (half_range > 0U) {
        /* Always advance one pixel per frame so the shrink/grow stays smooth
           even at the fastest speed (speed only controls the frame delay).
           A configurable gap holds the bar paused at both the fully-shrunk and
           fully-expanded extremes. */
        uint32_t gap = clamp_shrink_gap(state->shrink_gap);
        uint32_t cycle = ((uint32_t) half_range * 2U) + (gap * 2U);
        uint32_t phase = frame % cycle;

        if (phase < half_range) {
            removed_per_side = (uint8_t) phase;                      /* shrinking */
        } else if (phase < (half_range + gap)) {
            removed_per_side = half_range;                           /* hold shrunk */
        } else if (phase < ((uint32_t) half_range * 2U) + gap) {
            removed_per_side = (uint8_t) (((uint32_t) half_range * 2U) + gap - phase); /* growing */
        } else {
            removed_per_side = 0U;                                   /* hold expanded */
        }
    }

    uint8_t active_end = (uint8_t) (bar_len - removed_per_side);

    for (uint8_t bar_index = 0; bar_index < SHRINK_BAR_COUNT; ++bar_index) {
        uint16_t start = (uint16_t) (((uint16_t) (SHRINK_SEGMENT_LENGTH * (bar_index + 1U)) - 1U) % led_count);
        for (uint8_t pos = 0; pos < bar_len; ++pos) {
            uint16_t led = (uint16_t) ((start + pos) % led_count);
            uint8_t palette_index = (bar_len > 1U)
                                        ? (uint8_t) (((uint16_t) pos * 255U) / (bar_len - 1U))
                                        : 0U;
            bool active = (pos >= removed_per_side) && (pos < active_end);
            const light_state_t *ctx = active ? &bar : &background;
            set_state_palette_level(led, ctx, palette_index, 255U);
        }
    }
}

static void render_rotation(const light_state_t *state, uint32_t frame)
{
    light_state_t background;
    light_state_t rotation;
    load_swoosh_palette_context(&background, state, state->rotation_background_palette);
    load_swoosh_palette_context(&rotation, state, state->rotation_palette);

    const uint16_t led_count = CONFIG_LIGHT_RING_LED_COUNT;
    uint8_t base_len = clamp_rotation_length(state->rotation_length);
    if (base_len > (uint8_t) ROTATION_SEGMENT_LENGTH) {
        base_len = (uint8_t) ROTATION_SEGMENT_LENGTH;
    }

    /* Paint the whole ring with the background palette first; the rotation
       blocks are drawn on top of it. */
    for (uint16_t index = 0; index < led_count; ++index) {
        set_state_pixel_level(index, &background, 255U);
    }

    /* The three corner blocks grow from base_len up to a full segment so they
       eventually cover every background pixel, then restore back to base_len.
       The rotation gap holds the blocks paused at both extremes. */
    uint8_t grow_range = (uint8_t) (ROTATION_SEGMENT_LENGTH - base_len);
    uint8_t grow = 0U;
    if (grow_range > 0U) {
        uint32_t gap = clamp_rotation_gap(state->rotation_gap);
        uint32_t cycle = ((uint32_t) grow_range * 2U) + (gap * 2U);
        uint32_t phase = frame % cycle;
        if (phase < grow_range) {
            grow = (uint8_t) phase;                                          /* expanding */
        } else if (phase < ((uint32_t) grow_range + gap)) {
            grow = grow_range;                                               /* hold covered */
        } else if (phase < (((uint32_t) grow_range * 2U) + gap)) {
            grow = (uint8_t) (((uint32_t) grow_range * 2U) + gap - phase);   /* restoring */
        } else {
            grow = 0U;                                                       /* hold at base */
        }
    }

    uint8_t active_len = (uint8_t) (base_len + grow);

    for (uint8_t corner = 0; corner < ROTATION_CORNER_COUNT; ++corner) {
        /* The corner pairs sit at LEDs {hi-1, hi} where hi matches the segment
           boundary (8, 17, 26 on a 27-LED ring). The base block of base_len
           pixels expands symmetrically around that pair. */
        uint16_t hi = (uint16_t) (((uint16_t) (ROTATION_SEGMENT_LENGTH * (corner + 1U)) - 1U) % led_count);
        uint8_t extra = (uint8_t) (base_len - ROTATION_MIN_LENGTH);
        uint8_t left_extra = (uint8_t) (extra / 2U);
        uint16_t base_left = (uint16_t) ((hi + led_count - 1U - left_extra) % led_count);
        uint16_t base_right = (uint16_t) ((hi + (base_len - 2U - left_extra)) % led_count);

        uint16_t start;
        if (!state->rotation_counterclockwise) {
            /* Clockwise: anchor the left edge and grow toward higher indices. */
            start = base_left;
        } else {
            /* Counterclockwise: anchor the right edge and grow toward lower indices. */
            start = (uint16_t) ((base_right + led_count + 1U - active_len) % led_count);
        }

        for (uint8_t pos = 0; pos < active_len; ++pos) {
            uint16_t led = (uint16_t) ((start + pos) % led_count);
            uint8_t palette_index = (active_len > 1U)
                                        ? (uint8_t) (((uint16_t) pos * 255U) / (active_len - 1U))
                                        : 0U;
            set_state_palette_level(led, &rotation, palette_index, 255U);
        }
    }
}

static esp_err_t transmit_pixels(void)
{
    const rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };

    ESP_RETURN_ON_ERROR(rmt_transmit(s_led_channel, s_led_encoder, s_led_pixels, sizeof(s_led_pixels), &tx_config), TAG, "LED transmit failed");
    return rmt_tx_wait_all_done(s_led_channel, portMAX_DELAY);
}

static void render_frame(const light_state_t *state, uint32_t frame)
{
    if (!state->on || state->brightness == 0U) {
        clear_pixels();
        return;
    }

    switch (state->effect) {
    case EFFECT_SOLID:
        render_solid(state);
        break;
    case EFFECT_BREATHE:
        render_breathe(state, frame);
        break;
    case EFFECT_RAINBOW:
        render_rainbow(state, frame);
        break;
    case EFFECT_CHASE:
        render_chase(state, frame);
        break;
    case EFFECT_COLOR_WIPE:
        render_color_wipe(state, frame);
        break;
    case EFFECT_TWINKLE:
        render_twinkle(state, frame);
        break;
    case EFFECT_SCANNER:
        render_scanner(state, frame);
        break;
    case EFFECT_SPARKLE:
        render_sparkle(state, frame);
        break;
    case EFFECT_SWOOSH:
        render_swoosh(state, frame);
        break;
    case EFFECT_SWOOSH_REVERSE:
        render_swoosh_reverse(state, frame);
        break;
    case EFFECT_SHRINK:
        render_shrink(state, frame);
        break;
    case EFFECT_ROTATION:
        render_rotation(state, frame);
        break;
    default:
        render_solid(state);
        break;
    }
}

static void light_render_task(void *arg)
{
    uint32_t frame = 0;
    (void) arg;

    while (true) {
        light_state_t state;
        snapshot_light_state(&state);
        if (!(state.paused && state.on)) {
            render_frame(&state, frame++);
            if (transmit_pixels() != ESP_OK) {
                ESP_LOGW(TAG, "Failed to flush frame to LED ring");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(effect_delay_ms(&state)));
    }
}

static esp_err_t init_led_strip(void)
{
    rmt_tx_channel_config_t channel_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = CONFIG_LIGHT_RING_LED_GPIO,
        .mem_block_symbols = LED_STRIP_MEM_BLOCK_SYMBOLS,
        .resolution_hz = LED_STRIP_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&channel_config, &s_led_channel), TAG, "create RMT channel failed");

    led_strip_encoder_config_t encoder_config = {
        .resolution_hz = LED_STRIP_RESOLUTION_HZ,
    };
    ESP_RETURN_ON_ERROR(rmt_new_led_strip_encoder(&encoder_config, &s_led_encoder), TAG, "create LED encoder failed");
    ESP_RETURN_ON_ERROR(rmt_enable(s_led_channel), TAG, "enable RMT channel failed");

    BaseType_t task_created = xTaskCreate(light_render_task, "light_render", 4096, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(task_created == pdPASS, ESP_ERR_NO_MEM, TAG, "create light task failed");

    return ESP_OK;
}

static void build_device_identity(void)
{
    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(s_device_name, sizeof(s_device_name), "WLED-S3R8-%02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s", s_device_name);
}

static bool has_sta_credentials(void)
{
    return strlen(CONFIG_LIGHT_RING_STA_SSID) > 0U;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void) arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START && has_sta_credentials()) {
        ESP_LOGI(TAG, "Connecting to station SSID: %s", CONFIG_LIGHT_RING_STA_SSID);
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        s_sta_ip[0] = '\0';
        if (has_sta_credentials()) {
            ESP_LOGW(TAG, "STA disconnected, retrying");
            esp_wifi_connect();
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *got_ip_event = (ip_event_got_ip_t *) event_data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&got_ip_event->ip_info.ip));
        s_sta_connected = true;
        ESP_LOGI(TAG, "STA connected with IP %s", s_sta_ip);
    }
}

static void build_ap_root_url(char *buffer, size_t length)
{
    if (s_ap_ip.addr != 0U) {
        snprintf(buffer, length, "http://" IPSTR "/", IP2STR(&s_ap_ip));
    } else {
        strlcpy(buffer, "http://192.168.4.1/", length);
    }
}

static size_t dns_question_end_offset(const uint8_t *packet, size_t length)
{
    size_t offset = 12U;
    while (offset < length) {
        uint8_t label_len = packet[offset++];
        if (label_len == 0U) {
            break;
        }
        if ((label_len & 0xC0U) != 0U || (offset + label_len) > length) {
            return 0U;
        }
        offset += label_len;
    }

    if ((offset + 4U) > length) {
        return 0U;
    }
    return offset + 4U;
}

static ssize_t build_dns_response_packet(const uint8_t *query, size_t query_len, uint8_t *response, size_t response_len)
{
    if (query_len < 12U || response_len < 12U) {
        return -1;
    }

    uint16_t question_count = (uint16_t) ((query[4] << 8) | query[5]);
    if (question_count == 0U) {
        return -1;
    }

    size_t question_end = dns_question_end_offset(query, query_len);
    if (question_end == 0U || question_end > response_len) {
        return -1;
    }

    uint16_t question_type = (uint16_t) ((query[question_end - 4U] << 8) | query[question_end - 3U]);
    bool answer_ipv4 = (question_type == 1U || question_type == 255U);

    size_t required = question_end + (answer_ipv4 ? 16U : 0U);
    if (required > response_len) {
        return -1;
    }

    memcpy(response, query, question_end);
    response[2] = 0x81;
    response[3] = 0x80;
    response[6] = 0x00;
    response[7] = answer_ipv4 ? 0x01 : 0x00;
    response[8] = 0x00;
    response[9] = 0x00;
    response[10] = 0x00;
    response[11] = 0x00;

    if (!answer_ipv4) {
        return (ssize_t) question_end;
    }

    size_t offset = question_end;
    response[offset++] = 0xC0;
    response[offset++] = 0x0C;
    response[offset++] = 0x00;
    response[offset++] = 0x01;
    response[offset++] = 0x00;
    response[offset++] = 0x01;
    response[offset++] = 0x00;
    response[offset++] = 0x00;
    response[offset++] = 0x00;
    response[offset++] = 0x3C;
    response[offset++] = 0x00;
    response[offset++] = 0x04;
    uint32_t ap_ip_host = ntohl(s_ap_ip.addr);
    response[offset++] = (uint8_t) ((ap_ip_host >> 24) & 0xFFU);
    response[offset++] = (uint8_t) ((ap_ip_host >> 16) & 0xFFU);
    response[offset++] = (uint8_t) ((ap_ip_host >> 8) & 0xFFU);
    response[offset++] = (uint8_t) (ap_ip_host & 0xFFU);

    return (ssize_t) offset;
}

static void captive_dns_task(void *arg)
{
    (void) arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Captive DNS socket create failed: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_SERVER_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *) &bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "Captive DNS bind failed: errno=%d", errno);
        lwip_close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Captive DNS ready on " IPSTR ":%d", IP2STR(&s_ap_ip), DNS_SERVER_PORT);

    uint8_t query[DNS_PACKET_MAX_SIZE];
    uint8_t response[DNS_PACKET_MAX_SIZE];
    while (true) {
        struct sockaddr_in source_addr;
        socklen_t source_len = sizeof(source_addr);
        ssize_t received = recvfrom(sock, query, sizeof(query), 0, (struct sockaddr *) &source_addr, &source_len);
        if (received <= 0) {
            continue;
        }

        ssize_t response_size = build_dns_response_packet(query, (size_t) received, response, sizeof(response));
        if (response_size <= 0) {
            continue;
        }

        sendto(sock, response, (size_t) response_size, 0, (struct sockaddr *) &source_addr, source_len);
    }
}

static esp_err_t start_captive_dns_server(void)
{
    BaseType_t task_created = xTaskCreate(captive_dns_task, "captive_dns", 4096, NULL, 4, NULL);
    ESP_RETURN_ON_FALSE(task_created == pdPASS, ESP_ERR_NO_MEM, TAG, "create captive dns task failed");
    return ESP_OK;
}

static esp_err_t init_wifi(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop create failed");

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(ap_netif != NULL, ESP_FAIL, TAG, "create AP netif failed");

    esp_netif_t *sta_netif = NULL;
    if (has_sta_credentials()) {
        sta_netif = esp_netif_create_default_wifi_sta();
        ESP_RETURN_ON_FALSE(sta_netif != NULL, ESP_FAIL, TAG, "create STA netif failed");
        ESP_ERROR_CHECK(esp_netif_set_hostname(sta_netif, s_device_name));
    }

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_init_config), TAG, "esp_wifi_init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL), TAG, "register WIFI handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL), TAG, "register IP handler failed");

    wifi_config_t ap_config = {
        .ap = {
            .channel = 1,
            .max_connection = AP_MAX_STA_CONNECTIONS,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    strlcpy((char *) ap_config.ap.ssid, s_ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(s_ap_ssid);
    strlcpy((char *) ap_config.ap.password, CONFIG_LIGHT_RING_AP_PASSWORD, sizeof(ap_config.ap.password));
    ap_config.ap.authmode = strlen(CONFIG_LIGHT_RING_AP_PASSWORD) >= 8U ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    if (ap_config.ap.authmode == WIFI_AUTH_OPEN) {
        ap_config.ap.password[0] = '\0';
    }

    wifi_mode_t mode = has_sta_credentials() ? WIFI_MODE_APSTA : WIFI_MODE_AP;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(mode), TAG, "esp_wifi_set_mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "esp_wifi_set_storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), TAG, "esp_wifi_set_config AP failed");

    if (has_sta_credentials()) {
        wifi_config_t sta_config = {
            .sta = {
                .failure_retry_cnt = 5,
                .scan_method = WIFI_ALL_CHANNEL_SCAN,
                .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
                .threshold.authmode = WIFI_AUTH_WPA2_PSK,
                .pmf_cfg = {
                    .capable = true,
                    .required = false,
                },
            },
        };

        strlcpy((char *) sta_config.sta.ssid, CONFIG_LIGHT_RING_STA_SSID, sizeof(sta_config.sta.ssid));
        strlcpy((char *) sta_config.sta.password, CONFIG_LIGHT_RING_STA_PASSWORD, sizeof(sta_config.sta.password));
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_config), TAG, "esp_wifi_set_config STA failed");
    }

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "disable Wi-Fi power save failed");

    esp_netif_ip_info_t ap_ip_info;
    ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(ap_netif, &ap_ip_info), TAG, "read AP IP info failed");
    s_ap_ip = ap_ip_info.ip;

    ESP_LOGI(TAG, "Access point ready: SSID=%s", s_ap_ssid);
    ESP_LOGI(TAG, "Access point IP: " IPSTR, IP2STR(&s_ap_ip));
    return ESP_OK;
}

static void set_common_response_headers(httpd_req_t *req, const char *content_type)
{
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

static esp_err_t send_json_response(httpd_req_t *req, cJSON *json)
{
    char *payload = cJSON_PrintUnformatted(json);
    if (payload == NULL) {
        return httpd_resp_send_500(req);
    }

    set_common_response_headers(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, payload);
    free(payload);
    return ret;
}

static bool apply_color_array(cJSON *color_array, light_state_t *state)
{
    if (!cJSON_IsArray(color_array) || cJSON_GetArraySize(color_array) < 3) {
        return false;
    }

    cJSON *red = cJSON_GetArrayItem(color_array, 0);
    cJSON *green = cJSON_GetArrayItem(color_array, 1);
    cJSON *blue = cJSON_GetArrayItem(color_array, 2);
    if (!cJSON_IsNumber(red) || !cJSON_IsNumber(green) || !cJSON_IsNumber(blue)) {
        return false;
    }

    state->red = clamp_u8(red->valueint);
    state->green = clamp_u8(green->valueint);
    state->blue = clamp_u8(blue->valueint);
    return true;
}

static void apply_segment_json(cJSON *segment, light_state_t *state)
{
    cJSON *fx = cJSON_GetObjectItemCaseSensitive(segment, "fx");
    cJSON *sx = cJSON_GetObjectItemCaseSensitive(segment, "sx");
    cJSON *pal = cJSON_GetObjectItemCaseSensitive(segment, "pal");
    cJSON *bg_pal = cJSON_GetObjectItemCaseSensitive(segment, "bgPal");
    cJSON *left_pal = cJSON_GetObjectItemCaseSensitive(segment, "leftPal");
    cJSON *right_pal = cJSON_GetObjectItemCaseSensitive(segment, "rightPal");
    cJSON *left_stops = cJSON_GetObjectItemCaseSensitive(segment, "leftStops");
    cJSON *right_stops = cJSON_GetObjectItemCaseSensitive(segment, "rightStops");
    cJSON *shrink_bg = cJSON_GetObjectItemCaseSensitive(segment, "shrinkBg");
    cJSON *shrink_bar = cJSON_GetObjectItemCaseSensitive(segment, "shrinkBar");
    cJSON *shrink_len = cJSON_GetObjectItemCaseSensitive(segment, "shrinkLen");
    cJSON *shrink_gap = cJSON_GetObjectItemCaseSensitive(segment, "shrinkGap");
    cJSON *rot_bg = cJSON_GetObjectItemCaseSensitive(segment, "rotBg");
    cJSON *rot_pal = cJSON_GetObjectItemCaseSensitive(segment, "rotPal");
    cJSON *rot_len = cJSON_GetObjectItemCaseSensitive(segment, "rotLen");
    cJSON *rot_gap = cJSON_GetObjectItemCaseSensitive(segment, "rotGap");
    cJSON *rot_ccw = cJSON_GetObjectItemCaseSensitive(segment, "rotCcw");
    cJSON *on = cJSON_GetObjectItemCaseSensitive(segment, "on");
    cJSON *colors = cJSON_GetObjectItemCaseSensitive(segment, "col");

    if (cJSON_IsNumber(fx)) {
        state->effect = clamp_effect(fx->valueint);
    }
    if (cJSON_IsNumber(sx)) {
        state->speed = clamp_u8(sx->valueint);
    }
    if (cJSON_IsNumber(pal)) {
        state->palette = clamp_palette(pal->valueint);
    }
    if (cJSON_IsNumber(bg_pal)) {
        state->swoosh_background_palette = clamp_palette(bg_pal->valueint);
    }
    if (cJSON_IsNumber(left_pal)) {
        state->swoosh_left_palette = clamp_palette(left_pal->valueint);
    }
    if (cJSON_IsNumber(right_pal)) {
        state->swoosh_right_palette = clamp_palette(right_pal->valueint);
    }
    if (cJSON_IsNumber(left_stops)) {
        state->swoosh_left_stops = clamp_swoosh_span(left_stops->valueint);
    }
    if (cJSON_IsNumber(right_stops)) {
        state->swoosh_right_stops = clamp_swoosh_span(right_stops->valueint);
    }
    if (cJSON_IsNumber(shrink_bg)) {
        state->shrink_background_palette = clamp_palette(shrink_bg->valueint);
    }
    if (cJSON_IsNumber(shrink_bar)) {
        state->shrink_bar_palette = clamp_palette(shrink_bar->valueint);
    }
    if (cJSON_IsNumber(shrink_len)) {
        state->shrink_length = clamp_shrink_length(shrink_len->valueint);
    }
    if (cJSON_IsNumber(shrink_gap)) {
        state->shrink_gap = clamp_shrink_gap(shrink_gap->valueint);
    }
    if (cJSON_IsNumber(rot_bg)) {
        state->rotation_background_palette = clamp_palette(rot_bg->valueint);
    }
    if (cJSON_IsNumber(rot_pal)) {
        state->rotation_palette = clamp_palette(rot_pal->valueint);
    }
    if (cJSON_IsNumber(rot_len)) {
        state->rotation_length = clamp_rotation_length(rot_len->valueint);
    }
    if (cJSON_IsNumber(rot_gap)) {
        state->rotation_gap = clamp_rotation_gap(rot_gap->valueint);
    }
    if (cJSON_IsBool(rot_ccw)) {
        state->rotation_counterclockwise = cJSON_IsTrue(rot_ccw);
    }
    if (cJSON_IsBool(on)) {
        state->on = cJSON_IsTrue(on);
    }
    if (cJSON_IsArray(colors) && cJSON_GetArraySize(colors) > 0) {
        apply_color_array(cJSON_GetArrayItem(colors, 0), state);
    }
}

static void apply_json_state(cJSON *root, light_state_t *state)
{
    cJSON *on = cJSON_GetObjectItemCaseSensitive(root, "on");
    cJSON *paused = cJSON_GetObjectItemCaseSensitive(root, "paused");
    cJSON *bri = cJSON_GetObjectItemCaseSensitive(root, "bri");
    cJSON *fx = cJSON_GetObjectItemCaseSensitive(root, "fx");
    cJSON *effect = cJSON_GetObjectItemCaseSensitive(root, "effect");
    cJSON *sx = cJSON_GetObjectItemCaseSensitive(root, "sx");
    cJSON *speed = cJSON_GetObjectItemCaseSensitive(root, "speed");
    cJSON *pal = cJSON_GetObjectItemCaseSensitive(root, "pal");
    cJSON *palette = cJSON_GetObjectItemCaseSensitive(root, "palette");
    cJSON *bg_pal = cJSON_GetObjectItemCaseSensitive(root, "bgPal");
    cJSON *left_pal = cJSON_GetObjectItemCaseSensitive(root, "leftPal");
    cJSON *right_pal = cJSON_GetObjectItemCaseSensitive(root, "rightPal");
    cJSON *left_stops = cJSON_GetObjectItemCaseSensitive(root, "leftStops");
    cJSON *right_stops = cJSON_GetObjectItemCaseSensitive(root, "rightStops");
    cJSON *shrink_bg = cJSON_GetObjectItemCaseSensitive(root, "shrinkBg");
    cJSON *shrink_bar = cJSON_GetObjectItemCaseSensitive(root, "shrinkBar");
    cJSON *shrink_len = cJSON_GetObjectItemCaseSensitive(root, "shrinkLen");
    cJSON *shrink_gap = cJSON_GetObjectItemCaseSensitive(root, "shrinkGap");
    cJSON *rot_bg = cJSON_GetObjectItemCaseSensitive(root, "rotBg");
    cJSON *rot_pal = cJSON_GetObjectItemCaseSensitive(root, "rotPal");
    cJSON *rot_len = cJSON_GetObjectItemCaseSensitive(root, "rotLen");
    cJSON *rot_gap = cJSON_GetObjectItemCaseSensitive(root, "rotGap");
    cJSON *rot_ccw = cJSON_GetObjectItemCaseSensitive(root, "rotCcw");
    cJSON *color = cJSON_GetObjectItemCaseSensitive(root, "color");
    cJSON *segments = cJSON_GetObjectItemCaseSensitive(root, "seg");

    if (cJSON_IsBool(on)) {
        state->on = cJSON_IsTrue(on);
    }
    if (cJSON_IsBool(paused)) {
        state->paused = cJSON_IsTrue(paused);
    }
    if (cJSON_IsNumber(bri)) {
        state->brightness = clamp_u8(bri->valueint);
    }
    if (cJSON_IsNumber(fx)) {
        state->effect = clamp_effect(fx->valueint);
    }
    if (cJSON_IsString(effect) && effect->valuestring != NULL) {
        state->effect = parse_effect_name(effect->valuestring);
    } else if (cJSON_IsNumber(effect)) {
        state->effect = clamp_effect(effect->valueint);
    }
    if (cJSON_IsNumber(sx)) {
        state->speed = clamp_u8(sx->valueint);
    }
    if (cJSON_IsNumber(speed)) {
        state->speed = clamp_u8(speed->valueint);
    }
    if (cJSON_IsNumber(pal)) {
        state->palette = clamp_palette(pal->valueint);
    }
    if (cJSON_IsNumber(palette)) {
        state->palette = clamp_palette(palette->valueint);
    }
    if (cJSON_IsNumber(bg_pal)) {
        state->swoosh_background_palette = clamp_palette(bg_pal->valueint);
    }
    if (cJSON_IsNumber(left_pal)) {
        state->swoosh_left_palette = clamp_palette(left_pal->valueint);
    }
    if (cJSON_IsNumber(right_pal)) {
        state->swoosh_right_palette = clamp_palette(right_pal->valueint);
    }
    if (cJSON_IsNumber(left_stops)) {
        state->swoosh_left_stops = clamp_swoosh_span(left_stops->valueint);
    }
    if (cJSON_IsNumber(right_stops)) {
        state->swoosh_right_stops = clamp_swoosh_span(right_stops->valueint);
    }
    if (cJSON_IsNumber(shrink_bg)) {
        state->shrink_background_palette = clamp_palette(shrink_bg->valueint);
    }
    if (cJSON_IsNumber(shrink_bar)) {
        state->shrink_bar_palette = clamp_palette(shrink_bar->valueint);
    }
    if (cJSON_IsNumber(shrink_len)) {
        state->shrink_length = clamp_shrink_length(shrink_len->valueint);
    }
    if (cJSON_IsNumber(shrink_gap)) {
        state->shrink_gap = clamp_shrink_gap(shrink_gap->valueint);
    }
    if (cJSON_IsNumber(rot_bg)) {
        state->rotation_background_palette = clamp_palette(rot_bg->valueint);
    }
    if (cJSON_IsNumber(rot_pal)) {
        state->rotation_palette = clamp_palette(rot_pal->valueint);
    }
    if (cJSON_IsNumber(rot_len)) {
        state->rotation_length = clamp_rotation_length(rot_len->valueint);
    }
    if (cJSON_IsNumber(rot_gap)) {
        state->rotation_gap = clamp_rotation_gap(rot_gap->valueint);
    }
    if (cJSON_IsBool(rot_ccw)) {
        state->rotation_counterclockwise = cJSON_IsTrue(rot_ccw);
    }
    apply_color_array(color, state);

    if (cJSON_IsArray(segments) && cJSON_GetArraySize(segments) > 0) {
        apply_segment_json(cJSON_GetArrayItem(segments, 0), state);
    }
}

static cJSON *build_state_json(void)
{
    light_state_t state;
    snapshot_light_state(&state);

    cJSON *root = cJSON_CreateObject();
    cJSON *segment_array = cJSON_AddArrayToObject(root, "seg");
    cJSON *segment = cJSON_CreateObject();
    cJSON *color_array = cJSON_AddArrayToObject(root, "color");
    cJSON *segment_colors = cJSON_AddArrayToObject(segment, "col");
    cJSON *primary_color = cJSON_CreateArray();

    cJSON_AddBoolToObject(root, "on", state.on);
    cJSON_AddBoolToObject(root, "paused", state.paused);
    cJSON_AddNumberToObject(root, "bri", state.brightness);
    cJSON_AddNumberToObject(root, "fx", state.effect);
    cJSON_AddNumberToObject(root, "speed", state.speed);
    cJSON_AddNumberToObject(root, "pal", state.palette);
    cJSON_AddNumberToObject(root, "palette", state.palette);
    cJSON_AddNumberToObject(root, "bgPal", state.swoosh_background_palette);
    cJSON_AddNumberToObject(root, "leftPal", state.swoosh_left_palette);
    cJSON_AddNumberToObject(root, "rightPal", state.swoosh_right_palette);
    cJSON_AddNumberToObject(root, "leftStops", state.swoosh_left_stops);
    cJSON_AddNumberToObject(root, "rightStops", state.swoosh_right_stops);
    cJSON_AddNumberToObject(root, "shrinkBg", state.shrink_background_palette);
    cJSON_AddNumberToObject(root, "shrinkBar", state.shrink_bar_palette);
    cJSON_AddNumberToObject(root, "shrinkLen", state.shrink_length);
    cJSON_AddNumberToObject(root, "shrinkGap", state.shrink_gap);
    cJSON_AddNumberToObject(root, "rotBg", state.rotation_background_palette);
    cJSON_AddNumberToObject(root, "rotPal", state.rotation_palette);
    cJSON_AddNumberToObject(root, "rotLen", state.rotation_length);
    cJSON_AddNumberToObject(root, "rotGap", state.rotation_gap);
    cJSON_AddBoolToObject(root, "rotCcw", state.rotation_counterclockwise);
    cJSON_AddStringToObject(root, "effectName", effect_name((effect_id_t) state.effect));
    cJSON_AddStringToObject(root, "paletteName", state.palette_label[0] != '\0' ? state.palette_label : palette_name(state.palette));

    cJSON_AddItemToArray(color_array, cJSON_CreateNumber(state.red));
    cJSON_AddItemToArray(color_array, cJSON_CreateNumber(state.green));
    cJSON_AddItemToArray(color_array, cJSON_CreateNumber(state.blue));

    cJSON_AddNumberToObject(segment, "id", 0);
    cJSON_AddNumberToObject(segment, "start", 0);
    cJSON_AddNumberToObject(segment, "stop", CONFIG_LIGHT_RING_LED_COUNT);
    cJSON_AddNumberToObject(segment, "fx", state.effect);
    cJSON_AddNumberToObject(segment, "sx", state.speed);
    cJSON_AddNumberToObject(segment, "pal", state.palette);
    cJSON_AddNumberToObject(segment, "bgPal", state.swoosh_background_palette);
    cJSON_AddNumberToObject(segment, "leftPal", state.swoosh_left_palette);
    cJSON_AddNumberToObject(segment, "rightPal", state.swoosh_right_palette);
    cJSON_AddNumberToObject(segment, "leftStops", state.swoosh_left_stops);
    cJSON_AddNumberToObject(segment, "rightStops", state.swoosh_right_stops);
    cJSON_AddNumberToObject(segment, "shrinkBg", state.shrink_background_palette);
    cJSON_AddNumberToObject(segment, "shrinkBar", state.shrink_bar_palette);
    cJSON_AddNumberToObject(segment, "shrinkLen", state.shrink_length);
    cJSON_AddNumberToObject(segment, "shrinkGap", state.shrink_gap);
    cJSON_AddNumberToObject(segment, "rotBg", state.rotation_background_palette);
    cJSON_AddNumberToObject(segment, "rotPal", state.rotation_palette);
    cJSON_AddNumberToObject(segment, "rotLen", state.rotation_length);
    cJSON_AddNumberToObject(segment, "rotGap", state.rotation_gap);
    cJSON_AddBoolToObject(segment, "rotCcw", state.rotation_counterclockwise);

    cJSON_AddItemToArray(primary_color, cJSON_CreateNumber(state.red));
    cJSON_AddItemToArray(primary_color, cJSON_CreateNumber(state.green));
    cJSON_AddItemToArray(primary_color, cJSON_CreateNumber(state.blue));
    cJSON_AddItemToArray(segment_colors, primary_color);
    cJSON_AddItemToArray(segment_array, segment);

    return root;
}

static cJSON *build_info_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *led = cJSON_AddObjectToObject(root, "led");
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON *api = cJSON_AddArrayToObject(root, "api");

    cJSON_AddStringToObject(root, "name", s_device_name);
    cJSON_AddStringToObject(root, "brand", "WLED-style ESP-IDF");
    cJSON_AddStringToObject(root, "board", "esp32s3r8");
    cJSON_AddNumberToObject(led, "count", CONFIG_LIGHT_RING_LED_COUNT);
    cJSON_AddNumberToObject(led, "gpio", CONFIG_LIGHT_RING_LED_GPIO);
    cJSON_AddStringToObject(wifi, "ap_ssid", s_ap_ssid);
    cJSON_AddBoolToObject(wifi, "sta_connected", s_sta_connected);
    cJSON_AddStringToObject(wifi, "sta_ssid", CONFIG_LIGHT_RING_STA_SSID);
    cJSON_AddStringToObject(wifi, "sta_ip", s_sta_connected ? s_sta_ip : "");

    cJSON_AddItemToArray(api, cJSON_CreateString("/json/info"));
    cJSON_AddItemToArray(api, cJSON_CreateString("/json/state"));
    cJSON_AddItemToArray(api, cJSON_CreateString("/json/palettes"));
    cJSON_AddItemToArray(api, cJSON_CreateString("/win"));

    return root;
}

static int hex_digit_to_value(char digit)
{
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f') {
        return 10 + digit - 'a';
    }
    if (digit >= 'A' && digit <= 'F') {
        return 10 + digit - 'A';
    }
    return -1;
}

static bool parse_hex_color(const char *value, uint8_t *red, uint8_t *green, uint8_t *blue)
{
    if (value == NULL) {
        return false;
    }

    const char *hex = (value[0] == '#') ? value + 1 : value;
    if (strlen(hex) != 6U) {
        return false;
    }

    int nibbles[6];
    for (size_t index = 0; index < 6U; ++index) {
        nibbles[index] = hex_digit_to_value(hex[index]);
        if (nibbles[index] < 0) {
            return false;
        }
    }

    *red = (uint8_t) ((nibbles[0] << 4) | nibbles[1]);
    *green = (uint8_t) ((nibbles[2] << 4) | nibbles[3]);
    *blue = (uint8_t) ((nibbles[4] << 4) | nibbles[5]);
    return true;
}

static void serialize_palette_colors(cJSON *json, const uint8_t colors[PALETTE_ENTRY_COUNT][3])
{
    for (size_t index = 0; index < PALETTE_ENTRY_COUNT; ++index) {
        cJSON *entry = cJSON_CreateArray();
        cJSON_AddItemToArray(entry, cJSON_CreateNumber((index == (PALETTE_ENTRY_COUNT - 1U)) ? 255 : (int) (index << 4)));
        cJSON_AddItemToArray(entry, cJSON_CreateNumber(colors[index][0]));
        cJSON_AddItemToArray(entry, cJSON_CreateNumber(colors[index][1]));
        cJSON_AddItemToArray(entry, cJSON_CreateNumber(colors[index][2]));
        cJSON_AddItemToArray(json, entry);
    }
}

static void serialize_palette_stops(cJSON *json, const palette_stop_t *stops, size_t count)
{
    for (size_t index = 0; index < count; ++index) {
        cJSON *entry = cJSON_CreateArray();
        cJSON_AddItemToArray(entry, cJSON_CreateNumber(stops[index].index));
        cJSON_AddItemToArray(entry, cJSON_CreateNumber(stops[index].red));
        cJSON_AddItemToArray(entry, cJSON_CreateNumber(stops[index].green));
        cJSON_AddItemToArray(entry, cJSON_CreateNumber(stops[index].blue));
        cJSON_AddItemToArray(json, entry);
    }
}

static cJSON *build_palettes_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *palette_map = cJSON_AddObjectToObject(root, "p");
    cJSON *items = cJSON_AddArrayToObject(root, "items");
    light_state_t state;
    uint8_t primary_colors[PALETTE_ENTRY_COUNT][3];

    snapshot_light_state(&state);
    for (size_t index = 0; index < PALETTE_ENTRY_COUNT; ++index) {
        primary_colors[index][0] = state.red;
        primary_colors[index][1] = state.green;
        primary_colors[index][2] = state.blue;
    }

    cJSON_AddNumberToObject(root, "m", 0);
    cJSON_AddNumberToObject(root, "selected", state.palette);
    cJSON_AddNumberToObject(root, "customStart", CUSTOM_PALETTE_START_ID);
    cJSON_AddNumberToObject(root, "customCount", CUSTOM_PALETTE_SLOT_COUNT);

    for (uint8_t palette_id = 0U; palette_id < PALETTE_COUNT; ++palette_id) {
        const uint8_t (*colors)[3] = primary_colors;
        const char *name = "Primary";
        bool editable = false;
        bool empty = false;
        bool circular = false;
        const palette_stop_t *stops = NULL;
        size_t stop_count = 0U;
        char generated_name[PALETTE_NAME_LENGTH];

        if (palette_id == 0U) {
            name = "Primary";
        } else if (palette_id < CUSTOM_PALETTE_START_ID) {
            colors = s_builtin_palettes[palette_id - 1U].colors;
            name = s_builtin_palettes[palette_id - 1U].name;
        } else {
            size_t slot = custom_palette_slot_from_id(palette_id);
            colors = s_custom_palettes[slot].colors;
            editable = true;
            empty = custom_palette_is_empty(&s_custom_palettes[slot]);
            circular = custom_palette_is_circular(&s_custom_palettes[slot]);
            if (empty) {
                snprintf(generated_name, sizeof(generated_name), "Custom %u", (unsigned) (slot + 1U));
                name = generated_name;
            } else {
                name = s_custom_palettes[slot].name;
                stops = s_custom_palettes[slot].stops;
                stop_count = custom_palette_stop_count(&s_custom_palettes[slot]);
            }
        }

        char key[8];
        snprintf(key, sizeof(key), "%u", palette_id);

        cJSON *palette_array = cJSON_AddArrayToObject(palette_map, key);
        if (!empty) {
            serialize_palette_colors(palette_array, colors);
        }

        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", palette_id);
        cJSON_AddStringToObject(item, "name", name);
        cJSON_AddBoolToObject(item, "editable", editable);
        cJSON_AddBoolToObject(item, "empty", empty);
        cJSON_AddBoolToObject(item, "circle", circular);
        cJSON *item_colors = cJSON_AddArrayToObject(item, "colors");
        if (!empty) {
            serialize_palette_colors(item_colors, colors);
        }
        if (editable && stops != NULL) {
            cJSON *item_stops = cJSON_AddArrayToObject(item, "stops");
            serialize_palette_stops(item_stops, stops, stop_count);
        }
        cJSON_AddItemToArray(items, item);
    }

    return root;
}

static bool parse_palette_stop_array(cJSON *entry, palette_stop_t *stop)
{
    if (!cJSON_IsArray(entry) || cJSON_GetArraySize(entry) < 2) {
        return false;
    }

    cJSON *index = cJSON_GetArrayItem(entry, 0);
    cJSON *value_a = cJSON_GetArrayItem(entry, 1);
    if (!cJSON_IsNumber(index)) {
        return false;
    }

    stop->index = clamp_u8(index->valueint);
    if (cJSON_IsString(value_a) && value_a->valuestring != NULL) {
        return parse_hex_color(value_a->valuestring, &stop->red, &stop->green, &stop->blue);
    }

    cJSON *value_b = cJSON_GetArrayItem(entry, 2);
    cJSON *value_c = cJSON_GetArrayItem(entry, 3);
    if (!cJSON_IsNumber(value_a) || !cJSON_IsNumber(value_b) || !cJSON_IsNumber(value_c)) {
        return false;
    }

    stop->red = clamp_u8(value_a->valueint);
    stop->green = clamp_u8(value_b->valueint);
    stop->blue = clamp_u8(value_c->valueint);
    return true;
}

static bool parse_palette_definition(cJSON *source, palette_stop_t *stops, uint8_t *stop_count)
{
    if (!cJSON_IsArray(source)) {
        return false;
    }

    uint8_t count = 0U;
    cJSON *first = cJSON_GetArrayItem(source, 0);

    if (cJSON_IsArray(first)) {
        int total = cJSON_GetArraySize(source);
        for (int index = 0; index < total && count < CUSTOM_PALETTE_MAX_STOPS; ++index) {
            if (parse_palette_stop_array(cJSON_GetArrayItem(source, index), &stops[count])) {
                ++count;
            }
        }
    } else {
        int total = cJSON_GetArraySize(source);
        int index = 0;
        while (index < total && count < CUSTOM_PALETTE_MAX_STOPS) {
            cJSON *position = cJSON_GetArrayItem(source, index++);
            if (!cJSON_IsNumber(position)) {
                break;
            }

            palette_stop_t stop = {
                .index = clamp_u8(position->valueint),
                .red = 0U,
                .green = 0U,
                .blue = 0U,
            };

            cJSON *value = cJSON_GetArrayItem(source, index++);
            if (cJSON_IsString(value) && value->valuestring != NULL) {
                if (parse_hex_color(value->valuestring, &stop.red, &stop.green, &stop.blue)) {
                    stops[count++] = stop;
                }
                continue;
            }

            cJSON *green = cJSON_GetArrayItem(source, index++);
            cJSON *blue = cJSON_GetArrayItem(source, index++);
            if (!cJSON_IsNumber(value) || !cJSON_IsNumber(green) || !cJSON_IsNumber(blue)) {
                break;
            }

            stop.red = clamp_u8(value->valueint);
            stop.green = clamp_u8(green->valueint);
            stop.blue = clamp_u8(blue->valueint);
            stops[count++] = stop;
        }
    }

    if (count == 0U) {
        return false;
    }

    *stop_count = count;
    return true;
}

static esp_err_t json_palettes_get_handler(httpd_req_t *req)
{
    cJSON *palettes = build_palettes_json();
    esp_err_t ret = send_json_response(req, palettes);
    cJSON_Delete(palettes);
    return ret;
}

static esp_err_t json_palettes_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= HTTP_RECV_BUFFER_SIZE) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Invalid body length");
    }

    char body[HTTP_RECV_BUFFER_SIZE];
    int total_read = 0;
    while (total_read < req->content_len) {
        int read_now = httpd_req_recv(req, body + total_read, req->content_len - total_read);
        if (read_now <= 0) {
            return httpd_resp_send_500(req);
        }
        total_read += read_now;
    }
    body[total_read] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "JSON parse failed");
    }

    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    cJSON *stops = cJSON_GetObjectItemCaseSensitive(root, "stops");
    cJSON *palette = cJSON_GetObjectItemCaseSensitive(root, "palette");
    cJSON *colors = cJSON_GetObjectItemCaseSensitive(root, "colors");
    cJSON *circle = cJSON_GetObjectItemCaseSensitive(root, "circle");
    cJSON *circular = cJSON_GetObjectItemCaseSensitive(root, "circular");
    cJSON *definition = cJSON_IsArray(stops) ? stops : (cJSON_IsArray(palette) ? palette : colors);

    if (!cJSON_IsNumber(id)) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Palette id is required");
    }

    uint8_t palette_id = clamp_palette(id->valueint);
    if (!is_custom_palette_id(palette_id)) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Palette id must refer to a custom palette");
    }

    custom_palette_t updated = {0};
    uint8_t stop_count = 0U;
    size_t slot = custom_palette_slot_from_id(palette_id);

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    updated = s_custom_palettes[slot];
    xSemaphoreGive(s_state_lock);

    if (!parse_palette_definition(definition, updated.stops, &stop_count)) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Palette definition is invalid");
    }

    bool circle_enabled = custom_palette_is_circular(&updated);
    cJSON *circle_value = (cJSON_IsBool(circle) || cJSON_IsNumber(circle)) ? circle : circular;
    if (cJSON_IsBool(circle_value)) {
        circle_enabled = cJSON_IsTrue(circle_value);
    } else if (cJSON_IsNumber(circle_value)) {
        circle_enabled = circle_value->valueint != 0;
    }

    custom_palette_set_stop_count(&updated, stop_count);
    custom_palette_set_circular(&updated, circle_enabled);
    if (cJSON_IsString(name) && name->valuestring != NULL && name->valuestring[0] != '\0') {
        strlcpy(updated.name, name->valuestring, sizeof(updated.name));
    }
    rebuild_custom_palette_colors(&updated);

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_custom_palettes[slot] = updated;
    xSemaphoreGive(s_state_lock);

    cJSON_Delete(root);
    if (save_custom_palettes_to_nvs() != ESP_OK) {
        return httpd_resp_send_500(req);
    }

    cJSON *response = build_palettes_json();
    esp_err_t ret = send_json_response(req, response);
    cJSON_Delete(response);
    return ret;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    set_common_response_headers(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    char portal_url[48];
    build_ap_root_url(portal_url, sizeof(portal_url));

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", portal_url);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "Redirecting to captive portal");
}

static esp_err_t captive_204_handler(httpd_req_t *req)
{
    char portal_url[48];
    build_ap_root_url(portal_url, sizeof(portal_url));

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", portal_url);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "Captive portal");
}

static esp_err_t captive_hotspot_detect_handler(httpd_req_t *req)
{
    char portal_url[48];
    build_ap_root_url(portal_url, sizeof(portal_url));

    set_common_response_headers(req, "text/html; charset=utf-8");
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_hdr(req, "Refresh", "0; url=/");

    char body[192];
    snprintf(body, sizeof(body),
             "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>"
             "Success <A HREF=\"%s\">Continue</A>."
             "</BODY></HTML>",
             portal_url);
    return httpd_resp_sendstr(req, body);
}

static esp_err_t captive_generate_204_handler(httpd_req_t *req)
{
    return captive_204_handler(req);
}

static esp_err_t captive_ncsi_handler(httpd_req_t *req)
{
    return captive_redirect_handler(req);
}

static esp_err_t captive_connecttest_handler(httpd_req_t *req)
{
    return captive_redirect_handler(req);
}

static esp_err_t captive_mobile_connect_handler(httpd_req_t *req)
{
    return captive_redirect_handler(req);
}

static esp_err_t captive_root_fallback_handler(httpd_req_t *req)
{
    return captive_redirect_handler(req);
}

static esp_err_t json_info_get_handler(httpd_req_t *req)
{
    cJSON *info = build_info_json();
    esp_err_t ret = send_json_response(req, info);
    cJSON_Delete(info);
    return ret;
}

static esp_err_t json_state_get_handler(httpd_req_t *req)
{
    cJSON *state = build_state_json();
    esp_err_t ret = send_json_response(req, state);
    cJSON_Delete(state);
    return ret;
}

static esp_err_t json_state_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= HTTP_RECV_BUFFER_SIZE) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Invalid body length");
    }

    char body[HTTP_RECV_BUFFER_SIZE];
    int total_read = 0;
    while (total_read < req->content_len) {
        int read_now = httpd_req_recv(req, body + total_read, req->content_len - total_read);
        if (read_now <= 0) {
            return httpd_resp_send_500(req);
        }
        total_read += read_now;
    }
    body[total_read] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "JSON parse failed");
    }

    light_state_t state;
    snapshot_light_state(&state);
    apply_json_state(root, &state);
    store_light_state(&state);
    cJSON_Delete(root);

    cJSON *response = build_state_json();
    esp_err_t ret = send_json_response(req, response);
    cJSON_Delete(response);
    return ret;
}

static void apply_query_value(light_state_t *state, const char *key, const char *value)
{
    if (strcmp(key, "T") == 0) {
        int toggle = atoi(value);
        if (toggle == 2) {
            state->on = !state->on;
        } else {
            state->on = toggle != 0;
        }
        return;
    }
    if (strcmp(key, "A") == 0) {
        state->brightness = clamp_u8(atoi(value));
        return;
    }
    if (strcmp(key, "R") == 0) {
        state->red = clamp_u8(atoi(value));
        return;
    }
    if (strcmp(key, "G") == 0) {
        state->green = clamp_u8(atoi(value));
        return;
    }
    if (strcmp(key, "B") == 0) {
        state->blue = clamp_u8(atoi(value));
        return;
    }
    if (strcmp(key, "FX") == 0) {
        state->effect = clamp_effect(atoi(value));
        return;
    }
    if (strcmp(key, "SX") == 0) {
        state->speed = clamp_u8(atoi(value));
        return;
    }
    if (strcmp(key, "FP") == 0) {
        state->palette = clamp_palette(atoi(value));
    }
}

static esp_err_t win_get_handler(httpd_req_t *req)
{
    int query_len = httpd_req_get_url_query_len(req);
    if (query_len > 0) {
        char query[256];
        if (query_len < (int) sizeof(query) && httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            light_state_t state;
            snapshot_light_state(&state);

            const char *keys[] = {"T", "A", "R", "G", "B", "FX", "SX", "FP"};
            char value[32];
            for (size_t index = 0; index < sizeof(keys) / sizeof(keys[0]); ++index) {
                if (httpd_query_key_value(query, keys[index], value, sizeof(value)) == ESP_OK) {
                    apply_query_value(&state, keys[index], value);
                }
            }

            store_light_state(&state);
        }
    }

    cJSON *response = build_state_json();
    esp_err_t ret = send_json_response(req, response);
    cJSON_Delete(response);
    return ret;
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 24;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_RETURN_ON_ERROR(httpd_start(&s_http_server, &config), TAG, "httpd_start failed");

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    const httpd_uri_t json_info = {
        .uri = "/json/info",
        .method = HTTP_GET,
        .handler = json_info_get_handler,
    };
    const httpd_uri_t json_state_get = {
        .uri = "/json/state",
        .method = HTTP_GET,
        .handler = json_state_get_handler,
    };
    const httpd_uri_t json_state_post = {
        .uri = "/json/state",
        .method = HTTP_POST,
        .handler = json_state_post_handler,
    };
    const httpd_uri_t json_palettes_get = {
        .uri = "/json/palettes",
        .method = HTTP_GET,
        .handler = json_palettes_get_handler,
    };
    const httpd_uri_t json_palettes_post = {
        .uri = "/json/palettes",
        .method = HTTP_POST,
        .handler = json_palettes_post_handler,
    };
    const httpd_uri_t win = {
        .uri = "/win",
        .method = HTTP_GET,
        .handler = win_get_handler,
    };
    const httpd_uri_t captive_hotspot_detect = {
        .uri = "/hotspot-detect.html",
        .method = HTTP_GET,
        .handler = captive_hotspot_detect_handler,
    };
    const httpd_uri_t captive_generate_204 = {
        .uri = "/generate_204",
        .method = HTTP_GET,
        .handler = captive_generate_204_handler,
    };
    const httpd_uri_t captive_gen_204 = {
        .uri = "/gen_204",
        .method = HTTP_GET,
        .handler = captive_generate_204_handler,
    };
    const httpd_uri_t captive_ncsi = {
        .uri = "/ncsi.txt",
        .method = HTTP_GET,
        .handler = captive_ncsi_handler,
    };
    const httpd_uri_t captive_connecttest = {
        .uri = "/connecttest.txt",
        .method = HTTP_GET,
        .handler = captive_connecttest_handler,
    };
    const httpd_uri_t captive_mobile_connect = {
        .uri = "/mobile/status.php",
        .method = HTTP_GET,
        .handler = captive_mobile_connect_handler,
    };
    const httpd_uri_t captive_root_fallback = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = captive_root_fallback_handler,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &root), TAG, "register root handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &json_info), TAG, "register info handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &json_state_get), TAG, "register state GET handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &json_state_post), TAG, "register state POST handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &json_palettes_get), TAG, "register palettes GET handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &json_palettes_post), TAG, "register palettes POST handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &win), TAG, "register win handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &captive_hotspot_detect), TAG, "register captive hotspot handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &captive_generate_204), TAG, "register captive generate_204 handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &captive_gen_204), TAG, "register captive gen_204 handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &captive_ncsi), TAG, "register captive ncsi handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &captive_connecttest), TAG, "register captive connecttest handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &captive_mobile_connect), TAG, "register captive mobile status handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &captive_root_fallback), TAG, "register captive fallback handler failed");

    return ESP_OK;
}

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase failed");
        ret = nvs_flash_init();
    }
    return ret;
}

void app_main(void)
{
    ESP_ERROR_CHECK(init_nvs());

    s_state_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_state_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(load_custom_palettes_from_nvs());

    build_device_identity();

    ESP_LOGI(TAG, "Booting %s", s_device_name);
    ESP_LOGI(TAG, "Light ring: %d LEDs on GPIO%d", CONFIG_LIGHT_RING_LED_COUNT, CONFIG_LIGHT_RING_LED_GPIO);

    ESP_ERROR_CHECK(init_led_strip());
    ESP_ERROR_CHECK(init_wifi());
    ESP_ERROR_CHECK(start_captive_dns_server());
    ESP_ERROR_CHECK(start_http_server());

    ESP_LOGI(TAG, "Control UI ready at http://" IPSTR "/", IP2STR(&s_ap_ip));
}