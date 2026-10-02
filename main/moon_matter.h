#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the Moon Matter integration.
 *
 * Creates a Matter extended_color_light endpoint that maps:
 *   - OnOff cluster      → LEDs on/off
 *   - LevelControl       → WS2812 brightness (0-255)
 *   - ColorControl       → Hue + Saturation (current color)
 *   - Custom cluster      → Animation effect selection + speed + intensity
 *
 * Must be called after WiFi is connected and LED subsystem is initialized.
 * Uses CONFIG_USE_MINIMAL_MDNS=n so Matter shares the ESP-IDF mDNS socket
 * with AirPlay (both use UDP port 5353).
 */
esp_err_t moon_matter_init(void);

/**
 * Reopen the Matter commissioning window so a second fabric (e.g. Home
 * Assistant) can commission the device after it was first commissioned
 * (e.g. to Apple Home). This is multi-admin pairing: the existing fabric
 * is NOT removed.
 *
 * Opens a "basic" window using the factory passcode (20202021) and
 * discriminator (3840), advertised over DNS-SD (BLE is released after
 * first commissioning). The window stays open for 5 minutes or until a
 * commissioning session completes.
 *
 * Returns ESP_ERR_INVALID_STATE if Matter is not running (MQTT mode or
 * init failed).
 */
esp_err_t moon_matter_open_commissioning_window(void);

#ifdef __cplusplus
}
#endif
