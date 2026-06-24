#pragma once

#include <stdint.h>

#include "app_config.h"

/* Palette compression.
 *
 * Several effects spread a full palette (index 0..255) across a configurable
 * run of pixels. This module remaps a pixel position within such a run to a
 * palette index so that a short run reproduces the original gradient as a
 * uniformly down-scaled view: every pixel samples the palette at an equal
 * fraction of its 0..255 range. The two extreme pixels still reproduce the
 * exact end colors (index 0 lights LED 1, index 255 lights LED 27), but no
 * end is given sampling priority -- the whole palette is scaled evenly.
 */

/* Returns the palette index (0..255) for pixel `pos` within a run of `length`
 * pixels, scaling the full palette evenly across the run. */
uint8_t palette_compress_index(uint8_t pos, uint8_t length);

/* Fills `out_indices` (length entries) with the compressed palette indices for
 * a run of `length` pixels. `out_indices` must hold at least `length` bytes. */
void palette_compress_indices(uint8_t length, uint8_t *out_indices);
