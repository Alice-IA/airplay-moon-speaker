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
