#pragma once

#include "esp_err.h"

/**
 * Initialize the Moon MQTT client.
 *
 * Connects to the configured broker using the device MAC address as both
 * client ID and device ID. Subscribes to command topics under
 * moon/{device_id}/cmd/# and starts a telemetry publisher task.
 *
 * Must be called after the network (WiFi/Ethernet) and LED subsystems are
 * initialized.
 */
esp_err_t moon_mqtt_init(void);

/**
 * Publish the current device status immediately.
 * Normally called automatically by the telemetry task; exposed for testing.
 */
void moon_mqtt_publish_status(void);
