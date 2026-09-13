/*
 * board - ESP32-C6 binding.
 *
 * A thin wrapper over the Waveshare C6 BSP, which already does everything
 * board_detect() promises (I2C init, reset pulses, touch probe). Kept as a
 * pass-through so this board's behaviour is unchanged in effect.
 */
#include "board/board.h"

#include "bsp/esp-bsp.h"

static board_variant_t
from_bsp_variant(bsp_board_variant_t variant) {
    switch (variant) {
        case BSP_BOARD_VARIANT_SH8601_FT5X06: return BOARD_VARIANT_SH8601_FT;
        case BSP_BOARD_VARIANT_CO5300_CST820: return BOARD_VARIANT_CO5300_CST;
        default: return BOARD_VARIANT_UNKNOWN;
    }
}

board_variant_t
board_detect(void) {
    return from_bsp_variant(bsp_board_detect());
}

board_variant_t
board_variant(void) {
    return from_bsp_variant(bsp_board_get_variant());
}

const char*
board_variant_name(board_variant_t variant) {
    bsp_board_variant_t bsp_variant = BSP_BOARD_VARIANT_UNKNOWN;
    if (variant == BOARD_VARIANT_SH8601_FT) {
        bsp_variant = BSP_BOARD_VARIANT_SH8601_FT5X06;
    } else if (variant == BOARD_VARIANT_CO5300_CST) {
        bsp_variant = BSP_BOARD_VARIANT_CO5300_CST820;
    }
    return bsp_board_variant_to_name(bsp_variant);
}

esp_err_t
board_audio_amp_enable(bool on) {
    return bsp_audio_poweramp_enable(on);
}
