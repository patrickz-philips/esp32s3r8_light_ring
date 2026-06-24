#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"

/* Effect identifiers (order must match the UI <select> option values). */
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
    EFFECT_POINT,
    EFFECT_BREATHE_EDGE,
    EFFECT_COUNT,
} effect_id_t;

/* A 16-entry blended lookup palette baked at build time. */
typedef struct {
    const char *name;
    uint8_t colors[PALETTE_ENTRY_COUNT][3];
} builtin_palette_t;

/* A single WLED-style gradient stop. */
typedef struct {
    uint8_t index;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} palette_stop_t;

/* A user-editable custom palette (stops + cached 16-entry colors). */
typedef struct {
    uint8_t stop_count;
    char name[PALETTE_NAME_LENGTH];
    palette_stop_t stops[CUSTOM_PALETTE_MAX_STOPS];
    uint8_t colors[PALETTE_ENTRY_COUNT][3];
} custom_palette_t;

/* The complete render state for the ring. */

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
    uint8_t point_palette;
    uint8_t point_background_palette;
    uint8_t point_length;
    uint8_t point_total_length;
    uint8_t point_repeat;
    uint8_t point_gap;
    uint8_t edge_palette;
    uint8_t edge_length;
    uint8_t edge_repeat;
    uint8_t edge_gap;
    bool has_palette_cache;
    uint8_t palette_stop_count;
    bool palette_circular;
    palette_stop_t palette_stops[CUSTOM_PALETTE_MAX_STOPS];
    char palette_label[PALETTE_NAME_LENGTH];
    uint8_t palette_cache[PALETTE_ENTRY_COUNT][3];
} light_state_t;

/* A saved scenario: a name plus a serialized state payload. */
typedef struct {
    char name[SCENARIO_NAME_LENGTH];
    char payload[SCENARIO_PAYLOAD_LENGTH];
} scenario_t;

