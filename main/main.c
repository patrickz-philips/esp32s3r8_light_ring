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
#define CUSTOM_PALETTE_MAX_STOPS    8U
#define PALETTE_NAME_LENGTH         24U
#define CUSTOM_PALETTE_START_ID     (1U + BUILTIN_PALETTE_COUNT)
#define PALETTE_COUNT               (CUSTOM_PALETTE_START_ID + CUSTOM_PALETTE_SLOT_COUNT)
#define PALETTE_NVS_NAMESPACE       "palettes"
#define PALETTE_NVS_KEY             "catalog"

typedef enum {
    EFFECT_SOLID = 0,
    EFFECT_BREATHE,
    EFFECT_RAINBOW,
    EFFECT_CHASE,
    EFFECT_COLOR_WIPE,
    EFFECT_TWINKLE,
    EFFECT_SCANNER,
    EFFECT_SPARKLE,
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
    uint8_t brightness;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t effect;
    uint8_t speed;
    uint8_t palette;
    bool has_palette_cache;
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
    .brightness = CONFIG_LIGHT_RING_DEFAULT_BRIGHTNESS,
    .red = 255,
    .green = 120,
    .blue = 16,
    .effect = EFFECT_SOLID,
    .speed = 128,
    .palette = 0,
};

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

static void rebuild_custom_palette_colors(custom_palette_t *palette)
{
    if (palette->stop_count == 0U) {
        memset(palette->colors, 0, sizeof(palette->colors));
        return;
    }

    if (palette->stop_count > CUSTOM_PALETTE_MAX_STOPS) {
        palette->stop_count = CUSTOM_PALETTE_MAX_STOPS;
    }

    sort_palette_stops(palette->stops, palette->stop_count);

    for (size_t entry = 0; entry < PALETTE_ENTRY_COUNT; ++entry) {
        uint16_t target = (entry == (PALETTE_ENTRY_COUNT - 1U)) ? 255U : (uint16_t) (entry << 4);
        const palette_stop_t *left = &palette->stops[0];
        const palette_stop_t *right = &palette->stops[palette->stop_count - 1U];

        if (target <= left->index) {
            palette->colors[entry][0] = left->red;
            palette->colors[entry][1] = left->green;
            palette->colors[entry][2] = left->blue;
            continue;
        }

        if (target >= right->index) {
            palette->colors[entry][0] = right->red;
            palette->colors[entry][1] = right->green;
            palette->colors[entry][2] = right->blue;
            continue;
        }

        for (size_t stop = 1; stop < palette->stop_count; ++stop) {
            right = &palette->stops[stop];
            if (target > right->index) {
                left = right;
                continue;
            }

            uint16_t span = (uint16_t) (right->index - left->index);
            if (span == 0U) {
                palette->colors[entry][0] = right->red;
                palette->colors[entry][1] = right->green;
                palette->colors[entry][2] = right->blue;
                break;
            }

            uint16_t progress = (uint16_t) ((target - left->index) * 255U / span);
            uint16_t remain = 255U - progress;

            palette->colors[entry][0] = (uint8_t) ((((uint16_t) left->red * remain) + ((uint16_t) right->red * progress)) / 255U);
            palette->colors[entry][1] = (uint8_t) ((((uint16_t) left->green * remain) + ((uint16_t) right->green * progress)) / 255U);
            palette->colors[entry][2] = (uint8_t) ((((uint16_t) left->blue * remain) + ((uint16_t) right->blue * progress)) / 255U);
            break;
        }
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
    "    .editor-header { display:flex; gap:12px; align-items:center; justify-content:space-between; }\n"
    "    .editor-header > * { flex:1; }\n"
    "    .actions { display:flex; gap:10px; justify-content:flex-end; }\n"
    "    .actions button, .toolbar button { width:auto; }\n"
    "    .toolbar { display:flex; gap:10px; flex-wrap:wrap; }\n"
    "    .stop-list { display:grid; gap:10px; }\n"
    "    .stop-row { display:grid; gap:10px; grid-template-columns:88px minmax(0,1fr) auto; align-items:end; padding:12px; border:1px solid var(--line); border-radius:16px; background:rgba(255,255,255,0.76); }\n"
    "    .mini-label { font-size:12px; color:var(--muted); margin-bottom:6px; text-transform:uppercase; letter-spacing:0.08em; }\n"
    "    .color-fields { display:grid; gap:10px; grid-template-columns:120px repeat(3, minmax(0,1fr)); align-items:end; }\n"
    "    .color-fields input[type=number] { min-width:0; }\n"
    "    .stop-row button { width:auto; }\n"
    "    @media (max-width: 880px) { .page { grid-template-columns:1fr; } .actions { justify-content:flex-start; } }\n"
    "    @media (max-width: 640px) { .stop-row { grid-template-columns:1fr; } .color-fields { grid-template-columns:1fr 1fr; } }\n"
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
    "            <div class=\"pill\"><span>Status</span><strong id=\"powerLabel\">Unknown</strong></div>\n"
    "          </div>\n"
    "          <div class=\"grid\">\n"
    "            <div><label for=\"color\">Primary Color</label><input id=\"color\" type=\"color\" value=\"#ff7810\"></div>\n"
    "            <div><label for=\"brightness\">Brightness</label><input id=\"brightness\" type=\"range\" min=\"0\" max=\"255\" value=\"160\"></div>\n"
    "            <div><label for=\"effect\">Effect</label><select id=\"effect\"><option value=\"0\">Solid</option><option value=\"1\">Breathe</option><option value=\"2\">Rainbow</option><option value=\"3\">Chase</option><option value=\"4\">Color Wipe</option><option value=\"5\">Twinkle</option><option value=\"6\">Scanner</option><option value=\"7\">Sparkle</option></select></div>\n"
    "            <div><label for=\"palette\">Palette</label><select id=\"palette\"></select></div>\n"
    "            <div><label for=\"speed\">Speed</label><input id=\"speed\" type=\"range\" min=\"1\" max=\"255\" value=\"128\"></div>\n"
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
    "              <p class=\"note\">Built-ins are read-only. Four custom slots can be edited and saved to flash.</p>\n"
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
    "            <div>\n"
    "              <label for=\"customPaletteName\">Palette Name</label>\n"
    "              <input id=\"customPaletteName\" type=\"text\" maxlength=\"23\" placeholder=\"Custom palette name\">\n"
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
    "  </main>\n"
    "  <script>\n"
    "    const colorInput = document.getElementById('color');\n"
    "    const brightnessInput = document.getElementById('brightness');\n"
    "    const speedInput = document.getElementById('speed');\n"
    "    const effectInput = document.getElementById('effect');\n"
    "    const paletteInput = document.getElementById('palette');\n"
    "    const powerLabel = document.getElementById('powerLabel');\n"
    "    const deviceInfo = document.getElementById('deviceInfo');\n"
    "    const paletteGallery = document.getElementById('paletteGallery');\n"
    "    const editorHint = document.getElementById('editorHint');\n"
    "    const customPaletteName = document.getElementById('customPaletteName');\n"
    "    const stopList = document.getElementById('stopList');\n"
    "    const editorPreview = document.getElementById('editorPreview');\n"
    "    const addStopBtn = document.getElementById('addStopBtn');\n"
    "    const savePaletteBtn = document.getElementById('savePaletteBtn');\n"
    "    const newPaletteBtn = document.getElementById('newPaletteBtn');\n"
    "    const reloadPalettesBtn = document.getElementById('reloadPalettesBtn');\n"
    "    let lastOn = true;\n"
    "    let paletteCatalog = [];\n"
    "    let selectedPaletteId = 0;\n"
    "    let editingPaletteId = null;\n"
    "    let editingStops = [];\n"
    "    function rgbToHex(rgb) { return '#' + rgb.map(v => Number(v).toString(16).padStart(2, '0')).join(''); }\n"
    "    function hexToRgb(hex) { const value = hex.replace('#', ''); return [parseInt(value.slice(0,2),16), parseInt(value.slice(2,4),16), parseInt(value.slice(4,6),16)]; }\n"
    "    function normalizeStops(stops) { return (Array.isArray(stops) ? stops : []).map(stop => ({ index: Number(stop[0] ?? stop.index ?? 0), rgb: Array.isArray(stop) ? [Number(stop[1] ?? 0), Number(stop[2] ?? 0), Number(stop[3] ?? 0)] : [Number(stop.red ?? 0), Number(stop.green ?? 0), Number(stop.blue ?? 0)] })).sort((a, b) => a.index - b.index); }\n"
    "    function seedPaletteStops() { return [{ index: 0, rgb: hexToRgb(colorInput ? colorInput.value : '#ff7810') }, { index: 255, rgb: [255, 255, 255] }]; }\n"
    "    function nextEmptyCustomPalette() { return paletteCatalog.find(item => item.editable && item.empty); }\n"
    "    function renderPreview(target, colors) { if (!target) return; target.innerHTML = ''; (colors || []).forEach(entry => { const swatch = document.createElement('div'); const rgb = Array.isArray(entry) ? entry.slice(1,4) : entry.rgb; swatch.style.background = `rgb(${rgb[0]}, ${rgb[1]}, ${rgb[2]})`; target.appendChild(swatch); }); }\n"
    "    function renderPaletteSelect() { if (!paletteInput) return; const current = String(selectedPaletteId); paletteInput.innerHTML = paletteCatalog.filter(item => !item.empty).map(item => `<option value=\"${item.id}\">${item.name}</option>`).join(''); if ([...paletteInput.options].some(option => option.value === current)) paletteInput.value = current; }\n"
    "    function activatePaletteCard(id) { if (!paletteGallery) return; [...paletteGallery.children].forEach(card => card.classList.toggle('active', Number(card.dataset.id) === Number(id))); }\n"
    "    function renderStopEditor() { if (!stopList || !editorPreview || !editorHint || !customPaletteName) return; stopList.innerHTML = ''; const current = paletteCatalog.find(item => Number(item.id) === Number(editingPaletteId)); if (!current) { editorHint.textContent = 'Select a custom palette card or create a new one.'; customPaletteName.value = ''; renderPreview(editorPreview, []); return; } editorHint.textContent = current.empty ? `Creating ${current.name}. Give it a name and tune its color stops.` : `Editing custom palette ${current.name}. Stops are saved as WLED-style [index,r,g,b] entries.`; if (!current.empty && customPaletteName.value === '') customPaletteName.value = current.name; const previewColors = editingStops.map(stop => [stop.index, ...stop.rgb]); renderPreview(editorPreview, previewColors.length ? previewColors : ((current.stops && current.stops.length) ? current.stops : (current.colors || []))); editingStops.forEach((stop, index) => { const row = document.createElement('div'); row.className = 'stop-row'; row.innerHTML = `<div><div class=\"mini-label\">Index</div><input type=\"number\" min=\"0\" max=\"255\" value=\"${stop.index}\" data-role=\"index\" data-index=\"${index}\"></div><div><div class=\"mini-label\">Color</div><div class=\"color-fields\"><input type=\"color\" value=\"${rgbToHex(stop.rgb)}\" data-role=\"color\" data-index=\"${index}\"><div><div class=\"mini-label\">R</div><input type=\"number\" min=\"0\" max=\"255\" value=\"${stop.rgb[0]}\" data-role=\"red\" data-index=\"${index}\"></div><div><div class=\"mini-label\">G</div><input type=\"number\" min=\"0\" max=\"255\" value=\"${stop.rgb[1]}\" data-role=\"green\" data-index=\"${index}\"></div><div><div class=\"mini-label\">B</div><input type=\"number\" min=\"0\" max=\"255\" value=\"${stop.rgb[2]}\" data-role=\"blue\" data-index=\"${index}\"></div></div></div><button type=\"button\" class=\"ghost\" data-role=\"remove\" data-index=\"${index}\">Remove</button>`; stopList.appendChild(row); }); }\n"
    "    function renderPaletteGallery() { if (!paletteGallery) return; paletteGallery.innerHTML = ''; paletteCatalog.filter(item => !item.empty).forEach(item => { const card = document.createElement('button'); card.type = 'button'; card.className = `palette-card${item.editable ? ' editable' : ''}${Number(item.id) === Number(selectedPaletteId) ? ' active' : ''}`; card.dataset.id = item.id; const preview = document.createElement('div'); preview.className = 'palette-preview'; (item.colors || []).forEach(entry => { const swatch = document.createElement('div'); const rgb = Array.isArray(entry) ? entry.slice(1,4) : entry.rgb; swatch.style.background = `rgb(${rgb[0]}, ${rgb[1]}, ${rgb[2]})`; preview.appendChild(swatch); }); const meta = document.createElement('div'); meta.className = 'palette-meta'; meta.innerHTML = `<strong>${item.name}</strong><span>${item.editable ? 'Custom editable palette' : 'Built-in palette'}</span>`; card.append(preview, meta); card.addEventListener('click', () => { selectedPaletteId = Number(item.id); if (paletteInput) paletteInput.value = String(selectedPaletteId); activatePaletteCard(selectedPaletteId); if (item.editable) { loadPaletteEditor(item.id); } else { editingPaletteId = null; editingStops = []; renderStopEditor(); } }); paletteGallery.appendChild(card); }); if (newPaletteBtn) { const next = nextEmptyCustomPalette(); newPaletteBtn.disabled = !next; newPaletteBtn.textContent = next ? 'New Custom Palette' : 'Custom Slots Full'; } }\n"
    "    function loadPaletteEditor(id) { const item = paletteCatalog.find(entry => Number(entry.id) === Number(id)); if (!item || !item.editable) return; editingPaletteId = Number(id); customPaletteName.value = item.empty ? '' : item.name; editingStops = item.empty ? seedPaletteStops() : normalizeStops(item.stops && item.stops.length ? item.stops : item.colors).slice(0, 8); renderStopEditor(); }\n"
    "    function startNewPalette() { const item = nextEmptyCustomPalette(); if (!item) { editorHint.textContent = 'All custom palette slots are already in use.'; return; } loadPaletteEditor(item.id); }\n"
    "    async function loadInfo() { const response = await fetch('/json/info'); const info = await response.json(); deviceInfo.textContent = `${info.name} | AP ${info.wifi.ap_ssid} | LEDs ${info.led.count} @ GPIO${info.led.gpio}`; }\n"
    "    async function loadPalettes() { const response = await fetch('/json/palettes'); const json = await response.json(); paletteCatalog = Array.isArray(json.items) ? json.items : []; renderPaletteSelect(); renderPaletteGallery(); if (editingPaletteId !== null) { loadPaletteEditor(editingPaletteId); } else { renderStopEditor(); } }\n"
    "    async function loadState() { const response = await fetch('/json/state'); const state = await response.json(); const seg = state.seg && state.seg[0] ? state.seg[0] : {}; const color = seg.col && seg.col[0] ? seg.col[0] : state.color; lastOn = !!state.on; selectedPaletteId = Number(seg.pal ?? state.pal ?? state.palette ?? 0); powerLabel.textContent = lastOn ? 'ON' : 'OFF'; brightnessInput.value = state.bri ?? 0; speedInput.value = seg.sx ?? state.speed ?? 128; effectInput.value = seg.fx ?? state.fx ?? 0; if (paletteInput) paletteInput.value = String(selectedPaletteId); activatePaletteCard(selectedPaletteId); const selected = paletteCatalog.find(item => Number(item.id) === Number(selectedPaletteId)); if (selected && selected.editable) { if (editingPaletteId !== selected.id) loadPaletteEditor(selected.id); } else { editingPaletteId = null; editingStops = []; renderStopEditor(); } if (Array.isArray(color)) { colorInput.value = rgbToHex(color); } }\n"
    "    async function applyState() { const [r, g, b] = hexToRgb(colorInput.value); const payload = { on: true, bri: Number(brightnessInput.value), color: [r, g, b], effect: Number(effectInput.value), speed: Number(speedInput.value), palette: Number(paletteInput.value) }; await fetch('/json/state', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) }); await loadState(); }\n"
    "    async function savePalette() { if (editingPaletteId === null) { editorHint.textContent = 'Select a custom palette before saving.'; return; } if (!editingStops.length) { editorHint.textContent = 'Add at least one color stop before saving.'; return; } const payload = { id: Number(editingPaletteId), name: customPaletteName.value.trim() || 'Custom Palette', stops: editingStops.map(stop => [Number(stop.index), ...stop.rgb]) }; await fetch('/json/palettes', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) }); await loadPalettes(); await loadState(); loadPaletteEditor(editingPaletteId); }\n"
    "    document.getElementById('applyBtn').addEventListener('click', applyState);\n"
    "    document.getElementById('refreshBtn').addEventListener('click', loadState);\n"
    "    document.getElementById('toggleBtn').addEventListener('click', async () => { await fetch(`/win?T=${lastOn ? 0 : 1}`); await loadState(); });\n"
    "    paletteInput.addEventListener('change', () => { selectedPaletteId = Number(paletteInput.value); activatePaletteCard(selectedPaletteId); const selected = paletteCatalog.find(item => Number(item.id) === Number(selectedPaletteId)); if (selected && selected.editable) { loadPaletteEditor(selected.id); } else { editingPaletteId = null; editingStops = []; renderStopEditor(); } });\n"
    "    stopList.addEventListener('input', event => { const target = event.target; const index = Number(target.dataset.index); if (Number.isNaN(index) || !editingStops[index]) return; if (target.dataset.role === 'index') { editingStops[index].index = Math.max(0, Math.min(255, Number(target.value) || 0)); editingStops.sort((a, b) => a.index - b.index); renderStopEditor(); return; } if (target.dataset.role === 'color') { const rgb = hexToRgb(target.value); editingStops[index].rgb = rgb; const row = target.closest('.color-fields'); if (row) { const redInput = row.querySelector('[data-role=red]'); const greenInput = row.querySelector('[data-role=green]'); const blueInput = row.querySelector('[data-role=blue]'); if (redInput) redInput.value = String(rgb[0]); if (greenInput) greenInput.value = String(rgb[1]); if (blueInput) blueInput.value = String(rgb[2]); } renderPreview(editorPreview, editingStops.map(stop => [stop.index, ...stop.rgb])); return; } if (target.dataset.role === 'red' || target.dataset.role === 'green' || target.dataset.role === 'blue') { const channelMap = { red: 0, green: 1, blue: 2 }; const channel = channelMap[target.dataset.role]; editingStops[index].rgb[channel] = Math.max(0, Math.min(255, Number(target.value) || 0)); const row = target.closest('.color-fields'); const colorField = row ? row.querySelector('[data-role=color]') : null; if (colorField) colorField.value = rgbToHex(editingStops[index].rgb); renderPreview(editorPreview, editingStops.map(stop => [stop.index, ...stop.rgb])); } });\n"
    "    stopList.addEventListener('click', event => { const target = event.target; if (target.dataset.role !== 'remove') return; const index = Number(target.dataset.index); if (!Number.isNaN(index)) { editingStops.splice(index, 1); renderStopEditor(); } });\n"
    "    addStopBtn.addEventListener('click', () => { if (editingPaletteId === null) { editorHint.textContent = 'Select a custom palette first.'; return; } if (editingStops.length >= 8) return; editingStops.push({ index: editingStops.length ? Math.min(255, editingStops[editingStops.length - 1].index + 32) : 0, rgb: [255, 255, 255] }); renderStopEditor(); });\n"
    "    savePaletteBtn.addEventListener('click', savePalette);\n"
    "    newPaletteBtn.addEventListener('click', startNewPalette);\n"
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
    return palette->stop_count == 0U && palette->name[0] == '\0';
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
    return clamp_effect(atoi(value));
}

static uint8_t scale_component(uint8_t value, uint8_t brightness)
{
    return (uint8_t) (((uint16_t) value * (uint16_t) brightness) / 255U);
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
                                   uint8_t *red, uint8_t *green, uint8_t *blue)
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

    const uint8_t *to = colors[(hi4 + 1U) & 0x0FU];
    uint16_t blend_to = ((uint16_t) lo4) << 4;
    uint16_t blend_from = 256U - blend_to;

    *red = (uint8_t) ((((uint16_t) from[0] * blend_from) + ((uint16_t) to[0] * blend_to)) >> 8);
    *green = (uint8_t) ((((uint16_t) from[1] * blend_from) + ((uint16_t) to[1] * blend_to)) >> 8);
    *blue = (uint8_t) ((((uint16_t) from[2] * blend_from) + ((uint16_t) to[2] * blend_to)) >> 8);
}

