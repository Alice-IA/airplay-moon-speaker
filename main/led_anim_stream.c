/**
 * WebSocket binary stream for LED animation frames.
 *
 * Endpoint: /ws/leds
 * Protocol version 1
 *
 * Client -> ESP32:
 *   STREAM_START  0x01  payload: led_count (u16), fps (u16)
 *   FRAME         0x02  payload: frame_id (u32), timestamp_ms (u32),
 *                              pixels[LED_COUNT * 4] (RGBA)
 *   STREAM_STOP   0x03  no payload
 *
 * ESP32 -> Client:
 *   STREAM_READY  0x81  payload: led_count (u16), max_fps (u16)
 *   BUFFER_STATUS 0x82  payload: last_frame_id (u32), buffer_time_ms (u16)
 */

#include "led_anim_stream.h"

#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "led_strip_ctrl.h"

#include <string.h>

#define TAG "led_anim"

#define PROTO_VERSION 1

#define MSG_STREAM_START  0x01
#define MSG_FRAME         0x02
#define MSG_STREAM_STOP   0x03
#define MSG_STREAM_READY  0x81
#define MSG_BUFFER_STATUS 0x82

#define FRAME_BUF_COUNT 8
#define MAX_LEDS        CONFIG_LED_ANIM_COUNT
#define BYTES_PER_LED   4
#define MAX_FRAME_BYTES (MAX_LEDS * BYTES_PER_LED)

typedef struct {
  uint32_t frame_id;
  uint32_t timestamp_ms;
  uint8_t  pixels[MAX_FRAME_BYTES];
} frame_t;

static httpd_handle_t s_server = NULL;
static int            s_client_fd = -1;

static SemaphoreHandle_t s_buf_mutex = NULL;
static frame_t           s_frame_buf[FRAME_BUF_COUNT];
static volatile uint8_t  s_buf_head = 0;
static volatile uint8_t  s_buf_tail = 0;
static volatile uint8_t  s_buf_count = 0;

static volatile bool s_streaming = false;
static uint16_t      s_led_count = 0;
static uint16_t      s_fps = 30;
static uint32_t      s_frame_period_ms = 33;
static uint32_t      s_last_rendered_frame_id = 0;

static TaskHandle_t s_render_task = NULL;

/* ------------------------------------------------------------------ */
/* Buffer helpers                                                     */
/* ------------------------------------------------------------------ */

static inline uint8_t buf_next(uint8_t idx) {
  return (uint8_t)((idx + 1) % FRAME_BUF_COUNT);
}

static bool buf_push(const frame_t *frame) {
  if (s_buf_count >= FRAME_BUF_COUNT) {
    return false;
  }
  memcpy(&s_frame_buf[s_buf_head], frame, sizeof(frame_t));
  s_buf_head = buf_next(s_buf_head);
  s_buf_count++;
  return true;
}

static bool buf_pop(frame_t *frame) {
  if (s_buf_count == 0) {
    return false;
  }
  memcpy(frame, &s_frame_buf[s_buf_tail], sizeof(frame_t));
  s_buf_tail = buf_next(s_buf_tail);
  s_buf_count--;
  return true;
}

static uint16_t buf_buffer_time_ms(void) {
  return (uint16_t)(s_buf_count * s_frame_period_ms);
}

/* ------------------------------------------------------------------ */
/* WebSocket send helpers                                             */
/* ------------------------------------------------------------------ */

static bool client_fd_valid(void) {
  if (s_client_fd < 0 || !s_server) {
    return false;
  }
  return httpd_ws_get_fd_info(s_server, s_client_fd) == HTTPD_WS_CLIENT_WEBSOCKET;
}

static void ws_send_ready(void) {
  if (!client_fd_valid()) {
    return;
  }
  uint8_t payload[4];
  payload[0] = (s_led_count >> 8) & 0xFF;
  payload[1] = s_led_count & 0xFF;
  payload[2] = 0;
  payload[3] = 60; // max_fps

  httpd_ws_frame_t frame = {
      .type = HTTPD_WS_TYPE_BINARY,
      .payload = payload,
      .len = sizeof(payload),
  };
  httpd_ws_send_frame_async(s_server, s_client_fd, &frame);
}

