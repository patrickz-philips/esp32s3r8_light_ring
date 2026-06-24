#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

#include "light_types.h"

/* Custom palette slots, guarded by the light-effects state lock. */
extern custom_palette_t s_custom_palettes[CUSTOM_PALETTE_SLOT_COUNT];

uint8_t custom_palette_stop_count(const custom_palette_t *palette);
bool custom_palette_is_circular(const custom_palette_t *palette);
void custom_palette_set_stop_count(custom_palette_t *palette, uint8_t stop_count);
void custom_palette_set_circular(custom_palette_t *palette, bool circular);
void rebuild_custom_palette_colors(custom_palette_t *palette);

esp_err_t load_custom_palettes_from_nvs(void);
esp_err_t save_custom_palettes_to_nvs(void);

const char *palette_name(uint8_t palette);
bool is_custom_palette_id(uint8_t palette);
bool custom_palette_is_empty(const custom_palette_t *palette);
size_t custom_palette_slot_from_id(uint8_t palette);
uint8_t clamp_palette(int value);

void sample_palette_entries(const uint8_t colors[PALETTE_ENTRY_COUNT][3], uint8_t index,
                            bool wrap, uint8_t *red, uint8_t *green, uint8_t *blue);
void sample_builtin_palette(uint8_t palette, uint8_t index,
                            uint8_t *red, uint8_t *green, uint8_t *blue);
void sample_custom_palette_stops(const palette_stop_t *stops, size_t count, bool circular,
                                 uint8_t target, uint8_t *red, uint8_t *green, uint8_t *blue);

esp_err_t json_palettes_get_handler(httpd_req_t *req);
esp_err_t json_palettes_post_handler(httpd_req_t *req);

cJSON *export_custom_palettes_json(void);
esp_err_t import_custom_palettes_json(const cJSON *array);