static uint8_t led_index_to_palette_index(uint16_t index)
{
    return (uint8_t) ((((uint32_t) index) * 256U) / CONFIG_LIGHT_RING_LED_COUNT);
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

    sample_palette_entries(s_builtin_palettes[palette_id - 1U].colors, index, red, green, blue);
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

    if (state->has_palette_cache) {
        uint8_t cached_red;
        uint8_t cached_green;
        uint8_t cached_blue;
        sample_palette_entries(state->palette_cache, palette_index, &cached_red, &cached_green, &cached_blue);
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
    set_state_palette_level(index, state, led_index_to_palette_index(index), level);
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
    memset(state->palette_cache, 0, sizeof(state->palette_cache));
    memset(state->palette_label, 0, sizeof(state->palette_label));

    if (is_custom_palette_id(state->palette)) {
        size_t slot = custom_palette_slot_from_id(state->palette);
        memcpy(state->palette_cache, s_custom_palettes[slot].colors, sizeof(state->palette_cache));
        strlcpy(state->palette_label, s_custom_palettes[slot].name, sizeof(state->palette_label));
        state->has_palette_cache = true;
    } else {
        strlcpy(state->palette_label, palette_name(state->palette), sizeof(state->palette_label));
    }

    xSemaphoreGive(s_state_lock);
}

static void store_light_state(const light_state_t *state)
{
    light_state_t sanitized = *state;
    sanitized.has_palette_cache = false;
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
            uint8_t palette_index = (uint8_t) (offset + led_index_to_palette_index(index));
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
        render_frame(&state, frame++);
        if (transmit_pixels() != ESP_OK) {
            ESP_LOGW(TAG, "Failed to flush frame to LED ring");
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

    ESP_LOGI(TAG, "Access point ready: SSID=%s", s_ap_ssid);
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
    cJSON *bri = cJSON_GetObjectItemCaseSensitive(root, "bri");
    cJSON *fx = cJSON_GetObjectItemCaseSensitive(root, "fx");
    cJSON *effect = cJSON_GetObjectItemCaseSensitive(root, "effect");
    cJSON *sx = cJSON_GetObjectItemCaseSensitive(root, "sx");
    cJSON *speed = cJSON_GetObjectItemCaseSensitive(root, "speed");
    cJSON *pal = cJSON_GetObjectItemCaseSensitive(root, "pal");
    cJSON *palette = cJSON_GetObjectItemCaseSensitive(root, "palette");
    cJSON *color = cJSON_GetObjectItemCaseSensitive(root, "color");
    cJSON *segments = cJSON_GetObjectItemCaseSensitive(root, "seg");

    if (cJSON_IsBool(on)) {
        state->on = cJSON_IsTrue(on);
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
    cJSON_AddNumberToObject(root, "bri", state.brightness);
    cJSON_AddNumberToObject(root, "fx", state.effect);
    cJSON_AddNumberToObject(root, "speed", state.speed);
    cJSON_AddNumberToObject(root, "pal", state.palette);
    cJSON_AddNumberToObject(root, "palette", state.palette);
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
            if (empty) {
                snprintf(generated_name, sizeof(generated_name), "Custom %u", (unsigned) (slot + 1U));
                name = generated_name;
            } else {
                name = s_custom_palettes[slot].name;
                stops = s_custom_palettes[slot].stops;
                stop_count = s_custom_palettes[slot].stop_count;
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

    updated.stop_count = stop_count;
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
    config.max_uri_handlers = 10;
    config.stack_size = 8192;

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

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &root), TAG, "register root handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &json_info), TAG, "register info handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &json_state_get), TAG, "register state GET handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &json_state_post), TAG, "register state POST handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &json_palettes_get), TAG, "register palettes GET handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &json_palettes_post), TAG, "register palettes POST handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &win), TAG, "register win handler failed");

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
    ESP_ERROR_CHECK(start_http_server());

    ESP_LOGI(TAG, "Control UI ready at http://192.168.4.1/");
}