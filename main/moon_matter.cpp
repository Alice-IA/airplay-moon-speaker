/**
 * Moon Matter integration — bridges Matter clusters to the LED animation system.
 *
 * Data model:
 *   Endpoint 1: Extended Color Light
 *     - OnOff cluster       → LEDs on/off (off = effect "off", on = restore previous)
 *     - LevelControl        → WS2812 brightness (0-254 mapped to 0-255)
 *     - ColorControl        → CurrentHue (0-254) + CurrentSaturation (0-254)
 *     - Custom cluster 0x131BFC01 (Moon Animation):
 *       attr 0x0000: EffectEnum   (uint8)  0=off,1=static,2=rainbow,3=breathe,
 *                                  4=chase,5=sparkle,6=fire
 *       attr 0x0001: Speed        (uint8)  0-255
 *       attr 0x0002: Intensity    (uint8)  0-255
 *
 * mDNS: CONFIG_USE_MINIMAL_MDNS=n must be set so Matter uses the ESP-IDF mdns
 * component (shared with AirPlay on UDP port 5353).
 */

#include "moon_matter.h"

#include "esp_log.h"
#include "esp_matter.h"
#include "esp_matter_core.h"
#include "esp_matter_endpoint.h"
#include "esp_matter_feature.h"

#include "led_anim_stream.h"

#include <cmath>
#include <cstring>

#include <app/server/Server.h>

static const char *TAG = "moon_matter";

using namespace chip;
using namespace chip::app::Clusters;
using namespace esp_matter;
using namespace esp_matter::endpoint;

/* --- Defaults --- */
#define DEFAULT_POWER       1
#define DEFAULT_BRIGHTNESS  128
#define DEFAULT_HUE         0    /* red */
#define DEFAULT_SATURATION  254  /* full */

/* --- Custom cluster for animation control --- */
#define MOON_CLUSTER_ID_ANIMATION  0x131BFC01
#define MOON_ATTR_EFFECT_ENUM      0x0000
#define MOON_ATTR_SPEED            0x0001
#define MOON_ATTR_INTENSITY        0x0002

/* Effect enum values (match order in led_anim_stream effect table) */
enum {
  MOON_EFFECT_OFF     = 0,
  MOON_EFFECT_STATIC  = 1,
  MOON_EFFECT_RAINBOW = 2,
  MOON_EFFECT_BREATHE = 3,
  MOON_EFFECT_CHASE   = 4,
  MOON_EFFECT_SPARKLE = 5,
  MOON_EFFECT_FIRE    = 6,
};

static const char *effect_names[] = {
    "off", "static", "rainbow", "breathe", "chase", "sparkle", "fire",
};

static uint16_t s_light_endpoint_id = 0;

/* Track state so OnOff can restore previous effect when toggled back on */
static uint8_t s_prev_effect = MOON_EFFECT_STATIC;
static uint8_t s_current_effect = MOON_EFFECT_STATIC;
static uint8_t s_speed = 128;
static uint8_t s_intensity = 128;
static uint8_t s_hue = DEFAULT_HUE;
static uint8_t s_saturation = DEFAULT_SATURATION;
static bool s_onoff = true;
static bool s_matter_started = false;

/* --- Helpers --- */

/* Matter hue is 0-254, saturation is 0-254.
 * Convert to RGB 0xRRGGBB for led_anim_stream. */
