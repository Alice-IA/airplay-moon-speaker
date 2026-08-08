/**
 * WebSocket binary stream for LED animation frames + built-in effects.
 *
 * Endpoint: /ws/leds
 * Protocol version 1
 *
 * Client -> ESP32:
 *   STREAM_START  0x01  payload: led_count (u16), fps (u16), flags (u8)
 *                              flags bit 0: loop (1=store frames, 0=play once)
 *   FRAME         0x02  payload: frame_id (u32), timestamp_ms (u32),
 *                              pixels[LED_COUNT * 4] (RGBA)
 *   STREAM_STOP   0x03  no payload
 *
 * ESP32 -> Client:
 *   STREAM_READY  0x81  payload: led_count (u16), max_fps (u16)
 *   BUFFER_STATUS 0x82  payload: last_frame_id (u32), buffer_time_ms (u16)
 *
 * Built-in effects are started via led_anim_stream_start_effect() or the
 * HTTP endpoint POST /api/led/effect.
 */

#include "led_anim_stream.h"

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "led_strip_ctrl.h"

#include <math.h>
#include <strings.h>
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

#define STREAM_FLAG_LOOP 0x01

/* Maximum frames stored for loop mode. With 12 LEDs this is ~288 KB,
 * with 256 LEDs it is ~6 MB, so it must fit in available PSRAM. */
#define LOOP_MAX_FRAMES 2000

#define EFFECT_FPS 30

/* ------------------------------------------------------------------ */
/* Mode and state                                                     */
/* ------------------------------------------------------------------ */

typedef enum {
  MODE_OFF,
  MODE_STATIC,
  MODE_STREAMING,
  MODE_STREAMING_LOOP,
  MODE_EFFECT,
} led_mode_t;

typedef struct {
  uint32_t frame_id;
  uint32_t timestamp_ms;
  uint8_t  pixels[MAX_FRAME_BYTES];
} frame_t;

typedef struct {
  uint8_t *data;          /* PSRAM or internal RAM */
  uint16_t capacity;      /* max frames that fit */
  uint16_t count;         /* frames currently stored */
  uint16_t read_pos;      /* playback position */
  uint16_t led_count;
  uint8_t  fps;
} loop_state_t;

typedef struct {
  const char *name;
  void (*render)(uint8_t *pixels, uint16_t led_count, uint32_t time_ms,
                 uint8_t speed, uint8_t intensity,
                 uint32_t color1, uint32_t color2, uint8_t brightness);
} effect_entry_t;

static httpd_handle_t s_server = NULL;
static int            s_client_fd = -1;

static SemaphoreHandle_t s_buf_mutex = NULL;
static frame_t           s_frame_buf[FRAME_BUF_COUNT];
static volatile uint8_t  s_buf_head = 0;
static volatile uint8_t  s_buf_tail = 0;
static volatile uint8_t  s_buf_count = 0;

static volatile led_mode_t s_mode = MODE_OFF;
static uint16_t            s_led_count = 0;
static uint16_t            s_fps = 30;
static uint32_t            s_frame_period_ms = 33;
static uint32_t            s_last_rendered_frame_id = 0;

static loop_state_t s_loop = {0};

static struct {
  const char *name;
  uint8_t     speed;
  uint8_t     intensity;
  uint8_t     brightness;
  uint32_t    color1;
  uint32_t    color2;
} s_effect_cfg = {0};

static uint8_t s_static_pixels[MAX_FRAME_BYTES] = {0};

static TaskHandle_t s_render_task = NULL;

/* ------------------------------------------------------------------ */
/* Color helpers                                                      */
/* ------------------------------------------------------------------ */

static inline void rgb_unpack(uint32_t rgb, uint8_t *r, uint8_t *g, uint8_t *b) {
  *r = (rgb >> 16) & 0xFF;
  *g = (rgb >> 8) & 0xFF;
  *b = rgb & 0xFF;
}

