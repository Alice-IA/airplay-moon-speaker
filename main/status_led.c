/**
 * Status LED indicator — maps system states to moon LED colors.
 *
 * Uses led_anim_stream "static" effect with a single solid color.
 * Brightness kept moderate so the status is visible but not blinding.
 */

#include "status_led.h"

#include "esp_log.h"
#include "led_anim_stream.h"

static const char *TAG = "status_led";

#define COLOR_ORANGE 0xFF8000
#define COLOR_BLUE   0x0000FF
#define COLOR_GREEN  0x00FF00
#define COLOR_RED    0xFF0000

#define STATUS_BRIGHTNESS 180
#define STATUS_SPEED      128
#define STATUS_INTENSITY  255

static void set_solid(uint32_t rgb) {
  // "static" effect: color1 = rgb, color2 = same (uniform fill)
  esp_err_t err = led_anim_stream_start_effect("static", STATUS_SPEED,
                                               STATUS_INTENSITY,
                                               STATUS_BRIGHTNESS, rgb, rgb);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to set status color 0x%06lx: %s",
             (unsigned long)rgb, esp_err_to_name(err));
  }
}

void status_led_boot(void) {
  ESP_LOGI(TAG, "Status: boot (orange)");
  set_solid(COLOR_ORANGE);
}

void status_led_wifi_connected(void) {
  ESP_LOGI(TAG, "Status: WiFi connected (blue)");
  set_solid(COLOR_BLUE);
}

void status_led_ota_start(void) {
  ESP_LOGI(TAG, "Status: OTA start (orange)");
  set_solid(COLOR_ORANGE);
}

void status_led_ota_done(void) {
  ESP_LOGI(TAG, "Status: OTA done (green)");
  set_solid(COLOR_GREEN);
}

void status_led_ota_fail(void) {
  ESP_LOGI(TAG, "Status: OTA failed (red)");
  set_solid(COLOR_RED);
}

void status_led_off(void) {
  ESP_LOGI(TAG, "Status: off (normal operation)");
  led_anim_stream_stop();
}