static uint32_t hsv_to_rgb_uint8(uint8_t h, uint8_t s, uint8_t v) {
  /* h: 0-254 → 0-360°, s: 0-254 → 0-1.0, v: 0-254 → 0-1.0 */
  float hf = (float)h * 360.0f / 254.0f;
  float sf = (float)s / 254.0f;
  float vf = (float)v / 255.0f;

  float r, g, b;
  float c = vf * sf;
  float x = c * (1.0f - fabsf(fmodf(hf / 60.0f, 2.0f) - 1.0f));
  float m = vf - c;

  int hi = (int)(hf / 60.0f) % 6;
  switch (hi) {
  case 0: r = c; g = x; b = 0; break;
  case 1: r = x; g = c; b = 0; break;
  case 2: r = 0; g = c; b = x; break;
  case 3: r = 0; g = x; b = c; break;
  case 4: r = x; g = 0; b = c; break;
  default: r = c; g = 0; b = x; break;
  }

  uint8_t ri = (uint8_t)((r + m) * 255.0f);
  uint8_t gi = (uint8_t)((g + m) * 255.0f);
  uint8_t bi = (uint8_t)((b + m) * 255.0f);
  return ((uint32_t)ri << 16) | ((uint32_t)gi << 8) | (uint32_t)bi;
}

static void apply_current_state(void) {
  if (!s_onoff) {
    led_anim_stream_stop();
    return;
  }

  if (s_current_effect == MOON_EFFECT_OFF) {
    led_anim_stream_stop();
    return;
  }

  /* For static effect, use hue+saturation as color1.
   * For other effects, pass hue+sat as color1 and a complementary as color2. */
  uint32_t color1 = hsv_to_rgb_uint8(s_hue, s_saturation, 255);
  /* color2: shifted hue for variety in multi-color effects */
  uint8_t hue2 = (uint8_t)((s_hue + 127) % 255);
  uint32_t color2 = hsv_to_rgb_uint8(hue2, s_saturation, 255);

  uint8_t brightness = led_anim_stream_get_brightness();

  led_anim_stream_start_effect(effect_names[s_current_effect],
                                s_speed, s_intensity, brightness,
                                color1, color2);
}

/* --- Matter event callback --- */
static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg) {
  (void)arg;
  switch (event->Type) {
  case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
    ESP_LOGI(TAG, "Interface IP Address changed");
    break;
  case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
    ESP_LOGI(TAG, "Commissioning complete");
    break;
  case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
    ESP_LOGI(TAG, "Commissioning window opened");
    break;
  case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
    ESP_LOGI(TAG, "Commissioning window closed");
    break;
  case chip::DeviceLayer::DeviceEventType::kBLEDeinitialized:
    ESP_LOGI(TAG, "BLE deinitialized");
    break;
  default:
    break;
  }
}

/* --- Attribute update callback --- */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type,
                                          uint16_t endpoint_id,
                                          uint32_t cluster_id,
                                          uint32_t attribute_id,
                                          esp_matter_attr_val_t *val,
                                          void *priv_data) {
  (void)priv_data;

  if (type != attribute::PRE_UPDATE) {
    return ESP_OK;
  }

  if (endpoint_id != s_light_endpoint_id) {
    return ESP_OK;
  }

  /* OnOff cluster (0x0006) */
  if (cluster_id == OnOff::Id) {
    if (attribute_id == OnOff::Attributes::OnOff::Id) {
      s_onoff = val->val.b;
      if (s_onoff && s_current_effect == MOON_EFFECT_OFF) {
        /* Restore previous effect when turned on */
        s_current_effect = s_prev_effect;
      } else if (!s_onoff) {
        s_prev_effect = s_current_effect;
      }
      apply_current_state();
      ESP_LOGI(TAG, "OnOff → %s", s_onoff ? "ON" : "OFF");
    }
  }
  /* LevelControl cluster (0x0008) */
  else if (cluster_id == LevelControl::Id) {
    if (attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
      /* Matter level is 0-254, our brightness is 0-255 */
      uint8_t brightness = (uint8_t)(val->val.u8 + 1);
      led_anim_stream_set_brightness(brightness);
      ESP_LOGI(TAG, "Level → %d (brightness %d)", val->val.u8, brightness);
    }
  }
  /* ColorControl cluster (0x0300) */
  else if (cluster_id == ColorControl::Id) {
    if (attribute_id == ColorControl::Attributes::CurrentHue::Id) {
      s_hue = (uint8_t)val->val.u8;
      apply_current_state();
      ESP_LOGI(TAG, "Hue → %d", s_hue);
    } else if (attribute_id == ColorControl::Attributes::CurrentSaturation::Id) {
      s_saturation = (uint8_t)val->val.u8;
      apply_current_state();
      ESP_LOGI(TAG, "Saturation → %d", s_saturation);
    }
  }
  /* Custom animation cluster */
  else if (cluster_id == MOON_CLUSTER_ID_ANIMATION) {
    if (attribute_id == MOON_ATTR_EFFECT_ENUM) {
      s_current_effect = (uint8_t)val->val.u8;
      if (s_current_effect != MOON_EFFECT_OFF) {
        s_prev_effect = s_current_effect;
      }
      apply_current_state();
      ESP_LOGI(TAG, "Animation effect → %d (%s)", s_current_effect,
               effect_names[s_current_effect]);
    } else if (attribute_id == MOON_ATTR_SPEED) {
      s_speed = (uint8_t)val->val.u8;
      apply_current_state();
      ESP_LOGI(TAG, "Animation speed → %d", s_speed);
    } else if (attribute_id == MOON_ATTR_INTENSITY) {
      s_intensity = (uint8_t)val->val.u8;
      apply_current_state();
      ESP_LOGI(TAG, "Animation intensity → %d", s_intensity);
    }
  }

  return ESP_OK;
}