static void hsv_to_rgb(uint8_t *r, uint8_t *g, uint8_t *b, float h, float s, float v) {
  if (s <= 0.0f) {
    *r = *g = *b = (uint8_t)(v * 255.0f);
    return;
  }
  int i = (int)(h * 6.0f);
  float f = h * 6.0f - i;
  float p = v * (1.0f - s);
  float q = v * (1.0f - s * f);
  float t = v * (1.0f - s * (1.0f - f));
  i = i % 6;
  float rf = 0, gf = 0, bf = 0;
  switch (i) {
  case 0: rf = v; gf = t; bf = p; break;
  case 1: rf = q; gf = v; bf = p; break;
  case 2: rf = p; gf = v; bf = t; break;
  case 3: rf = p; gf = q; bf = v; break;
  case 4: rf = t; gf = p; bf = v; break;
  case 5: rf = v; gf = p; bf = q; break;
  }
  *r = (uint8_t)(rf * 255.0f);
  *g = (uint8_t)(gf * 255.0f);
  *b = (uint8_t)(bf * 255.0f);
}

static inline uint8_t scale8(uint8_t v, uint8_t scale) {
  return (uint8_t)(((uint16_t)v * scale) >> 8);
}

static void apply_brightness(uint8_t *pixels, uint16_t led_count, uint8_t brightness) {
  if (brightness == 255) {
    return;
  }
  for (uint16_t i = 0; i < led_count; i++) {
    pixels[i * 4 + 0] = scale8(pixels[i * 4 + 0], brightness);
    pixels[i * 4 + 1] = scale8(pixels[i * 4 + 1], brightness);
    pixels[i * 4 + 2] = scale8(pixels[i * 4 + 2], brightness);
  }
}

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
/* Loop storage                                                       */
/* ------------------------------------------------------------------ */

static void loop_free(void) {
  if (s_loop.data) {
    heap_caps_free(s_loop.data);
    s_loop.data = NULL;
  }
  s_loop.capacity = 0;
  s_loop.count = 0;
  s_loop.read_pos = 0;
}

static bool loop_alloc(uint16_t led_count) {
  loop_free();
  size_t frame_bytes = (size_t)led_count * BYTES_PER_LED;
  size_t total = (size_t)LOOP_MAX_FRAMES * frame_bytes;

  s_loop.data = (uint8_t *)heap_caps_malloc(total,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_loop.data) {
    s_loop.data = (uint8_t *)malloc(total);
  }
  if (!s_loop.data) {
    ESP_LOGE(TAG, "Failed to allocate loop buffer (%zu bytes)", total);
    return false;
  }
  s_loop.capacity = LOOP_MAX_FRAMES;
  s_loop.count = 0;
  s_loop.read_pos = 0;
  s_loop.led_count = led_count;
  return true;
}

static bool loop_store_frame(const uint8_t *pixels, uint16_t led_count) {
  if (!s_loop.data || s_loop.count >= s_loop.capacity) {
    return false;
  }
  size_t frame_bytes = (size_t)led_count * BYTES_PER_LED;
  memcpy(s_loop.data + (size_t)s_loop.count * frame_bytes, pixels, frame_bytes);
  s_loop.count++;
  return true;
}

static void loop_get_frame(uint8_t *pixels, uint16_t led_count) {
  if (!s_loop.data || s_loop.count == 0) {
    memset(pixels, 0, (size_t)led_count * BYTES_PER_LED);
    return;
  }
  size_t frame_bytes = (size_t)led_count * BYTES_PER_LED;
  memcpy(pixels, s_loop.data + (size_t)s_loop.read_pos * frame_bytes, frame_bytes);
  s_loop.read_pos = (uint16_t)((s_loop.read_pos + 1) % s_loop.count);
}

static void loop_reset_read(void) {
  s_loop.read_pos = 0;
}

/* ------------------------------------------------------------------ */
/* Effects                                                            */
/* ------------------------------------------------------------------ */

static void effect_off(uint8_t *pixels, uint16_t led_count, uint32_t time_ms,
                       uint8_t speed, uint8_t intensity,
                       uint32_t color1, uint32_t color2, uint8_t brightness) {
  (void)time_ms; (void)speed; (void)intensity; (void)color1; (void)color2; (void)brightness;
  memset(pixels, 0, (size_t)led_count * BYTES_PER_LED);
}

