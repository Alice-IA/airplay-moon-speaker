#include "moon_mqtt.h"

#include "esp_log.h"
#include "mqtt_client.h"  // esp-mqtt component (ESP-IDF)
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "led_anim_stream.h"
#include "wifi.h"
#include "ethernet.h"
#include "settings.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "moon_mqtt";

// HiveMQ public broker. No authentication required for testing.
#define MQTT_BROKER_URI "mqtt://broker.hivemq.com:1883"
#define MQTT_STATUS_INTERVAL_MS 30000
#define MQTT_DEVICE_TOPIC_PREFIX "moon"
#define MQTT_CMD_TOPIC_SUFFIX  "cmd"
#define MQTT_STATUS_TOPIC_SUFFIX "status"

static esp_mqtt_client_handle_t s_client = NULL;
static char s_device_id[13] = {0};  // MAC without colons, 12 chars + null
static char s_client_id[32] = {0};
static char s_cmd_topic[64] = {0};
static char s_status_topic[64] = {0};
static bool s_connected = false;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static void mac_to_device_id(const char *mac_str, char *out, size_t out_len) {
  if (!mac_str || !out || out_len < 13) {
    return;
  }
  size_t j = 0;
  for (size_t i = 0; mac_str[i] != '\0' && j < 12; i++) {
    if ((mac_str[i] >= '0' && mac_str[i] <= '9') ||
        (mac_str[i] >= 'a' && mac_str[i] <= 'f') ||
        (mac_str[i] >= 'A' && mac_str[i] <= 'F')) {
      out[j++] = mac_str[i];
    }
  }
  out[j] = '\0';
}

static uint32_t hex_to_rgb(const char *hex) {
  if (!hex) {
    return 0;
  }
  while (*hex == '#') {
    hex++;
  }
  if (strlen(hex) < 6) {
    return 0;
  }
  char buf[7] = {0};
  strncpy(buf, hex, 6);
  return (uint32_t)strtol(buf, NULL, 16);
}

static void get_ip_str(char *buf, size_t len) {
  if (ethernet_is_connected()) {
    ethernet_get_ip_str(buf, len);
  } else if (wifi_is_connected()) {
    wifi_get_ip_str(buf, len);
  } else {
    strncpy(buf, "0.0.0.0", len - 1);
    buf[len - 1] = '\0';
  }
}

/* ------------------------------------------------------------------ */
/* Command handlers                                                   */
/* ------------------------------------------------------------------ */

static void handle_cmd_frame(const char *data, size_t len) {
  // Payload is the raw binary frame without the 2-byte WebSocket header.
  // Format: frame_id(4 BE), timestamp_ms(4 BE), pixels(...)
  if (len < 8) {
    ESP_LOGW(TAG, "frame payload too short");
    return;
  }
  led_anim_stream_handle_frame((const uint8_t *)data, len);
}

static void handle_cmd_stream(const char *data, size_t len) {
  if (len < 4) {
    ESP_LOGW(TAG, "stream start payload too short");
    return;
  }
  led_anim_stream_handle_stream_start((const uint8_t *)data, len);
}

