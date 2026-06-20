/**
 * @file health_logic.h
 * @brief Pure decision logic for the application-level software watchdog.
 *
 * Deliberately free of ESP-IDF / FreeRTOS dependencies so it can be unit
 * tested on the host. The ESP-specific task wrapper lives in health_monitor.c.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    HEALTH_OK = 0,
    HEALTH_RESTART_SIM_STALL,     /* rx_task stopped heartbeating          */
    HEALTH_RESTART_MQTT_OFFLINE,  /* alive but unable to deliver too long  */
} health_verdict_t;

typedef struct {
    int64_t now_ms;                 /* current monotonic time (ms)             */
    int64_t last_sim_heartbeat_ms;  /* last time rx_task proved it was alive   */
    int64_t last_mqtt_connected_ms; /* last time MQTT was connected            */
    bool    mqtt_connected;         /* MQTT connected right now?               */
    int64_t boot_grace_until_ms;    /* suppress all restarts until this time   */
    int64_t sim_stall_timeout_ms;   /* >this with no heartbeat => SIM stall    */
    int64_t mqtt_offline_timeout_ms;/* >this offline => restart (0 disables)   */
} health_snapshot_t;

/**
 * @brief Decide whether the device should reboot itself.
 *
 * Pure function: no side effects, no globals. Returns the first matching
 * restart reason, or HEALTH_OK if everything looks healthy.
 */
health_verdict_t health_evaluate(const health_snapshot_t *s);

/* --- Heartbeat payload (ESP32 -> Orange Pi over MQTT) ------------------ */

typedef struct {
    const char *device;        /* stable device id, e.g. "ESP32_7c7038" */
    const char *reset_reason;  /* why this boot started (see compute) */
    uint32_t    boot_id;       /* random per-boot id; changes on restart */
    uint32_t    uptime_s;      /* seconds since boot */
    uint32_t    free_heap;     /* bytes free heap */
    bool        mqtt_connected;
} heartbeat_info_t;

/**
 * @brief Serialize a heartbeat into compact JSON.
 *
 * Pure function. Returns the number of bytes written (excluding the null
 * terminator), or -1 on bad args / truncation.
 */
int format_heartbeat_json(char *buf, size_t buf_size, const heartbeat_info_t *hb);
