/**
 * @file health_logic.c
 * @brief Pure decision logic for the software watchdog (see header).
 */
#include "health_logic.h"
#include <stdio.h>

health_verdict_t health_evaluate(const health_snapshot_t *s)
{
    if (!s) return HEALTH_OK;

    /* During the boot grace window WiFi/MQTT/SIM are still coming up, and the
     * SIM init sequence holds rx_task in blocking delays for several seconds.
     * Never restart in this window. */
    if (s->now_ms < s->boot_grace_until_ms) {
        return HEALTH_OK;
    }

    /* rx_task heartbeats well under a second on every loop iteration. If it has
     * gone silent for the stall timeout, the SIM-reading task is wedged. */
    if (s->sim_stall_timeout_ms > 0 &&
        (s->now_ms - s->last_sim_heartbeat_ms) > s->sim_stall_timeout_ms) {
        return HEALTH_RESTART_SIM_STALL;
    }

    /* "Alive but cannot deliver": the device is running but MQTT has been
     * unreachable for too long (WiFi never came back, broker gone, etc.).
     * last_mqtt_connected_ms is seeded to boot time, so a device that never
     * connects is also caught once the timeout elapses. */
    if (!s->mqtt_connected && s->mqtt_offline_timeout_ms > 0 &&
        (s->now_ms - s->last_mqtt_connected_ms) > s->mqtt_offline_timeout_ms) {
        return HEALTH_RESTART_MQTT_OFFLINE;
    }

    return HEALTH_OK;
}

int format_heartbeat_json(char *buf, size_t buf_size, const heartbeat_info_t *hb)
{
    if (!buf || buf_size == 0 || !hb) return -1;

    int n = snprintf(buf, buf_size,
        "{\"device\":\"%s\",\"boot_id\":%u,\"reset_reason\":\"%s\","
        "\"uptime_s\":%u,\"free_heap\":%u,\"mqtt\":%s}",
        hb->device ? hb->device : "",
        (unsigned)hb->boot_id,
        hb->reset_reason ? hb->reset_reason : "",
        (unsigned)hb->uptime_s,
        (unsigned)hb->free_heap,
        hb->mqtt_connected ? "true" : "false");

    if (n < 0 || (size_t)n >= buf_size) return -1; /* truncated */
    return n;
}
