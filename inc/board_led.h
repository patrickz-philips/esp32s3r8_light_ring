#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t board_led_init(void);
esp_err_t transmit_pixels(void);
void set_pixel_rgb(uint16_t index, uint8_t red, uint8_t green, uint8_t blue);
void clear_pixels(void);
void fill_pixels_rgb(uint8_t red, uint8_t green, uint8_t blue);
