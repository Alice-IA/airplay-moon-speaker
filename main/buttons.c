/**
 * System button driver — interrupt-driven with timer-based debounce.
 *
 * Single button, system control only (no media — AirPlay 2/MRP limitation).
 *
 *   1 click         → toggle control mode (MQTT <-> Matter), reboot
 *   hold 5 seconds  → factory reset (erase NVS, reboot to setup)
 *
 * Each button GPIO triggers an ISR on any edge. The ISR resets a FreeRTOS
 * software timer (the debounce window). When the timer expires — meaning the
 * signal has been stable for DEBOUNCE_MS — the callback reads the GPIO and
 * acts on the new state.
 *
 * Actions are dispatched to a dedicated task via a queue so that NVS writes
 * and esp_restart() never run inside the FreeRTOS timer daemon.
 */

#include "buttons.h"
#include "audio_output.h"
#include "playback_control.h"
#include "settings.h"
#include "spiram_task.h"

#include "board_common.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/timers.h"

static const char *TAG = "buttons";

#define DEBOUNCE_MS      50    // Stable period before accepting state change
#define FACTORY_RESET_MS 5000  // Hold to factory reset (erase NVS, reboot)
#define ACTION_QUEUE_LEN 4

typedef enum {
  BTN_SYSTEM = 0,     // The single physical button
  // Actions dispatched to the task (not 1:1 with GPIO)
  BTN_MODE_SWITCH,    // 1 click: toggle MQTT <-> Matter
  BTN_FACTORY_RESET,  // 5s hold: erase NVS + reboot
  BTN_COUNT
} button_id_t;

typedef struct {
  int gpio;
  bool pressed;                 // Debounced state
  TimerHandle_t debounce_timer;
  TimerHandle_t factory_reset_timer; // 5s hold
  bool factory_reset_fired;     // 5s already triggered (ignore release)
} button_state_t;

static button_state_t buttons[BTN_COUNT];
static QueueHandle_t s_action_queue;

// Post a button action to the dedicated task (safe from timer callbacks)
static void post_button_action(button_id_t id) {
  int action = (int)id;
  xQueueSend(s_action_queue, &action, 0); // drop if full, never block timer task
}

// Dedicated task that processes button actions (NVS writes + reboot).
static void button_action_task(void *pvParameters) {
  (void)pvParameters;
  int action;
  while (1) {
    if (xQueueReceive(s_action_queue, &action, portMAX_DELAY) == pdTRUE) {
      switch ((button_id_t)action) {
      case BTN_MODE_SWITCH: {
        uint8_t mode;
        settings_get_ctrl_mode(&mode);
        uint8_t new_mode = (mode == SETTINGS_CTRL_MODE_MATTER)
                               ? SETTINGS_CTRL_MODE_MQTT
                               : SETTINGS_CTRL_MODE_MATTER;
        ESP_LOGW(TAG, "Switching control mode to %s — rebooting",
                 new_mode == SETTINGS_CTRL_MODE_MATTER ? "Matter" : "MQTT");
        settings_set_ctrl_mode(new_mode);
        vTaskDelay(pdMS_TO_TICKS(500)); // let the log flush
        esp_restart();
        break;
      }
      case BTN_FACTORY_RESET:
        ESP_LOGW(TAG, "Factory reset — erasing all settings, rebooting");
        settings_factory_reset();
        vTaskDelay(pdMS_TO_TICKS(500)); // let the log flush
        esp_restart();
        break;
      default:
        break;
      }
    }
  }
}

// 5s hold — factory reset. Fires in the timer daemon; posts to the task.
static void factory_reset_timer_cb(TimerHandle_t timer) {
  int id = (int)(intptr_t)pvTimerGetTimerID(timer);
  button_state_t *btn = &buttons[id];

  btn->factory_reset_fired = true; // suppress the click on release
  ESP_LOGW(TAG, "5s hold detected — factory reset");
  post_button_action(BTN_FACTORY_RESET);
}

