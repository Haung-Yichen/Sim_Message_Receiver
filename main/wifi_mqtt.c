#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "mqtt_client.h"
#include "app_common.h"
#include "config.h"
#include "sim_modem.h"

static const char *TAG = "WIFI_MQTT";

// Globals defined in app_common.h
volatile app_state_t g_app_state = APP_STATE_INIT;
esp_mqtt_client_handle_t mqtt_client = NULL;

// --- WiFi 重連控制（不在 event handler 內 block）---
#define WIFI_RECONNECT_DELAY_MS   5000   // 重連間隔（節流，避免猛打）
#define WIFI_MAX_RECONNECT        60     // 連續失敗約 5 分鐘仍連不上 -> 重啟
static esp_timer_handle_t s_wifi_reconnect_timer = NULL;
static int s_wifi_reconnect_count = 0;

static void mqtt_app_start(void);

// esp_timer callback：跑在 timer task，非 event task，不會卡住事件迴圈
static void wifi_reconnect_timer_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%ld", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        g_app_state = APP_STATE_MQTT_CONNECTED;
        // Trigger SIM to read and send any stored messages
        sim_modem_trigger_flush();
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        if (g_app_state == APP_STATE_MQTT_CONNECTED) {
            g_app_state = APP_STATE_WIFI_CONNECTED;
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "Last error code reported from esp-tls: 0x%x", event->error_handle->esp_tls_last_esp_err);
            ESP_LOGE(TAG, "Last error code reported from tls stack: 0x%x", event->error_handle->esp_tls_stack_err);
            ESP_LOGE(TAG, "Last error code reported from socket errno: %d", event->error_handle->esp_transport_sock_errno);
        }
        break;
    default:
        break;
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        g_app_state = APP_STATE_INIT;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        g_app_state = APP_STATE_INIT;
        s_wifi_reconnect_count++;

        // 連續重連失敗超過上限 -> 乾淨重啟（避免「在線但永遠連不回來」的假死）
        if (s_wifi_reconnect_count > WIFI_MAX_RECONNECT) {
            ESP_LOGE(TAG, "WiFi reconnect failed %d times, restarting...", s_wifi_reconnect_count);
            esp_restart();
        }

        ESP_LOGI(TAG, "WiFi disconnected, retry #%d in %d ms",
                 s_wifi_reconnect_count, WIFI_RECONNECT_DELAY_MS);

        // 用 oneshot timer 做節流重連，不在 event handler 內 vTaskDelay 卡住事件迴圈
        if (s_wifi_reconnect_timer) {
            esp_timer_stop(s_wifi_reconnect_timer); // 若已在跑先停，避免 INVALID_STATE
            esp_timer_start_once(s_wifi_reconnect_timer,
                                 (uint64_t)WIFI_RECONNECT_DELAY_MS * 1000);
        } else {
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));

        s_wifi_reconnect_count = 0; // 連上了，歸零

        if (g_app_state != APP_STATE_MQTT_CONNECTED) {
            g_app_state = APP_STATE_WIFI_CONNECTED;
        }

        if (mqtt_client == NULL) {
            mqtt_app_start();
        } else {
            // If MQTT client exists but was disconnected, it should auto-reconnect.
            // But if we want to force it or check status, we can.
            // Usually esp_mqtt_client handles reconnection if network comes back.
        }
    }
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .network.reconnect_timeout_ms = 10000,
        .network.disable_auto_reconnect = false,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return;
    }
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

void wifi_mqtt_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    
    // 建立 WiFi 重連節流用的 oneshot timer
    esp_timer_create_args_t reconnect_timer_args = {
        .callback = &wifi_reconnect_timer_cb,
        .name = "wifi_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&reconnect_timer_args, &s_wifi_reconnect_timer));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");
}