static void effect_static(uint8_t *pixels, uint16_t led_count, uint32_t time_ms,
                          uint8_t speed, uint8_t intensity,
                          uint32_t color1, uint32_t color2, uint8_t brightness) {
  (void)time_ms; (void)speed; (void)intensity; (void)color2;
  uint8_t r, g, b;
  rgb_unpack(color1, &r, &g, &b);
  for (uint16_t i = 0; i < led_count; i++) {
    pixels[i * 4 + 0] = r;
    pixels[i * 4 + 1] = g;
    pixels[i * 4 + 2] = b;
    pixels[i * 4 + 3] = 255;
  }
  apply_brightness(pixels, led_count, brightness);
}

static void effect_rainbow(uint8_t *pixels, uint16_t led_count, uint32_t time_ms,
                           uint8_t speed, uint8_t intensity,
                           uint32_t color1, uint32_t color2, uint8_t brightness) {
  (void)color1; (void)color2; (void)intensity;
  float phase = (time_ms * (speed + 1)) / 20000.0f;
  for (uint16_t i = 0; i < led_count; i++) {
    float hue = fmodf(phase + (float)i / led_count, 1.0f);
    hsv_to_rgb(&pixels[i * 4 + 0], &pixels[i * 4 + 1], &pixels[i * 4 + 2],
               hue, 1.0f, 1.0f);
    pixels[i * 4 + 3] = 255;
  }
  apply_brightness(pixels, led_count, brightness);
}

static void effect_breathe(uint8_t *pixels, uint16_t led_count, uint32_t time_ms,
                           uint8_t speed, uint8_t intensity,
                           uint32_t color1, uint32_t color2, uint8_t brightness) {
  (void)color2;
  uint8_t r, g, b;
  rgb_unpack(color1, &r, &g, &b);
  float period = 2000.0f / (speed + 1);
  float t = sinf((time_ms / period) * (float)M_PI * 2.0f) * 0.5f + 0.5f;
  t = 0.2f + t * 0.8f; /* keep a minimum glow */
  uint8_t br = (uint8_t)(t * brightness);
  for (uint16_t i = 0; i < led_count; i++) {
    pixels[i * 4 + 0] = scale8(r, br);
    pixels[i * 4 + 1] = scale8(g, br);
    pixels[i * 4 + 2] = scale8(b, br);
    pixels[i * 4 + 3] = 255;
  }
}

static void effect_chase(uint8_t *pixels, uint16_t led_count, uint32_t time_ms,
                         uint8_t speed, uint8_t intensity,
                         uint32_t color1, uint32_t color2, uint8_t brightness) {
  uint8_t r1, g1, b1, r2, g2, b2;
  rgb_unpack(color1, &r1, &g1, &b1);
  rgb_unpack(color2, &r2, &g2, &b2);
  float pos = fmodf((time_ms * (speed + 1)) / 1000.0f, (float)led_count);
  float tail = 3.0f + (intensity / 255.0f) * 10.0f;
  for (uint16_t i = 0; i < led_count; i++) {
    float dist = fabsf((float)i - pos);
    if (dist > led_count / 2.0f) {
      dist = led_count - dist;
    }
    float v = fmaxf(0.0f, 1.0f - dist / tail);
    v = v * v;
    pixels[i * 4 + 0] = scale8((uint8_t)(r1 * v + r2 * (1.0f - v)), brightness);
    pixels[i * 4 + 1] = scale8((uint8_t)(g1 * v + g2 * (1.0f - v)), brightness);
    pixels[i * 4 + 2] = scale8((uint8_t)(b1 * v + b2 * (1.0f - v)), brightness);
    pixels[i * 4 + 3] = 255;
  }
}

static void effect_sparkle(uint8_t *pixels, uint16_t led_count, uint32_t time_ms,
                           uint8_t speed, uint8_t intensity,
                           uint32_t color1, uint32_t color2, uint8_t brightness) {
  uint8_t r1, g1, b1, r2, g2, b2;
  rgb_unpack(color1, &r1, &g1, &b1);
  rgb_unpack(color2, &r2, &g2, &b2);
  uint16_t threshold = (uint16_t)(intensity * 2) + 1;
  uint32_t time_div = (speed >= 109) ? 1 : (110 - speed);
  for (uint16_t i = 0; i < led_count; i++) {
    uint32_t seed = (time_ms / time_div) * 2654435761u + i * 12345;
    if ((seed & 0xFFFF) < threshold) {
      pixels[i * 4 + 0] = scale8(r1, brightness);
      pixels[i * 4 + 1] = scale8(g1, brightness);
      pixels[i * 4 + 2] = scale8(b1, brightness);
    } else {
      pixels[i * 4 + 0] = scale8(r2, brightness);
      pixels[i * 4 + 1] = scale8(g2, brightness);
      pixels[i * 4 + 2] = scale8(b2, brightness);
    }
    pixels[i * 4 + 3] = 255;
  }
}

