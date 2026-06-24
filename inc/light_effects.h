#pragma once

#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "light_types.h"

/* Guards the shared light state, custom palettes and scenario tables. */
extern SemaphoreHandle_t s_state_lock;

esp_err_t light_effects_start(void);
void snapshot_light_state(light_state_t *state);
void store_light_state(const light_state_t *state);
cJSON *build_state_json(void);
void apply_json_state(cJSON *root, light_state_t *state);
uint8_t clamp_u8(int value);
effect_id_t clamp_effect(int value);
