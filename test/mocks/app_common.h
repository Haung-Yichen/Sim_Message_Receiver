#pragma once
// Mock app_common.h for host testing
#include "mqtt_client.h"

typedef enum {
    APP_STATE_INIT,
    APP_STATE_WIFI_CONNECTED,
    APP_STATE_MQTT_CONNECTED
} app_state_t;

extern volatile app_state_t g_app_state;
extern esp_mqtt_client_handle_t mqtt_client;
