#include <stddef.h>
#include <stdint.h>

#include "palette_compress.h"

uint8_t palette_compress_index(uint8_t pos, uint8_t length)
{
    if (length <= 1U || pos == 0U) {
        return 0U;
    }
    if (pos >= (uint8_t) (length - 1U)) {
        return 255U;
    }

    /* Evenly scale the whole palette across the run: every pixel samples the
       palette at an equal fraction of its 0..255 range, so a short run is a
       uniformly down-scaled view of the original gradient rather than a view
       biased toward either end. */
    uint32_t index = (((uint32_t) pos * 255U) + ((length - 1U) / 2U)) / (uint32_t) (length - 1U);
    return (index > 255U) ? 255U : (uint8_t) index;
}

void palette_compress_indices(uint8_t length, uint8_t *out_indices)
{
    if (out_indices == NULL) {
        return;
    }

    for (uint8_t pos = 0U; pos < length; ++pos) {
        out_indices[pos] = palette_compress_index(pos, length);
    }
}
