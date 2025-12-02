#pragma once

#include "esp_log.h"
#include "mqtt_client.h"

typedef enum {
    APP_STATE_INIT,           // Startup / No Network (Solid On)
    APP_STATE_WIFI_CONNECTED, // Got IP, No MQTT (Fast Blink)
    APP_STATE_MQTT_CONNECTED  // Got IP + MQTT (Slow Blink)
} app_state_t;

extern volatile app_state_t g_app_state;
extern esp_mqtt_client_handle_t mqtt_client;

// Common TAG for logging (or use specific ones in files)
// #define TAG "SMS_RECEIVER" 
