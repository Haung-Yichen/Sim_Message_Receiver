/**
 * @file test_health_logic.c
 * @brief Unit tests for the software-watchdog decision logic (health_evaluate).
 *
 * health_evaluate() is a pure function, so we test every branch and boundary
 * directly with no mocking.
 */
#include <string.h>
#include <stdio.h>

#include "unity.h"
#include "health_logic.h"

/* Sensible defaults mirroring health_monitor.c; individual tests override. */
#define GRACE_MS        90000
#define SIM_TIMEOUT_MS  60000
#define MQTT_TIMEOUT_MS 300000

static health_snapshot_t base_snapshot(void) {
    health_snapshot_t s;
    memset(&s, 0, sizeof(s));
    s.now_ms                  = 1000000; /* well past boot grace */
    s.last_sim_heartbeat_ms   = s.now_ms;       /* fresh */
    s.last_mqtt_connected_ms  = s.now_ms;       /* fresh */
    s.mqtt_connected          = true;
    s.boot_grace_until_ms     = GRACE_MS;
    s.sim_stall_timeout_ms    = SIM_TIMEOUT_MS;
    s.mqtt_offline_timeout_ms = MQTT_TIMEOUT_MS;
    return s;
}

void test_health_null_is_ok(void) {
    TEST_ASSERT_EQUAL_INT(HEALTH_OK, health_evaluate(NULL));
}

void test_health_all_fresh_is_ok(void) {
    health_snapshot_t s = base_snapshot();
    TEST_ASSERT_EQUAL_INT(HEALTH_OK, health_evaluate(&s));
}

void test_health_boot_grace_suppresses_everything(void) {
    health_snapshot_t s = base_snapshot();
    s.now_ms = GRACE_MS - 1;          /* still in grace */
    s.last_sim_heartbeat_ms = 0;      /* would be a huge stall */
    s.mqtt_connected = false;
    s.last_mqtt_connected_ms = 0;     /* never connected */
    TEST_ASSERT_EQUAL_INT(HEALTH_OK, health_evaluate(&s));
}

void test_health_sim_stall_triggers(void) {
    health_snapshot_t s = base_snapshot();
    s.last_sim_heartbeat_ms = s.now_ms - (SIM_TIMEOUT_MS + 1);
    TEST_ASSERT_EQUAL_INT(HEALTH_RESTART_SIM_STALL, health_evaluate(&s));
}

void test_health_sim_fresh_no_trigger(void) {
    health_snapshot_t s = base_snapshot();
    s.last_sim_heartbeat_ms = s.now_ms - (SIM_TIMEOUT_MS - 1);
    TEST_ASSERT_EQUAL_INT(HEALTH_OK, health_evaluate(&s));
}

void test_health_sim_boundary_exact_is_ok(void) {
    /* exactly == timeout is NOT a stall (logic uses strict >) */
    health_snapshot_t s = base_snapshot();
    s.last_sim_heartbeat_ms = s.now_ms - SIM_TIMEOUT_MS;
    TEST_ASSERT_EQUAL_INT(HEALTH_OK, health_evaluate(&s));
}

void test_health_mqtt_offline_triggers(void) {
    health_snapshot_t s = base_snapshot();
    s.mqtt_connected = false;
    s.last_mqtt_connected_ms = s.now_ms - (MQTT_TIMEOUT_MS + 1);
    TEST_ASSERT_EQUAL_INT(HEALTH_RESTART_MQTT_OFFLINE, health_evaluate(&s));
}

void test_health_mqtt_offline_within_timeout_ok(void) {
    health_snapshot_t s = base_snapshot();
    s.mqtt_connected = false;
    s.last_mqtt_connected_ms = s.now_ms - (MQTT_TIMEOUT_MS - 1);
    TEST_ASSERT_EQUAL_INT(HEALTH_OK, health_evaluate(&s));
}

void test_health_mqtt_connected_never_offline(void) {
    health_snapshot_t s = base_snapshot();
    s.mqtt_connected = true;
    s.last_mqtt_connected_ms = s.now_ms - (MQTT_TIMEOUT_MS * 10); /* stale but connected now */
    TEST_ASSERT_EQUAL_INT(HEALTH_OK, health_evaluate(&s));
}

void test_health_never_connected_eventually_restarts(void) {
    /* Device booted, WiFi never came up. last_mqtt_connected seeded to boot(0). */
    health_snapshot_t s = base_snapshot();
    s.mqtt_connected = false;
    s.last_mqtt_connected_ms = 0;
    s.now_ms = GRACE_MS + MQTT_TIMEOUT_MS + 1;
    TEST_ASSERT_EQUAL_INT(HEALTH_RESTART_MQTT_OFFLINE, health_evaluate(&s));
}

void test_health_sim_stall_takes_priority(void) {
    /* Both conditions true -> SIM stall is reported first. */
    health_snapshot_t s = base_snapshot();
    s.mqtt_connected = false;
    s.last_sim_heartbeat_ms = s.now_ms - (SIM_TIMEOUT_MS + 1);
    s.last_mqtt_connected_ms = s.now_ms - (MQTT_TIMEOUT_MS + 1);
    TEST_ASSERT_EQUAL_INT(HEALTH_RESTART_SIM_STALL, health_evaluate(&s));
}

void test_health_zero_timeout_disables_check(void) {
    health_snapshot_t s = base_snapshot();
    s.sim_stall_timeout_ms = 0;            /* disabled */
    s.last_sim_heartbeat_ms = 0;           /* would otherwise be a huge stall */
    s.mqtt_connected = false;
    s.mqtt_offline_timeout_ms = 0;         /* disabled */
    s.last_mqtt_connected_ms = 0;
    TEST_ASSERT_EQUAL_INT(HEALTH_OK, health_evaluate(&s));
}

void run_health_logic_tests(void) {
    printf("\n=== Software Watchdog (health_evaluate) Tests ===\n");
    RUN_TEST(test_health_null_is_ok);
    RUN_TEST(test_health_all_fresh_is_ok);
    RUN_TEST(test_health_boot_grace_suppresses_everything);
    RUN_TEST(test_health_sim_stall_triggers);
    RUN_TEST(test_health_sim_fresh_no_trigger);
    RUN_TEST(test_health_sim_boundary_exact_is_ok);
    RUN_TEST(test_health_mqtt_offline_triggers);
    RUN_TEST(test_health_mqtt_offline_within_timeout_ok);
    RUN_TEST(test_health_mqtt_connected_never_offline);
    RUN_TEST(test_health_never_connected_eventually_restarts);
    RUN_TEST(test_health_sim_stall_takes_priority);
    RUN_TEST(test_health_zero_timeout_disables_check);
}
