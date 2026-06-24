#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_config.h"
#include "board_led.h"
#include "light_effects.h"
#include "light_types.h"
#include "palette_store.h"

static const char *TAG = "light_effects";

SemaphoreHandle_t s_state_lock;

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
    .swoosh_left_palette = 5,
    .swoosh_right_palette = 6,
    .swoosh_left_stops = 5,
    .swoosh_right_stops = 5,
    .shrink_background_palette = 0,
    .shrink_bar_palette = 5,
    .shrink_length = 4,
    .shrink_gap = 6,
    .rotation_background_palette = 0,
    .rotation_palette = 5,
    .rotation_length = 2,
    .rotation_gap = 6,
    .rotation_counterclockwise = false,
    .point_palette = 5,
    .point_background_palette = 0,
    .point_length = 3,
    .point_total_length = 14,
    .point_repeat = 1,
    .point_gap = 8,
    .edge_palette = 5,
    .edge_length = 2,
    .edge_repeat = 1,
    .edge_gap = 8,
};


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
    case EFFECT_POINT:
        return "point";
    case EFFECT_BREATHE_EDGE:
        return "breathe_edge";
    default:
        return "solid";
    }
}

uint8_t clamp_u8(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (uint8_t) value;
}

effect_id_t clamp_effect(int value)
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
    if (strcasecmp(value, "point") == 0) {
        return EFFECT_POINT;
    }
    if (strcasecmp(value, "breathe_edge") == 0 || strcasecmp(value, "breathe edge") == 0 ||
        strcasecmp(value, "edge") == 0) {
        return EFFECT_BREATHE_EDGE;
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

static uint8_t clamp_point_length(int value)
{
    if (value < 1) {
        return 1U;
    }
    if (value > (int) POINT_ARM_LENGTH) {
        return (uint8_t) POINT_ARM_LENGTH;
    }
    return (uint8_t) value;
}

static uint8_t clamp_point_total(int value)
{
    if (value < 1) {
        return 1U;
    }
    if (value > (int) POINT_ARM_LENGTH) {
        return (uint8_t) POINT_ARM_LENGTH;
    }
    return (uint8_t) value;
}

static uint8_t clamp_point_repeat(int value)
{
    if (value < (int) POINT_MIN_REPEAT) {
        return (uint8_t) POINT_MIN_REPEAT;
    }
    if (value > (int) POINT_MAX_REPEAT) {
        return (uint8_t) POINT_MAX_REPEAT;
    }
    return (uint8_t) value;
}

static uint8_t clamp_point_gap(int value)
{
    if (value < 0) {
        return 0U;
    }
    if (value > 60) {
        return 60U;
    }
    return (uint8_t) value;
}

static uint8_t clamp_edge_length(int value)
{
    if (value < 1) {
        return 1U;
    }
    if (value > (int) EDGE_MAX_LENGTH) {
        return (uint8_t) EDGE_MAX_LENGTH;
    }
    return (uint8_t) value;
}

static uint8_t clamp_edge_repeat(int value)
{
    if (value < (int) EDGE_MIN_REPEAT) {
        return (uint8_t) EDGE_MIN_REPEAT;
    }
    if (value > (int) EDGE_MAX_REPEAT) {
        return (uint8_t) EDGE_MAX_REPEAT;
    }
    return (uint8_t) value;
}

static uint8_t clamp_edge_gap(int value)
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

void snapshot_light_state(light_state_t *state)
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

void store_light_state(const light_state_t *state)
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
    case EFFECT_POINT:
        return speed_to_delay(state->speed, 30U, 360U);
    case EFFECT_BREATHE_EDGE:
        return speed_to_delay(state->speed, 18U, 42U);
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

/* Maps a position along one of the two abstract arms onto a physical LED.
   The single 1..N strip is split into two arms that share the apex (LED
   POINT_APEX). pos 0 == apex; pos grows toward the arm's tail.
   Arm 0 climbs toward the higher-numbered tail (… -> LED N-2).
   Arm 1 descends toward LED 0 and then wraps to the final LED (N-1). */
static uint16_t point_arm_led(uint8_t arm, uint8_t pos)
{
    const uint16_t apex = (uint16_t) POINT_APEX;
    if (arm == 0U) {
        return (uint16_t) (apex + pos);
    }
    if ((uint16_t) pos <= apex) {
        return (uint16_t) (apex - pos);
    }
    return (uint16_t) ((CONFIG_LIGHT_RING_LED_COUNT - 1U) - ((uint16_t) pos - apex - 1U));
}

static void render_point(const light_state_t *state, uint32_t frame)
{
    light_state_t background;
    light_state_t point;
    load_swoosh_palette_context(&background, state, state->point_background_palette);
    load_swoosh_palette_context(&point, state, state->point_palette);

    /* Paint the whole ring with the background palette first; the two moving
       points are drawn on top of it. */
    for (uint16_t index = 0; index < CONFIG_LIGHT_RING_LED_COUNT; ++index) {
        set_state_pixel_level(index, &background, 255U);
    }

    const uint8_t arm_len = (uint8_t) POINT_ARM_LENGTH;
    uint8_t total = clamp_point_total(state->point_total_length);
    if (total > arm_len) {
        total = arm_len;
    }
    uint8_t pt = clamp_point_length(state->point_length);
    if (pt > total) {
        pt = total;
    }
    uint8_t repeat = clamp_point_repeat(state->point_repeat);
    uint32_t gap = clamp_point_gap(state->point_gap);

    /* A single sweep slides the fully-formed point from the top of the rail
       down until its leading pixel reaches the tail (LED 26 / 27). The rail is
       anchored at the tail and spans `total` pixels back toward the apex, so a
       larger total length means a longer travel. `repeat` fires the sweep 1/2/3
       times back to back, then `gap` frames pause before the burst repeats. */
    uint32_t sweep_frames = (uint32_t) (total - pt + 1U); /* always >= 1 */
    uint32_t burst_frames = sweep_frames * repeat;
    uint32_t period = burst_frames + gap;
    uint32_t phase = (period > 0U) ? (frame % period) : 0U;

    if (phase >= burst_frames) {
        return; /* paused gap between bursts -> background only */
    }

    uint32_t step = phase % sweep_frames;          /* 0 .. sweep_frames-1 */
    uint8_t rail_offset = (uint8_t) (arm_len - total); /* rail top in arm coordinates */

    for (uint8_t arm = 0; arm < 2U; ++arm) {
        for (uint8_t seg = 0; seg < pt; ++seg) {
            uint32_t rail_pos = step + seg;            /* 0 .. total-1 */
            uint8_t arm_pos = (uint8_t) (rail_offset + rail_pos);
            uint16_t led = point_arm_led(arm, arm_pos);
            uint8_t palette_index = (pt > 1U)
                                        ? (uint8_t) (((uint16_t) seg * 255U) / (pt - 1U))
                                        : 0U;
            set_state_palette_level(led, &point, palette_index, 255U);
        }
    }
}

static void render_breathe_edge(const light_state_t *state, uint32_t frame)
{
    light_state_t edge;
    load_swoosh_palette_context(&edge, state, state->edge_palette);

    /* Only the edge segment is lit; the rest of the ring stays off. */
    clear_pixels();

    const uint16_t led_count = CONFIG_LIGHT_RING_LED_COUNT;
    uint8_t edge_len = clamp_edge_length(state->edge_length);
    if (edge_len > led_count) {
        edge_len = (uint8_t) led_count;
    }
    uint8_t repeat = clamp_edge_repeat(state->edge_repeat);
    uint32_t gap = clamp_edge_gap(state->edge_gap);

    /* One breath fades the edge in, briefly holds, then fades out. `repeat`
       chains 1/2/3 breaths back to back, then `gap` frames pause (edge off)
       before the burst repeats. */
    const uint32_t inhale_frames = 24U;
    const uint32_t hold_high_frames = 6U;
    const uint32_t exhale_frames = 24U;
    const uint32_t breath_frames = inhale_frames + hold_high_frames + exhale_frames;
    uint32_t burst_frames = breath_frames * repeat;
    uint32_t period = burst_frames + gap;
    uint32_t phase = (period > 0U) ? (frame % period) : 0U;

    if (phase >= burst_frames) {
        return; /* paused gap between bursts -> edge off */
    }

    uint32_t bp = phase % breath_frames;
    uint8_t breath_level;
    if (bp < inhale_frames) {
        breath_level = ease8_in_out((uint8_t) ((bp * 255U) / (inhale_frames - 1U)));
    } else if (bp < (inhale_frames + hold_high_frames)) {
        breath_level = 255U;
    } else {
        uint32_t ep = bp - inhale_frames - hold_high_frames;
        breath_level = (uint8_t) (255U - ease8_in_out((uint8_t) ((ep * 255U) / (exhale_frames - 1U))));
    }

    if (breath_level == 0U) {
        return;
    }

    /* The edge segment is anchored at the strip end (LEDs 26/27 on a 27-LED
       ring). Growing the edge keeps that base and splits the surplus around
       the strip end: it extends back toward the start while wrapping the
       remainder across the 27->1 seam (edge_len=4 lights LEDs 25,26,27,1). */
    uint16_t wrap_count = (edge_len >= 2U) ? (uint16_t) ((edge_len - 2U) / 2U) : 0U;
    uint16_t low_count = (uint16_t) (edge_len - wrap_count);
    uint16_t start = (uint16_t) (led_count - low_count);
    for (uint8_t pos = 0; pos < edge_len; ++pos) {
        uint16_t led = (uint16_t) ((start + pos) % led_count);
        uint8_t palette_index = (edge_len > 1U)
                                    ? (uint8_t) (((uint16_t) pos * 255U) / (edge_len - 1U))
                                    : 0U;
        set_state_palette_level(led, &edge, palette_index, breath_level);
    }
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
    case EFFECT_POINT:
        render_point(state, frame);
        break;
    case EFFECT_BREATHE_EDGE:
        render_breathe_edge(state, frame);
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
    cJSON *pt_pal = cJSON_GetObjectItemCaseSensitive(segment, "ptPal");
    cJSON *pt_bg = cJSON_GetObjectItemCaseSensitive(segment, "ptBg");
    cJSON *pt_len = cJSON_GetObjectItemCaseSensitive(segment, "ptLen");
    cJSON *pt_total = cJSON_GetObjectItemCaseSensitive(segment, "ptTotal");
    cJSON *pt_mode = cJSON_GetObjectItemCaseSensitive(segment, "ptMode");
    cJSON *pt_gap = cJSON_GetObjectItemCaseSensitive(segment, "ptGap");
    cJSON *edge_pal = cJSON_GetObjectItemCaseSensitive(segment, "edgePal");
    cJSON *edge_len = cJSON_GetObjectItemCaseSensitive(segment, "edgeLen");
    cJSON *edge_mode = cJSON_GetObjectItemCaseSensitive(segment, "edgeMode");
    cJSON *edge_gap = cJSON_GetObjectItemCaseSensitive(segment, "edgeGap");
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
    if (cJSON_IsNumber(pt_pal)) {
        state->point_palette = clamp_palette(pt_pal->valueint);
    }
    if (cJSON_IsNumber(pt_bg)) {
        state->point_background_palette = clamp_palette(pt_bg->valueint);
    }
    if (cJSON_IsNumber(pt_len)) {
        state->point_length = clamp_point_length(pt_len->valueint);
    }
    if (cJSON_IsNumber(pt_total)) {
        state->point_total_length = clamp_point_total(pt_total->valueint);
    }
    if (cJSON_IsNumber(pt_mode)) {
        state->point_repeat = clamp_point_repeat(pt_mode->valueint);
    }
    if (cJSON_IsNumber(pt_gap)) {
        state->point_gap = clamp_point_gap(pt_gap->valueint);
    }
    if (cJSON_IsNumber(edge_pal)) {
        state->edge_palette = clamp_palette(edge_pal->valueint);
    }
    if (cJSON_IsNumber(edge_len)) {
        state->edge_length = clamp_edge_length(edge_len->valueint);
    }
    if (cJSON_IsNumber(edge_mode)) {
        state->edge_repeat = clamp_edge_repeat(edge_mode->valueint);
    }
    if (cJSON_IsNumber(edge_gap)) {
        state->edge_gap = clamp_edge_gap(edge_gap->valueint);
    }
    if (cJSON_IsBool(on)) {
        state->on = cJSON_IsTrue(on);
    }
    if (cJSON_IsArray(colors) && cJSON_GetArraySize(colors) > 0) {
        apply_color_array(cJSON_GetArrayItem(colors, 0), state);
    }
}

void apply_json_state(cJSON *root, light_state_t *state)
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
    cJSON *pt_pal = cJSON_GetObjectItemCaseSensitive(root, "ptPal");
    cJSON *pt_bg = cJSON_GetObjectItemCaseSensitive(root, "ptBg");
    cJSON *pt_len = cJSON_GetObjectItemCaseSensitive(root, "ptLen");
    cJSON *pt_total = cJSON_GetObjectItemCaseSensitive(root, "ptTotal");
    cJSON *pt_mode = cJSON_GetObjectItemCaseSensitive(root, "ptMode");
    cJSON *pt_gap = cJSON_GetObjectItemCaseSensitive(root, "ptGap");
    cJSON *edge_pal = cJSON_GetObjectItemCaseSensitive(root, "edgePal");
    cJSON *edge_len = cJSON_GetObjectItemCaseSensitive(root, "edgeLen");
    cJSON *edge_mode = cJSON_GetObjectItemCaseSensitive(root, "edgeMode");
    cJSON *edge_gap = cJSON_GetObjectItemCaseSensitive(root, "edgeGap");
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
    if (cJSON_IsNumber(pt_pal)) {
        state->point_palette = clamp_palette(pt_pal->valueint);
    }
    if (cJSON_IsNumber(pt_bg)) {
        state->point_background_palette = clamp_palette(pt_bg->valueint);
    }
    if (cJSON_IsNumber(pt_len)) {
        state->point_length = clamp_point_length(pt_len->valueint);
    }
    if (cJSON_IsNumber(pt_total)) {
        state->point_total_length = clamp_point_total(pt_total->valueint);
    }
    if (cJSON_IsNumber(pt_mode)) {
        state->point_repeat = clamp_point_repeat(pt_mode->valueint);
    }
    if (cJSON_IsNumber(pt_gap)) {
        state->point_gap = clamp_point_gap(pt_gap->valueint);
    }
    if (cJSON_IsNumber(edge_pal)) {
        state->edge_palette = clamp_palette(edge_pal->valueint);
    }
    if (cJSON_IsNumber(edge_len)) {
        state->edge_length = clamp_edge_length(edge_len->valueint);
    }
    if (cJSON_IsNumber(edge_mode)) {
        state->edge_repeat = clamp_edge_repeat(edge_mode->valueint);
    }
    if (cJSON_IsNumber(edge_gap)) {
        state->edge_gap = clamp_edge_gap(edge_gap->valueint);
    }
    apply_color_array(color, state);

    if (cJSON_IsArray(segments) && cJSON_GetArraySize(segments) > 0) {
        apply_segment_json(cJSON_GetArrayItem(segments, 0), state);
    }
}