static void effect_fire(uint8_t *pixels, uint16_t led_count, uint32_t time_ms,
                        uint8_t speed, uint8_t intensity,
                        uint32_t color1, uint32_t color2, uint8_t brightness) {
  (void)color1; (void)color2;
  float t = time_ms * (speed + 1) / 5000.0f;
  for (uint16_t i = 0; i < led_count; i++) {
    float n = sinf(i * 0.3f + t) * cosf(i * 0.1f - t * 0.7f);
    n = (n + 1.0f) / 2.0f;
    float v = 0.3f + n * 0.7f * (intensity / 255.0f);
    uint8_t r = (uint8_t)(255 * v);
    uint8_t g = (uint8_t)(80 * v);
    uint8_t b = (uint8_t)(10 * v);
    pixels[i * 4 + 0] = scale8(r, brightness);
    pixels[i * 4 + 1] = scale8(g, brightness);
    pixels[i * 4 + 2] = scale8(b, brightness);
    pixels[i * 4 + 3] = 255;
  }
}

static const effect_entry_t s_effects[] = {
    {"off", effect_off},
    {"static", effect_static},
    {"rainbow", effect_rainbow},
    {"breathe", effect_breathe},
    {"chase", effect_chase},
    {"sparkle", effect_sparkle},
    {"fire", effect_fire},
};

