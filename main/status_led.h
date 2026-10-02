#pragma once

#include "esp_err.h"

/**
 * Status LED indicator — drives the moon's LED strip as a system status light.
 *
 * Colors:
 *   Orange (0xFF8000) — booting / no WiFi / OTA in progress
 *   Blue   (0x0000FF) — WiFi connected (transient; turns off after a delay)
 *   Green  (0x00FF00) — OTA completed successfully
 *   Off              — normal operation
 *
 * The status LED is a temporary override: once normal operation resumes
 * (status_led_off), the LED subsystem returns to whatever the user/app set.
 */

/** Boot state: orange, shown until WiFi connects or setup completes. */
void status_led_boot(void);

/** WiFi connected: blue. Call status_led_off() after a short delay. */
void status_led_wifi_connected(void);

/** OTA starting: orange. */
void status_led_ota_start(void);

/** OTA success: green (shown briefly before reboot). */
void status_led_ota_done(void);

/** OTA failed: red flash then off. */
void status_led_ota_fail(void);

/** Turn off the status indicator (return LED control to normal). */
void status_led_off(void);
