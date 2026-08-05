#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

/**
 * Initialize the animation LED strip (WS2812).
 * Uses LED_ANIM_GPIO and LED_ANIM_COUNT from Kconfig.
 */
void led_strip_ctrl_init(void);

/**
 * Set all pixels from an RGBA buffer.
 * @param rgba pixel data, 4 bytes per LED (R, G, B, A). Alpha is ignored for now.
 * @param count number of LEDs in the buffer.
 */
void led_strip_ctrl_set_pixels(const uint8_t *rgba, size_t count);

/**
 * Clear the strip (all LEDs off).
 */
void led_strip_ctrl_clear(void);

/**
 * Refresh the physical strip. Called automatically by set_pixels/clear.
 */
void led_strip_ctrl_refresh(void);