static const effect_entry_t *effect_find(const char *name) {
  for (size_t i = 0; i < sizeof(s_effects) / sizeof(s_effects[0]); i++) {
    if (strcasecmp(s_effects[i].name, name) == 0) {
      return &s_effects[i];
    }
  }
  return NULL;
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
  uint8_t effect_pixels[MAX_FRAME_BYTES];
  TickType_t last_wake = xTaskGetTickCount();

  while (1) {
    led_mode_t mode;
    xSemaphoreTake(s_buf_mutex, portMAX_DELAY);
    mode = s_mode;
    xSemaphoreGive(s_buf_mutex);

    if (mode == MODE_OFF) {
      vTaskDelay(pdMS_TO_TICKS(100));
      last_wake = xTaskGetTickCount();
      continue;
    }

    if (mode == MODE_STATIC) {
      led_strip_ctrl_set_pixels(s_static_pixels, s_led_count);
      vTaskDelay(pdMS_TO_TICKS(100));
      last_wake = xTaskGetTickCount();
      continue;
    }

    if (mode == MODE_EFFECT) {
      uint32_t time_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
      const effect_entry_t *eff = effect_find(s_effect_cfg.name);
      if (eff) {
        eff->render(effect_pixels, s_led_count, time_ms,
                    s_effect_cfg.speed, s_effect_cfg.intensity,
                    s_effect_cfg.color1, s_effect_cfg.color2,
                    s_effect_cfg.brightness);
        led_strip_ctrl_set_pixels(effect_pixels, s_led_count);
      }
      vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(EFFECT_FPS));
      continue;
    }

    /* MODE_STREAMING_LOOP */
    if (mode == MODE_STREAMING_LOOP) {
      /* In loop mode we play back from stored frames. Hold the mutex briefly
       * while reading s_loop so it cannot be freed mid-copy. */
      if (xSemaphoreTake(s_buf_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (s_loop.count > 0) {
          loop_get_frame(effect_pixels, s_led_count);
        }
        xSemaphoreGive(s_buf_mutex);
        if (s_loop.count > 0) {
          led_strip_ctrl_set_pixels(effect_pixels, s_led_count);
        }
      }
      vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(s_frame_period_ms));
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
  uint8_t flags = (len >= 5) ? payload[4] : 0;
  bool loop = (flags & STREAM_FLAG_LOOP) != 0;

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
  s_mode = loop ? MODE_STREAMING_LOOP : MODE_STREAMING;
  if (loop) {
    loop_alloc(led_count);
    s_loop.led_count = led_count;
    s_loop.fps = fps;
  } else {
    loop_free();
  }
  xSemaphoreGive(s_buf_mutex);

  ESP_LOGI(TAG, "Stream started: %d LEDs @ %d fps loop=%d", s_led_count, s_fps, loop);
  ws_send_ready();
}

static void handle_frame(const uint8_t *payload, size_t len) {
  uint16_t expected = (uint16_t)(8 + (size_t)s_led_count * BYTES_PER_LED);
  if (len < expected) {
    ESP_LOGW(TAG, "FRAME too short for %d LEDs (got %d, expected %d)",
             s_led_count, (int)len, expected);
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
  led_mode_t mode = s_mode;

  if (mode == MODE_STREAMING) {
    /* Animation/streaming mode: queue frame for timed playback. */
    if (!buf_push(&frame)) {
      frame_t dummy;
      buf_pop(&dummy);
      buf_push(&frame);
      ESP_LOGD(TAG, "Buffer full, dropped oldest frame");
    }
  } else if (mode == MODE_STREAMING_LOOP) {
    /* Loop mode: store frames directly in PSRAM for later replay. */
    if (!loop_store_frame(frame.pixels, s_led_count)) {
      ESP_LOGW(TAG, "Loop buffer full (%d frames)", s_loop.count);
    }
  } else {
    /* Single-frame / color-picker mode: render immediately and persist. */
    memcpy(s_static_pixels, frame.pixels, (size_t)s_led_count * BYTES_PER_LED);
    s_mode = MODE_STATIC;
  }
  xSemaphoreGive(s_buf_mutex);

  s_last_rendered_frame_id = frame.frame_id;
}

static void handle_stream_stop(void) {
  xSemaphoreTake(s_buf_mutex, portMAX_DELAY);
  s_mode = MODE_OFF;
  s_buf_head = s_buf_tail = s_buf_count = 0;
  xSemaphoreGive(s_buf_mutex);
  loop_free();
  led_strip_ctrl_clear();
  ESP_LOGI(TAG, "Stream stopped");
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t led_anim_stream_start_effect(const char *name,
                                       uint8_t speed,
                                       uint8_t intensity,
                                       uint8_t brightness,
                                       uint32_t color1,
                                       uint32_t color2) {
  if (!name) {
    return ESP_ERR_INVALID_ARG;
  }
  const effect_entry_t *eff = effect_find(name);
  if (!eff) {
    ESP_LOGW(TAG, "Unknown effect '%s'", name);
    return ESP_ERR_NOT_FOUND;
  }

  xSemaphoreTake(s_buf_mutex, portMAX_DELAY);
  s_effect_cfg.name = eff->name;
  s_effect_cfg.speed = speed;
  s_effect_cfg.intensity = intensity;
  s_effect_cfg.brightness = brightness;
  s_effect_cfg.color1 = color1;
  s_effect_cfg.color2 = color2;
  s_mode = MODE_EFFECT;
  s_buf_head = s_buf_tail = s_buf_count = 0;
  loop_free();
  xSemaphoreGive(s_buf_mutex);

  ESP_LOGI(TAG, "Effect started: %s speed=%d intensity=%d brightness=%d",
           name, speed, intensity, brightness);
  return ESP_OK;
}

void led_anim_stream_stop(void) {
  xSemaphoreTake(s_buf_mutex, portMAX_DELAY);
  s_mode = MODE_OFF;
  s_buf_head = s_buf_tail = s_buf_count = 0;
  xSemaphoreGive(s_buf_mutex);
  loop_free();
  led_strip_ctrl_clear();
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

  int fd = httpd_req_to_sockfd(req);
  s_client_fd = fd;

  uint8_t rx_buf[WS_RX_BUF_SIZE];
  httpd_ws_frame_t frame = {0};
  frame.payload = rx_buf;

  if (httpd_ws_recv_frame(req, &frame, sizeof(rx_buf)) != ESP_OK) {
    /* Receive failed: client disconnected or sent malformed data.
       Clear our tracked fd and tell httpd to close the connection. */
    s_client_fd = -1;
    led_anim_stream_stop();
    return ESP_FAIL;
  }

  if (frame.type == HTTPD_WS_TYPE_CLOSE) {
    s_client_fd = -1;
    led_anim_stream_stop();
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
/* Initialization                                                     */
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
