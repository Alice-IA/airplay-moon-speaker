#include "led_strip_ctrl.h"

#include "esp_log.h"

#if CONFIG_LED_ANIM_GPIO >= 0
#include "led_strip.h"

static const char *TAG = "led_strip_ctrl";
static led_strip_handle_t s_strip = NULL;

void led_strip_ctrl_init(void) {
  led_strip_config_t strip_cfg = {
      .strip_gpio_num = CONFIG_LED_ANIM_GPIO,
      .max_leds = CONFIG_LED_ANIM_COUNT,
      .led_model = LED_MODEL_WS2812,
      .flags.invert_out = false,
  };
  led_strip_rmt_config_t rmt_cfg = {
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = 10 * 1000 * 1000,
      .flags.with_dma = false,
  };

  if (led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip) != ESP_OK) {
    ESP_LOGE(TAG, "Animation LED strip init failed");
    s_strip = NULL;
    return;
  }

  led_strip_clear(s_strip);
  ESP_LOGI(TAG, "Animation LED strip initialized on GPIO %d with %d LEDs",
           CONFIG_LED_ANIM_GPIO, CONFIG_LED_ANIM_COUNT);
}

void led_strip_ctrl_set_pixels(const uint8_t *rgba, size_t count) {
  if (!s_strip) {
    return;
  }
  if (count > CONFIG_LED_ANIM_COUNT) {
    count = CONFIG_LED_ANIM_COUNT;
  }
  for (size_t i = 0; i < count; i++) {
    uint8_t r = rgba[i * 4 + 0];
    uint8_t g = rgba[i * 4 + 1];
    uint8_t b = rgba[i * 4 + 2];
    // Alpha could scale RGB here; for now we pass RGB straight through.
    led_strip_set_pixel(s_strip, i, r, g, b);
  }
  led_strip_refresh(s_strip);
}

void led_strip_ctrl_clear(void) {
  if (!s_strip) {
    return;
  }
  led_strip_clear(s_strip);
  led_strip_refresh(s_strip);
}

void led_strip_ctrl_refresh(void) {
  if (!s_strip) {
    return;
  }
  led_strip_refresh(s_strip);
}

#else

void led_strip_ctrl_init(void) {
}
void led_strip_ctrl_set_pixels(const uint8_t *rgba, size_t count) {
  (void)rgba;
  (void)count;
}
void led_strip_ctrl_clear(void) {
}
void led_strip_ctrl_refresh(void) {
}

#endif