cJSON *build_state_json(void)
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
    cJSON_AddNumberToObject(root, "ptPal", state.point_palette);
    cJSON_AddNumberToObject(root, "ptBg", state.point_background_palette);
    cJSON_AddNumberToObject(root, "ptLen", state.point_length);
    cJSON_AddNumberToObject(root, "ptTotal", state.point_total_length);
    cJSON_AddNumberToObject(root, "ptMode", state.point_repeat);
    cJSON_AddNumberToObject(root, "ptGap", state.point_gap);
    cJSON_AddNumberToObject(root, "edgePal", state.edge_palette);
    cJSON_AddNumberToObject(root, "edgeLen", state.edge_length);
    cJSON_AddNumberToObject(root, "edgeMode", state.edge_repeat);
    cJSON_AddNumberToObject(root, "edgeGap", state.edge_gap);
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
    cJSON_AddNumberToObject(segment, "ptPal", state.point_palette);
    cJSON_AddNumberToObject(segment, "ptBg", state.point_background_palette);
    cJSON_AddNumberToObject(segment, "ptLen", state.point_length);
    cJSON_AddNumberToObject(segment, "ptTotal", state.point_total_length);
    cJSON_AddNumberToObject(segment, "ptMode", state.point_repeat);
    cJSON_AddNumberToObject(segment, "ptGap", state.point_gap);
    cJSON_AddNumberToObject(segment, "edgePal", state.edge_palette);
    cJSON_AddNumberToObject(segment, "edgeLen", state.edge_length);
    cJSON_AddNumberToObject(segment, "edgeMode", state.edge_repeat);
    cJSON_AddNumberToObject(segment, "edgeGap", state.edge_gap);

    cJSON_AddItemToArray(primary_color, cJSON_CreateNumber(state.red));
    cJSON_AddItemToArray(primary_color, cJSON_CreateNumber(state.green));
    cJSON_AddItemToArray(primary_color, cJSON_CreateNumber(state.blue));
    cJSON_AddItemToArray(segment_colors, primary_color);
    cJSON_AddItemToArray(segment_array, segment);

    return root;
}

esp_err_t light_effects_start(void)
{
    BaseType_t task_created = xTaskCreate(light_render_task, "light_render", 4096, NULL, 5, NULL);
    return (task_created == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