// Called when debounce timer expires (runs in timer daemon task)
static void debounce_timer_cb(TimerHandle_t timer) {
  int id = (int)(intptr_t)pvTimerGetTimerID(timer);
  button_state_t *btn = &buttons[id];

  // Read settled GPIO state (active low)
  bool now_pressed = (gpio_get_level(btn->gpio) == 0);

  if (now_pressed == btn->pressed) {
    return; // No actual state change after debounce
  }

  btn->pressed = now_pressed;
  ESP_LOGI(TAG, "Button (GPIO %d) %s", btn->gpio,
           now_pressed ? "PRESSED" : "released");

  if (now_pressed) {
    btn->factory_reset_fired = false;
    // Start the 5s factory-reset timer
    if (btn->factory_reset_timer) {
      xTimerStart(btn->factory_reset_timer, 0);
    }
  } else {
    // Button just released — stop the 5s timer
    if (btn->factory_reset_timer) {
      xTimerStop(btn->factory_reset_timer, 0);
    }

    // If 5s already fired (factory reset), don't treat release as a click.
    if (btn->factory_reset_fired) {
      btn->factory_reset_fired = false;
      return;
    }

    // Short click → toggle control mode
    post_button_action(BTN_MODE_SWITCH);
  }
}

// GPIO ISR — just resets the debounce timer. Each new edge restarts the
// debounce window so the callback only fires once bouncing stops.
static void IRAM_ATTR gpio_isr_handler(void *arg) {
  int id = (int)(intptr_t)arg;
  BaseType_t woken = pdFALSE;
  xTimerResetFromISR(buttons[id].debounce_timer, &woken);
  if (woken) {
    portYIELD_FROM_ISR();
  }
}

static void configure_button(button_id_t id, int gpio) {
  buttons[id].gpio = gpio;
  buttons[id].pressed = false;
  buttons[id].debounce_timer = NULL;
  buttons[id].factory_reset_timer = NULL;
  buttons[id].factory_reset_fired = false;

  if (gpio < 0) {
    return;
  }

  buttons[id].debounce_timer =
      xTimerCreate("btn_db", pdMS_TO_TICKS(DEBOUNCE_MS), pdFALSE, // one-shot
                   (void *)(intptr_t)id, debounce_timer_cb);

  buttons[id].factory_reset_timer = xTimerCreate(
      "btn_frst", pdMS_TO_TICKS(FACTORY_RESET_MS), pdFALSE, // one-shot
      (void *)(intptr_t)id, factory_reset_timer_cb);

  // ESP32 classic: GPIOs 34-39 are input-only and lack internal pull-ups.
  // ESP32-S3: ALL GPIOs (0-48) have internal pull-ups — always enable them.
#ifdef CONFIG_IDF_TARGET_ESP32
  bool has_internal_pullup = (gpio < 34);
#else
  bool has_internal_pullup = true;
#endif
  gpio_config_t io_conf = {
      .pin_bit_mask = (1ULL << gpio),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en =
          has_internal_pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_ANYEDGE,
  };
  gpio_config(&io_conf);

  gpio_isr_handler_add(gpio, gpio_isr_handler, (void *)(intptr_t)id);

  if (!has_internal_pullup) {
    ESP_LOGW(TAG, "Button on GPIO %d: no internal pull-up, needs external",
             gpio);
  }
  ESP_LOGI(TAG, "System button on GPIO %d (interrupt)", gpio);
}

esp_err_t buttons_init(void) {
  // Action-only IDs — not backed by GPIO
  buttons[BTN_MODE_SWITCH].gpio = -1;
  buttons[BTN_FACTORY_RESET].gpio = -1;

  // Ensure the shared GPIO ISR service is installed (idempotent)
  esp_err_t err = board_gpio_isr_init();
  if (err != ESP_OK) {
    return err;
  }

  configure_button(BTN_SYSTEM, CONFIG_BTN_PLAY_PAUSE_GPIO);

  if (buttons[BTN_SYSTEM].gpio < 0) {
    ESP_LOGI(TAG, "No button configured");
    return ESP_OK;
  }

  s_action_queue = xQueueCreate(ACTION_QUEUE_LEN, sizeof(int));
  task_create_spiram(button_action_task, "btn_act", 4096, NULL, 5, NULL, NULL);

  ESP_LOGI(TAG, "Button initialized (1 click=mode, 5s=factory reset)");
  return ESP_OK;
}
