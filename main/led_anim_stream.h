#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * Initialize the LED animation stream subsystem.
 * Must be called before web_server_start().
 */
void led_anim_stream_init(void);

/**
 * Register the /ws/leds WebSocket endpoint on the running HTTP server.
 */
esp_err_t led_anim_stream_register(httpd_handle_t server);

/**
 * Start a built-in effect.
 *
 * @param name effect name ("off", "static", "rainbow", "breathe",
 *             "chase", "sparkle", "fire")
 * @param speed animation speed, 0-255 (higher = faster)
 * @param intensity effect intensity, 0-255
 * @param brightness global brightness, 0-255
 * @param color1 first color as 0xRRGGBB
 * @param color2 second color as 0xRRGGBB
 */
esp_err_t led_anim_stream_start_effect(const char *name,
                                       uint8_t speed,
                                       uint8_t intensity,
                                       uint8_t brightness,
                                       uint32_t color1,
                                       uint32_t color2);

/**
 * Stop any active effect or stream and clear the strip.
 */
void led_anim_stream_stop(void);
