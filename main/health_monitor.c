/**
 * @file health_monitor.c
 * @brief Software-watchdog task + MQTT heartbeat. Decision logic is in
 *        health_logic.c (pure, host-tested).
 */
#include "health_monitor.h"
#include "health_logic.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "mqtt_client.h"

#include "app_common.h"
#include "config.h"

static const char *TAG = "HEALTH";

/* --- Tunables ---------------------------------------------------------- */
#define BOOT_GRACE_MS            90000   /* cover init + WiFi/MQTT bring-up   */
#define SIM_STALL_TIMEOUT_MS     60000   /* rx_task heartbeats <1s; 60s = dead*/
#define MQTT_OFFLINE_TIMEOUT_MS  300000  /* 5 min unable to deliver -> reboot */
#define HEALTH_CHECK_PERIOD_MS   1000
#define HEARTBEAT_INTERVAL_MS    30000   /* publish liveness heartbeat        */

/* RTC marker survives a SW reset (esp_restart) but not power loss. It lets the
 * NEXT boot's heartbeat report exactly WHY the software watchdog rebooted. */
#define SW_MARKER_MAGIC   0xA5C30000u
#define SW_REASON_MQTT    1u
#define SW_REASON_SIM     2u
RTC_NOINIT_ATTR static uint32_t s_sw_restart_marker;

static volatile int64_t s_last_sim_heartbeat_ms = 0;

static char s_device_id[24]   = "ESP32_unknown";
static uint32_t s_boot_id      = 0;
static const char *s_reset_reason = "UNKNOWN";

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

void health_notify_sim_alive(void)
{
    s_last_sim_heartbeat_ms = now_ms();
}

/* Mark the reason just before a software-watchdog reboot, then restart. */
static void sw_watchdog_restart(uint32_t reason_code)
{
    s_sw_restart_marker = SW_MARKER_MAGIC | (reason_code & 0xFFFFu);
    vTaskDelay(pdMS_TO_TICKS(50)); /* let the log flush */
    esp_restart();
}

/* One-shot: read & consume the RTC marker, combine with esp_reset_reason(). */
static const char *compute_reset_reason(void)
{
    uint32_t marker = s_sw_restart_marker;
    s_sw_restart_marker = 0; /* consume regardless of outcome */

    esp_reset_reason_t r = esp_reset_reason();

    if (r == ESP_RST_SW && (marker & 0xFFFF0000u) == SW_MARKER_MAGIC) {
        switch (marker & 0xFFFFu) {
        case SW_REASON_MQTT: return "SW_WATCHDOG_MQTT";
        case SW_REASON_SIM:  return "SW_WATCHDOG_SIM";
        default:             return "SW";
        }
    }

    switch (r) {
    case ESP_RST_POWERON:  return "POWERON";
    case ESP_RST_SW:       return "SW";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_INT_WDT:  return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT:      return "WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_DEEPSLEEP:return "DEEPSLEEP";
    case ESP_RST_EXT:      return "EXT";
    default:               return "OTHER";
    }
}

static void init_identity(void)
{
    s_boot_id = esp_random();
    s_reset_reason = compute_reset_reason();

    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        snprintf(s_device_id, sizeof(s_device_id), "ESP32_%02x%02x%02x",
                 mac[3], mac[4], mac[5]);
    }
    ESP_LOGI(TAG, "Identity: device=%s boot_id=%u reset_reason=%s",
             s_device_id, (unsigned)s_boot_id, s_reset_reason);
}

static void publish_heartbeat(int64_t t, bool mqtt_up)
{
    if (!mqtt_client || !mqtt_up) return;

    heartbeat_info_t hb = {
        .device         = s_device_id,
        .reset_reason   = s_reset_reason,
        .boot_id        = s_boot_id,
        .uptime_s       = (uint32_t)(t / 1000),
        .free_heap      = (uint32_t)esp_get_free_heap_size(),
        .mqtt_connected = mqtt_up,
    };

    char buf[192];
    if (format_heartbeat_json(buf, sizeof(buf), &hb) > 0) {
        /* QoS 0, no retain: heartbeats are frequent and disposable; a retained
         * stale heartbeat would otherwise make a dead device look alive to a
         * freshly started subscriber. */
        esp_mqtt_client_publish(mqtt_client, MQTT_HEARTBEAT_TOPIC, buf, 0, 0, 0);
    }
}

static void health_task(void *arg)
{
    (void)arg;
    init_identity();

    const int64_t boot_ms = now_ms();
    const int64_t boot_grace_until = boot_ms + BOOT_GRACE_MS;
    int64_t last_mqtt_connected_ms = boot_ms; /* seed: "never connected" ref */
    int64_t last_heartbeat_ms = 0;            /* 0 => publish on first chance */
    s_last_sim_heartbeat_ms = boot_ms;

    ESP_LOGI(TAG, "Software watchdog started (grace %d ms, sim %d ms, mqtt %d ms, hb %d ms)",
             BOOT_GRACE_MS, SIM_STALL_TIMEOUT_MS, MQTT_OFFLINE_TIMEOUT_MS, HEARTBEAT_INTERVAL_MS);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(HEALTH_CHECK_PERIOD_MS));

        const int64_t t = now_ms();
        const bool mqtt_up = (g_app_state == APP_STATE_MQTT_CONNECTED);
        if (mqtt_up) {
            last_mqtt_connected_ms = t;
        }

        /* Periodic heartbeat (publish promptly once MQTT is up). */
        if (mqtt_up && (last_heartbeat_ms == 0 || (t - last_heartbeat_ms) >= HEARTBEAT_INTERVAL_MS)) {
            publish_heartbeat(t, mqtt_up);
            last_heartbeat_ms = t;
        }

        const health_snapshot_t snap = {
            .now_ms                  = t,
            .last_sim_heartbeat_ms   = s_last_sim_heartbeat_ms,
            .last_mqtt_connected_ms  = last_mqtt_connected_ms,
            .mqtt_connected          = mqtt_up,
            .boot_grace_until_ms     = boot_grace_until,
            .sim_stall_timeout_ms    = SIM_STALL_TIMEOUT_MS,
            .mqtt_offline_timeout_ms = MQTT_OFFLINE_TIMEOUT_MS,
        };

        switch (health_evaluate(&snap)) {
        case HEALTH_RESTART_SIM_STALL:
            ESP_LOGE(TAG, "SIM rx_task stalled (no heartbeat for %lld ms) -> restarting",
                     (long long)(t - s_last_sim_heartbeat_ms));
            sw_watchdog_restart(SW_REASON_SIM);
            break;
        case HEALTH_RESTART_MQTT_OFFLINE:
            ESP_LOGE(TAG, "MQTT offline for %lld ms -> restarting",
                     (long long)(t - last_mqtt_connected_ms));
            sw_watchdog_restart(SW_REASON_MQTT);
            break;
        case HEALTH_OK:
        default:
            break;
        }
    }
}

void health_monitor_start(void)
{
    /* Priority above rx_task(5) so the monitor still runs even if lower tasks
     * are busy; it spends almost all its time sleeping. */
    xTaskCreate(health_task, "health_task", 3072, NULL, 6, NULL);
}