/* --- Identification callback --- */
static esp_err_t app_identification_cb(identification::callback_type_t type,
                                       uint16_t endpoint_id,
                                       uint8_t effect_id,
                                       uint8_t effect_variant,
                                       void *priv_data) {
  (void)priv_data;
  ESP_LOGI(TAG, "Identify callback: type=%u endpoint=%u effect=%u variant=%u",
           type, endpoint_id, effect_id, effect_variant);
  return ESP_OK;
}

/* --- Initialization --- */
esp_err_t moon_matter_init(void) {
  ESP_LOGI(TAG, "Initializing Moon Matter integration...");

  node::config_t node_config;
  node_t *node = node::create(&node_config,
                              app_attribute_update_cb,
                              app_identification_cb,
                              NULL);
  if (!node) {
    ESP_LOGE(TAG, "Failed to create Matter node");
    return ESP_FAIL;
  }

  /* Create extended_color_light endpoint.
   * This includes OnOff, LevelControl, and ColorControl clusters.
   * Uses hue/saturation color mode (not color temperature). */
  extended_color_light::config_t light_config;
  light_config.on_off.on_off = DEFAULT_POWER;
  light_config.on_off_lighting.start_up_on_off = nullptr;
  light_config.level_control.current_level = DEFAULT_BRIGHTNESS;
  light_config.level_control.on_level = DEFAULT_BRIGHTNESS;
  light_config.level_control_lighting.start_up_current_level = nullptr;
  /* Use hue/saturation mode (1) instead of color temperature mode (2) */
  light_config.color_control.color_mode =
      (uint8_t)ColorControl::ColorMode::kCurrentHueAndCurrentSaturation;
  light_config.color_control.enhanced_color_mode =
      (uint8_t)ColorControl::ColorMode::kCurrentHueAndCurrentSaturation;
  /* Must set startup_color_temperature_mireds to nullptr, otherwise the
   * default value (0x00fa) forces ColorMode back to kColorTemperature
   * on boot. See espressif/esp-matter#1479. */
  light_config.color_control_color_temperature.start_up_color_temperature_mireds = nullptr;

  endpoint_t *light_endpoint = extended_color_light::create(
      node, &light_config, ENDPOINT_FLAG_NONE, NULL);
  if (!light_endpoint) {
    ESP_LOGE(TAG, "Failed to create extended_color_light endpoint");
    return ESP_FAIL;
  }

  s_light_endpoint_id = endpoint::get_id(light_endpoint);
  ESP_LOGI(TAG, "Light endpoint id: %d", s_light_endpoint_id);

  /* Enable the Hue/Saturation feature on the ColorControl cluster.
   * In extended_color_light, HS is OPTIONAL and NOT enabled by default —
   * only ColorXY and ColorTemperature are. Without this, Apple Home only
   * shows white-temperature presets (no RGB color wheel).
   * See espressif/esp-matter#513. */
  cluster_t *color_cluster = cluster::get(light_endpoint, ColorControl::Id);
  if (color_cluster) {
    cluster::color_control::feature::hue_saturation::config_t hs_config;
    hs_config.current_hue = DEFAULT_HUE;
    hs_config.current_saturation = DEFAULT_SATURATION;
    esp_err_t hs_err = cluster::color_control::feature::hue_saturation::add(
        color_cluster, &hs_config);
    if (hs_err != ESP_OK) {
      ESP_LOGW(TAG, "Failed to add hue_saturation feature: %s",
               esp_err_to_name(hs_err));
    } else {
      ESP_LOGI(TAG, "Hue/Saturation feature enabled (RGB color wheel)");
    }
  } else {
    ESP_LOGW(TAG, "ColorControl cluster not found — RGB unavailable");
  }

  /* Add custom animation cluster to the same endpoint */
  cluster_t *anim_cluster = cluster::create(light_endpoint,
                                             MOON_CLUSTER_ID_ANIMATION,
                                             CLUSTER_FLAG_SERVER);
  if (!anim_cluster) {
    ESP_LOGE(TAG, "Failed to create animation cluster");
    return ESP_FAIL;
  }

  /* Effect enum attribute */
  attribute::create(anim_cluster, MOON_ATTR_EFFECT_ENUM,
                    ATTRIBUTE_FLAG_NONE,
                    esp_matter_uint8(s_current_effect));

  /* Speed attribute */
  attribute::create(anim_cluster, MOON_ATTR_SPEED,
                    ATTRIBUTE_FLAG_NONE,
                    esp_matter_uint8(s_speed));

  /* Intensity attribute */
  attribute::create(anim_cluster, MOON_ATTR_INTENSITY,
                    ATTRIBUTE_FLAG_NONE,
                    esp_matter_uint8(s_intensity));

  /* Start Matter */
  esp_err_t err = esp_matter::start(app_event_cb);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start Matter: %s", esp_err_to_name(err));
    return err;
  }

  s_matter_started = true;

  ESP_LOGI(TAG, "Matter started. Light endpoint=%d with OnOff+Level+Color+Animation",
           s_light_endpoint_id);

  /* Apply initial state to LEDs */
  apply_current_state();

  return ESP_OK;
}

/* Reopen commissioning window for multi-admin pairing (e.g. add to Home
 * Assistant after Apple Home). Uses the factory passcode, so the same PIN
 * (20202021) works for the second fabric. */
extern "C" esp_err_t moon_matter_open_commissioning_window(void) {
  if (!s_matter_started) {
    ESP_LOGW(TAG, "Commissioning window request ignored — Matter not running");
    return ESP_ERR_INVALID_STATE;
  }

  chip::DeviceLayer::CommissioningWindowManager &cwm =
      chip::Server::GetInstance().GetCommissioningWindowManager();

  if (cwm.IsCommissioningWindowOpen()) {
    ESP_LOGI(TAG, "Commissioning window already open");
    return ESP_OK;
  }

  CHIP_ERROR chip_err =
      cwm.OpenBasicCommissioningWindow(chip::System::Clock::Seconds32(300));
  if (chip_err != CHIP_NO_ERROR) {
    ESP_LOGE(TAG, "Failed to open commissioning window: %" CHIP_ERROR_FORMAT,
             chip_err.Format());
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Commissioning window open for 300s — pair with PIN 20202021");
  return ESP_OK;
}