static void ws_send_buffer_status(void) {
  if (!client_fd_valid()) {
    return;
  }
  uint8_t payload[6];
  payload[0] = (s_last_rendered_frame_id >> 24) & 0xFF;
  payload[1] = (s_last_rendered_frame_id >> 16) & 0xFF;
  payload[2] = (s_last_rendered_frame_id >> 8) & 0xFF;
  payload[3] = s_last_rendered_frame_id & 0xFF;
  uint16_t bt = buf_buffer_time_ms();
  payload[4] = (bt >> 8) & 0xFF;
  payload[5] = bt & 0xFF;

  httpd_ws_frame_t frame = {
      .type = HTTPD_WS_TYPE_BINARY,
      .payload = payload,
      .len = sizeof(payload),
  };
  httpd_ws_send_frame_async(s_server, s_client_fd, &frame);
}

/* ------------------------------------------------------------------ */
/* Render task                                                        */
/* ------------------------------------------------------------------ */

static void render_task(void *arg) {
  (void)arg;
  frame_t frame;
  TickType_t last_wake = xTaskGetTickCount();

  while (1) {
    if (!s_streaming) {
      vTaskDelay(pdMS_TO_TICKS(100));
      last_wake = xTaskGetTickCount();
      continue;
    }

    bool have_frame = false;
    bool buffer_low = false;
    if (xSemaphoreTake(s_buf_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      have_frame = buf_pop(&frame);
      buffer_low = s_buf_count < 2;
      xSemaphoreGive(s_buf_mutex);
    }

    if (have_frame) {
      led_strip_ctrl_set_pixels(frame.pixels, s_led_count);
      s_last_rendered_frame_id = frame.frame_id;

      // If buffer is running low, ask for more frames.
      if (buffer_low) {
        ws_send_buffer_status();
      }
    }

    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(s_frame_period_ms));
  }
}

/* ------------------------------------------------------------------ */
/* Protocol handlers                                                  */
/* ------------------------------------------------------------------ */

static void handle_stream_start(const uint8_t *payload, size_t len) {
  if (len < 4) {
    ESP_LOGW(TAG, "STREAM_START too short");
    return;
  }
  uint16_t led_count = (uint16_t)((payload[0] << 8) | payload[1]);
  uint16_t fps = (uint16_t)((payload[2] << 8) | payload[3]);

  if (led_count == 0 || led_count > CONFIG_LED_ANIM_COUNT) {
    ESP_LOGW(TAG, "STREAM_START invalid led_count %d", led_count);
    return;
  }
  if (fps == 0 || fps > 120) {
    fps = 30;
  }

  xSemaphoreTake(s_buf_mutex, portMAX_DELAY);
  s_led_count = led_count;
  s_fps = fps;
  s_frame_period_ms = 1000 / fps;
  s_buf_head = s_buf_tail = s_buf_count = 0;
  s_streaming = true;
  xSemaphoreGive(s_buf_mutex);

  ESP_LOGI(TAG, "Stream started: %d LEDs @ %d fps", s_led_count, s_fps);
  ws_send_ready();
}

static void handle_frame(const uint8_t *payload, size_t len) {
  if (len < 8 + (size_t)s_led_count * BYTES_PER_LED) {
    ESP_LOGW(TAG, "FRAME too short for %d LEDs", s_led_count);
    return;
  }

  frame_t frame;
  frame.frame_id = ((uint32_t)payload[0] << 24) |
                   ((uint32_t)payload[1] << 16) |
                   ((uint32_t)payload[2] << 8) | payload[3];
  frame.timestamp_ms = ((uint32_t)payload[4] << 24) |
                       ((uint32_t)payload[5] << 16) |
                       ((uint32_t)payload[6] << 8) | payload[7];
  memcpy(frame.pixels, payload + 8, (size_t)s_led_count * BYTES_PER_LED);

  xSemaphoreTake(s_buf_mutex, portMAX_DELAY);
  bool streaming = s_streaming;

  if (streaming) {
    // Animation mode: queue frame for timed playback.
    if (!buf_push(&frame)) {
      frame_t dummy;
      buf_pop(&dummy);
      buf_push(&frame);
      ESP_LOGD(TAG, "Buffer full, dropped oldest frame");
    }
  }
  xSemaphoreGive(s_buf_mutex);

  if (!streaming) {
    // Single-frame / color-picker mode: render immediately and persist.
    led_strip_ctrl_set_pixels(frame.pixels, s_led_count);
    s_last_rendered_frame_id = frame.frame_id;
  }
}