static void handle_cmd_effect(const char *data, size_t len) {
  char *buf = calloc(1, len + 1);
  if (!buf) {
    return;
  }
  memcpy(buf, data, len);

  cJSON *json = cJSON_Parse(buf);
  free(buf);
  if (!json) {
    ESP_LOGW(TAG, "invalid effect JSON");
    return;
  }

  cJSON *effect_json = cJSON_GetObjectItem(json, "effect");
  const char *effect = effect_json && cJSON_IsString(effect_json)
                           ? effect_json->valuestring
                           : "off";

  cJSON *speed_json = cJSON_GetObjectItem(json, "speed");
  uint8_t speed = speed_json && cJSON_IsNumber(speed_json)
                      ? (uint8_t)speed_json->valueint
                      : 128;

  cJSON *intensity_json = cJSON_GetObjectItem(json, "intensity");
  uint8_t intensity = intensity_json && cJSON_IsNumber(intensity_json)
                          ? (uint8_t)intensity_json->valueint
                          : 128;

  cJSON *brightness_json = cJSON_GetObjectItem(json, "brightness");
  uint8_t brightness = brightness_json && cJSON_IsNumber(brightness_json)
                           ? (uint8_t)brightness_json->valueint
                           : 255;

  cJSON *color1_json = cJSON_GetObjectItem(json, "color1");
  const char *color1_str = color1_json && cJSON_IsString(color1_json)
                                 ? color1_json->valuestring
                                 : "ff0000";

  cJSON *color2_json = cJSON_GetObjectItem(json, "color2");
  const char *color2_str = color2_json && cJSON_IsString(color2_json)
                                 ? color2_json->valuestring
                                 : "0000ff";

  uint32_t color1 = hex_to_rgb(color1_str);
  uint32_t color2 = hex_to_rgb(color2_str);

  led_anim_stream_start_effect(effect, speed, intensity, brightness, color1, color2);
  cJSON_Delete(json);
}

static void handle_cmd_brightness(const char *data, size_t len) {
  char *buf = calloc(1, len + 1);
  if (!buf) {
    return;
  }
  memcpy(buf, data, len);

  cJSON *json = cJSON_Parse(buf);
  free(buf);
  if (!json) {
    ESP_LOGW(TAG, "invalid brightness JSON");
    return;
  }

  cJSON *val = cJSON_GetObjectItem(json, "brightness");
  if (val && cJSON_IsNumber(val)) {
    int b = (int)val->valuedouble;
    if (b < 0) {
      b = 0;
    }
    if (b > 255) {
      b = 255;
    }
    led_anim_stream_set_brightness((uint8_t)b);
  }
  cJSON_Delete(json);
}

static void dispatch_cmd(const char *topic, const char *data, size_t len) {
  // topic is expected to be: moon/{device_id}/cmd/<subtopic>
  const char *cmd_part = strrchr(topic, '/');
  if (!cmd_part || cmd_part == topic) {
    return;
  }
  cmd_part++;

  if (strcmp(cmd_part, "frame") == 0) {
    handle_cmd_frame(data, len);
  } else if (strcmp(cmd_part, "stream") == 0) {
    handle_cmd_stream(data, len);
  } else if (strcmp(cmd_part, "stop") == 0) {
    led_anim_stream_stop();
  } else if (strcmp(cmd_part, "effect") == 0) {
    handle_cmd_effect(data, len);
  } else if (strcmp(cmd_part, "brightness") == 0) {
    handle_cmd_brightness(data, len);
  } else {
    ESP_LOGW(TAG, "unknown cmd: %s", cmd_part);
  }
}

/* ------------------------------------------------------------------ */
/* MQTT event handler                                                 */
/* ------------------------------------------------------------------ */

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
  (void)handler_args;
  (void)base;
  esp_mqtt_event_handle_t event = event_data;

  switch ((esp_mqtt_event_id_t)event_id) {
  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(TAG, "MQTT connected");
    s_connected = true;
    esp_mqtt_client_subscribe(s_client, s_cmd_topic, 1);
    moon_mqtt_publish_status();
    break;

  case MQTT_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "MQTT disconnected");
    s_connected = false;
    break;

  case MQTT_EVENT_SUBSCRIBED:
    ESP_LOGI(TAG, "MQTT subscribed to %s", s_cmd_topic);
    break;

  case MQTT_EVENT_UNSUBSCRIBED:
    ESP_LOGW(TAG, "MQTT unsubscribed");
    break;

  case MQTT_EVENT_PUBLISHED:
    break;

  case MQTT_EVENT_DATA:
    dispatch_cmd(event->topic, event->data, event->data_len);
    break;

  case MQTT_EVENT_ERROR:
    ESP_LOGE(TAG, "MQTT error");
    break;

  default:
    break;
  }
}

