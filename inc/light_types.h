#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"

/* Effect identifiers (order must match the UI <select> option values). */
typedef enum {
    EFFECT_SOLID = 0,
    EFFECT_BREATHE = 1,
    EFFECT_RAINBOW = 2,
    EFFECT_CHASE = 3,
    EFFECT_COLOR_WIPE = 4,
    EFFECT_TWINKLE = 5,
    EFFECT_SCANNER = 6,
    EFFECT_SPARKLE = 7,
    EFFECT_SWOOSH = 8,
    EFFECT_SWOOSH_REVERSE = 9,
    EFFECT_SHRINK = 10,
    EFFECT_ROTATION = 11,
    EFFECT_POINT = 12,
    EFFECT_BREATHE_EDGE = 13,
    EFFECT_CHASE_TRANSITION = 14,
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
    bool swoosh_reverse_cover;
    uint8_t swoosh_reverse_gap;
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
    uint8_t chase_background_palette;
    uint8_t chase_palette;
    uint8_t chase_length;
    uint8_t chase_transition_palette;
    uint8_t chase_transition_speed;
    bool chase_transition_running;
    uint8_t chase_transition_progress;
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