static void handle_stream_stop(void) {
  xSemaphoreTake(s_buf_mutex, portMAX_DELAY);
  s_streaming = false;
  s_buf_head = s_buf_tail = s_buf_count = 0;
  xSemaphoreGive(s_buf_mutex);
  led_strip_ctrl_clear();
  ESP_LOGI(TAG, "Stream stopped");
}

/* ------------------------------------------------------------------ */
/* WebSocket handler                                                  */
/* ------------------------------------------------------------------ */

#define WS_RX_BUF_SIZE 512

static esp_err_t ws_leds_handler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
    // Handshake handled by httpd.
    s_client_fd = httpd_req_to_sockfd(req);
    return ESP_OK;
  }

  s_client_fd = httpd_req_to_sockfd(req);

  uint8_t rx_buf[WS_RX_BUF_SIZE];
  httpd_ws_frame_t frame = {0};
  frame.payload = rx_buf;

  if (httpd_ws_recv_frame(req, &frame, sizeof(rx_buf)) != ESP_OK) {
    return ESP_OK;
  }

  if (frame.type == HTTPD_WS_TYPE_CLOSE) {
    s_client_fd = -1;
    handle_stream_stop();
    return ESP_OK;
  }

  if (frame.type != HTTPD_WS_TYPE_BINARY || frame.len < 2) {
    return ESP_OK;
  }

  if (frame.len > sizeof(rx_buf)) {
    ESP_LOGW(TAG, "Frame too large: %d bytes", (int)frame.len);
    return ESP_OK;
  }

  if (rx_buf[0] != PROTO_VERSION) {
    ESP_LOGW(TAG, "Unknown protocol version %d", rx_buf[0]);
    return ESP_OK;
  }

  uint8_t type = rx_buf[1];
  const uint8_t *payload = rx_buf + 2;
  size_t payload_len = frame.len - 2;

  switch (type) {
  case MSG_STREAM_START:
    handle_stream_start(payload, payload_len);
    break;
  case MSG_FRAME:
    handle_frame(payload, payload_len);
    break;
  case MSG_STREAM_STOP:
    handle_stream_stop();
    break;
  default:
    ESP_LOGW(TAG, "Unknown message type 0x%02x", type);
    break;
  }

  return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void led_anim_stream_init(void) {
  led_strip_ctrl_init();

  s_buf_mutex = xSemaphoreCreateMutex();
  if (!s_buf_mutex) {
    ESP_LOGE(TAG, "Failed to create buffer mutex");
    return;
  }

  s_led_count = CONFIG_LED_ANIM_COUNT;
  s_fps = 30;
  s_frame_period_ms = 33;

  xTaskCreate(render_task, "led_render", 4096, NULL, 5, &s_render_task);
  ESP_LOGI(TAG, "LED animation stream initialized");
}

esp_err_t led_anim_stream_register(httpd_handle_t server) {
  s_server = server;

  httpd_uri_t ws_uri = {
      .uri = "/ws/leds",
      .method = HTTP_GET,
      .handler = ws_leds_handler,
      .is_websocket = true,
  };

  esp_err_t err = httpd_register_uri_handler(server, &ws_uri);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register /ws/leds: %s", esp_err_to_name(err));
    return err;
  }

  ESP_LOGI(TAG, "LED animation WebSocket registered on /ws/leds");
  return ESP_OK;
}
