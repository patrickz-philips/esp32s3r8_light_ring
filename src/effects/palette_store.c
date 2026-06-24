#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs.h"

#include "app_config.h"
#include "http_server.h"
#include "light_effects.h"
#include "light_types.h"
#include "palette_store.h"

static const char *TAG = "palette_store";

custom_palette_t s_custom_palettes[CUSTOM_PALETTE_SLOT_COUNT];

// WLED-inspired fixed palettes sampled with a 16-entry blended lookup.
static const builtin_palette_t s_builtin_palettes[BUILTIN_PALETTE_COUNT] = {
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
        .name = "Good Morning",
        .stops = {
            {0, 105, 109, 118},
            {64, 173, 170, 166},
            {128, 186, 158, 120},
            {192, 196, 143, 77},
            {255, 194, 107, 51},
        },
    },
    {
        .stop_count = 5,
        .name = "Good Evening",
        .stops = {
            {0, 115, 128, 142},
            {64, 140, 156, 182},
            {128, 72, 80, 161},
            {192, 62, 80, 170},
            {255, 90, 64, 158},
        },
    },
    {
        .stop_count = 3,
        .name = "Sensitive",
        .stops = {
            {0, 103, 147, 137},
            {128, 164, 171, 143},
            {255, 94, 115, 120},
        },
    },
    {
        .stop_count = 3,
        .name = "Intense",
        .stops = {
            {0, 130, 58, 55},
            {128, 120, 76, 60},
            {255, 124, 48, 59},
        },
    },
    {
        .stop_count = 3,
        .name = "Regular",
        .stops = {
            {0, 49, 68, 101},
            {128, 72, 85, 105},
            {255, 89, 110, 111},
        },
    },
    {
        .stop_count = 3,
        .name = "Foam",
        .stops = {
            {0, 104, 121, 154},
            {128, 102, 90, 118},
            {255, 71, 69, 92},
        },
    },
};


uint8_t custom_palette_stop_count(const custom_palette_t *palette)
{
    return (uint8_t) (palette->stop_count & CUSTOM_PALETTE_STOP_COUNT_MASK);
}

bool custom_palette_is_circular(const custom_palette_t *palette)
{
    return (palette->stop_count & CUSTOM_PALETTE_CIRCULAR_FLAG) != 0U;
}

void custom_palette_set_stop_count(custom_palette_t *palette, uint8_t stop_count)
{
    palette->stop_count = (uint8_t) ((palette->stop_count & CUSTOM_PALETTE_CIRCULAR_FLAG) |
        (stop_count & CUSTOM_PALETTE_STOP_COUNT_MASK));
}

void custom_palette_set_circular(custom_palette_t *palette, bool circular)
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

void sample_custom_palette_stops(const palette_stop_t *stops, size_t count, bool circular,
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

void rebuild_custom_palette_colors(custom_palette_t *palette)
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

esp_err_t load_custom_palettes_from_nvs(void)
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

esp_err_t save_custom_palettes_to_nvs(void)
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

const char *palette_name(uint8_t palette)
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

bool is_custom_palette_id(uint8_t palette)
{
    return palette >= CUSTOM_PALETTE_START_ID && palette < PALETTE_COUNT;
}

bool custom_palette_is_empty(const custom_palette_t *palette)
{
    return custom_palette_stop_count(palette) == 0U && palette->name[0] == '\0';
}

size_t custom_palette_slot_from_id(uint8_t palette)
{
    return (size_t) (palette - CUSTOM_PALETTE_START_ID);
}

uint8_t clamp_palette(int value)
{
    if (value < 0) {
        return 0U;
    }
    if (value >= (int) PALETTE_COUNT) {
        return (uint8_t) (PALETTE_COUNT - 1U);
    }
    return (uint8_t) value;
}

void sample_palette_entries(const uint8_t colors[PALETTE_ENTRY_COUNT][3], uint8_t index,
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

void sample_builtin_palette(uint8_t palette, uint8_t index,
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

esp_err_t json_palettes_get_handler(httpd_req_t *req)
{
    cJSON *palettes = build_palettes_json();
    esp_err_t ret = send_json_response(req, palettes);
    cJSON_Delete(palettes);
    return ret;
}

esp_err_t json_palettes_post_handler(httpd_req_t *req)
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

cJSON *export_custom_palettes_json(void)
{
    cJSON *array = cJSON_CreateArray();
    if (array == NULL) {
        return NULL;
    }

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    for (size_t slot = 0; slot < CUSTOM_PALETTE_SLOT_COUNT; ++slot) {
        const custom_palette_t *palette = &s_custom_palettes[slot];
        if (custom_palette_is_empty(palette)) {
            continue;
        }

        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", (int) (CUSTOM_PALETTE_START_ID + slot));
        cJSON_AddStringToObject(item, "name", palette->name);
        cJSON_AddBoolToObject(item, "circle", custom_palette_is_circular(palette));
        cJSON *stops = cJSON_AddArrayToObject(item, "stops");
        serialize_palette_stops(stops, palette->stops, custom_palette_stop_count(palette));
        cJSON_AddItemToArray(array, item);
    }
    xSemaphoreGive(s_state_lock);

    return array;
}

esp_err_t import_custom_palettes_json(const cJSON *array)
{
    if (!cJSON_IsArray(array)) {
        return ESP_ERR_INVALID_ARG;
    }

    custom_palette_t imported[CUSTOM_PALETTE_SLOT_COUNT];
    memset(imported, 0, sizeof(imported));

    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, array) {
        if (!cJSON_IsObject(entry)) {
            continue;
        }

        cJSON *id = cJSON_GetObjectItemCaseSensitive(entry, "id");
        cJSON *name = cJSON_GetObjectItemCaseSensitive(entry, "name");
        cJSON *circle = cJSON_GetObjectItemCaseSensitive(entry, "circle");
        cJSON *stops = cJSON_GetObjectItemCaseSensitive(entry, "stops");
        if (!cJSON_IsNumber(id)) {
            continue;
        }

        uint8_t palette_id = clamp_palette(id->valueint);
        if (!is_custom_palette_id(palette_id)) {
            continue;
        }

        custom_palette_t palette = {0};
        uint8_t stop_count = 0U;
        if (!parse_palette_definition(stops, palette.stops, &stop_count)) {
            continue;
        }

        custom_palette_set_stop_count(&palette, stop_count);

        bool circle_enabled = false;
        if (cJSON_IsBool(circle)) {
            circle_enabled = cJSON_IsTrue(circle);
        } else if (cJSON_IsNumber(circle)) {
            circle_enabled = circle->valueint != 0;
        }
        custom_palette_set_circular(&palette, circle_enabled);

        size_t slot = custom_palette_slot_from_id(palette_id);
        if (cJSON_IsString(name) && name->valuestring != NULL && name->valuestring[0] != '\0') {
            strlcpy(palette.name, name->valuestring, sizeof(palette.name));
        } else {
            snprintf(palette.name, sizeof(palette.name), "Custom %u", (unsigned) (slot + 1U));
        }
        rebuild_custom_palette_colors(&palette);
        imported[slot] = palette;
    }

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    memcpy(s_custom_palettes, imported, sizeof(s_custom_palettes));
    xSemaphoreGive(s_state_lock);

    return save_custom_palettes_to_nvs();
}