/* ------------------------------------------------------------------ */
/* Status publisher                                                   */
/* ------------------------------------------------------------------ */

void moon_mqtt_publish_status(void) {
  if (!s_client || !s_connected) {
    return;
  }

  char ip_str[32] = {0};
  get_ip_str(ip_str, sizeof(ip_str));

  char name[65] = {0};
  settings_get_device_name(name, sizeof(name));

  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "online", true);
  cJSON_AddStringToObject(root, "device_id", s_device_id);
  cJSON_AddStringToObject(root, "ip", ip_str);
  cJSON_AddStringToObject(root, "device_name", name);
  cJSON_AddNumberToObject(root, "led_count", CONFIG_LED_ANIM_COUNT);
  cJSON_AddStringToObject(root, "color_order", "GRB");
  cJSON_AddStringToObject(root, "rgb_type", "RGB8");
  cJSON_AddNumberToObject(root, "brightness", led_anim_stream_get_brightness());
  cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());

  if (wifi_is_connected()) {
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
      cJSON_AddStringToObject(root, "wifi_ssid", (const char *)ap_info.ssid);
      cJSON_AddNumberToObject(root, "wifi_rssi", ap_info.rssi);
    }
  }

  char *json_str = cJSON_PrintUnformatted(root);
  if (json_str) {
    esp_mqtt_client_publish(s_client, s_status_topic, json_str, 0, 1, 1);
    free(json_str);
  }
  cJSON_Delete(root);
}

static void status_publisher_task(void *arg) {
  (void)arg;
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(MQTT_STATUS_INTERVAL_MS));
    moon_mqtt_publish_status();
  }
}

/* ------------------------------------------------------------------ */
/* Initialization                                                     */
/* ------------------------------------------------------------------ */

esp_err_t moon_mqtt_init(void) {
  if (s_client) {
    return ESP_OK;
  }

  char mac_str[18] = {0};
  wifi_get_mac_str(mac_str, sizeof(mac_str));
  mac_to_device_id(mac_str, s_device_id, sizeof(s_device_id));

  if (strlen(s_device_id) != 12) {
    ESP_LOGE(TAG, "Failed to derive device ID from MAC");
    return ESP_FAIL;
  }

  snprintf(s_client_id, sizeof(s_client_id), "moon_%s", s_device_id);
  snprintf(s_cmd_topic, sizeof(s_cmd_topic), "%s/%s/%s/#",
           MQTT_DEVICE_TOPIC_PREFIX, s_device_id, MQTT_CMD_TOPIC_SUFFIX);
  snprintf(s_status_topic, sizeof(s_status_topic), "%s/%s/%s",
           MQTT_DEVICE_TOPIC_PREFIX, s_device_id, MQTT_STATUS_TOPIC_SUFFIX);

  ESP_LOGI(TAG, "MQTT device_id=%s client_id=%s", s_device_id, s_client_id);

  esp_mqtt_client_config_t mqtt_cfg = {
      .broker = {
          .address = {
              .uri = MQTT_BROKER_URI,
          },
      },
      .credentials = {
          .client_id = s_client_id,
      },
      .session = {
          .keepalive = 60,
          .last_will = {
              .topic = s_status_topic,
              .msg = "{\"online\":false}",
              .msg_len = 0,
              .qos = 1,
              .retain = 1,
          },
      },
  };

  s_client = esp_mqtt_client_init(&mqtt_cfg);
  if (!s_client) {
    ESP_LOGE(TAG, "Failed to init MQTT client");
    return ESP_FAIL;
  }

  ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                                 mqtt_event_handler, NULL));
  ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));

  xTaskCreate(status_publisher_task, "mqtt_status", 4096, NULL, 5, NULL);

  return ESP_OK;
}
