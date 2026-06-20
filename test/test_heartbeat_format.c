/**
 * @file test_heartbeat_format.c
 * @brief Unit tests for format_heartbeat_json() (pure, see health_logic.c).
 */
#include <string.h>
#include <stdio.h>

#include "unity.h"
#include "health_logic.h"

void test_hb_basic_json(void) {
    heartbeat_info_t hb = {
        .device = "ESP32_7c7038",
        .reset_reason = "POWERON",
        .boot_id = 123456789u,
        .uptime_s = 142u,
        .free_heap = 145000u,
        .mqtt_connected = true,
    };
    char buf[256];
    int n = format_heartbeat_json(buf, sizeof(buf), &hb);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING(
        "{\"device\":\"ESP32_7c7038\",\"boot_id\":123456789,\"reset_reason\":\"POWERON\","
        "\"uptime_s\":142,\"free_heap\":145000,\"mqtt\":true}",
        buf);
    TEST_ASSERT_EQUAL_INT((int)strlen(buf), n);
}

void test_hb_mqtt_false_and_wdt_reason(void) {
    heartbeat_info_t hb = {
        .device = "ESP32_7c7038",
        .reset_reason = "TASK_WDT",
        .boot_id = 1u,
        .uptime_s = 5u,
        .free_heap = 100u,
        .mqtt_connected = false,
    };
    char buf[256];
    TEST_ASSERT_TRUE(format_heartbeat_json(buf, sizeof(buf), &hb) > 0);
    TEST_ASSERT_EQUAL_STRING(
        "{\"device\":\"ESP32_7c7038\",\"boot_id\":1,\"reset_reason\":\"TASK_WDT\","
        "\"uptime_s\":5,\"free_heap\":100,\"mqtt\":false}",
        buf);
}

void test_hb_null_args(void) {
    heartbeat_info_t hb = {0};
    char buf[64];
    TEST_ASSERT_EQUAL_INT(-1, format_heartbeat_json(NULL, sizeof(buf), &hb));
    TEST_ASSERT_EQUAL_INT(-1, format_heartbeat_json(buf, 0, &hb));
    TEST_ASSERT_EQUAL_INT(-1, format_heartbeat_json(buf, sizeof(buf), NULL));
}

void test_hb_null_strings_safe(void) {
    /* device/reset_reason NULL must not crash -> empty strings */
    heartbeat_info_t hb = {
        .device = NULL, .reset_reason = NULL,
        .boot_id = 7u, .uptime_s = 0u, .free_heap = 0u, .mqtt_connected = true,
    };
    char buf[128];
    TEST_ASSERT_TRUE(format_heartbeat_json(buf, sizeof(buf), &hb) > 0);
    TEST_ASSERT_EQUAL_STRING(
        "{\"device\":\"\",\"boot_id\":7,\"reset_reason\":\"\","
        "\"uptime_s\":0,\"free_heap\":0,\"mqtt\":true}",
        buf);
}

void test_hb_truncation_returns_negative(void) {
    heartbeat_info_t hb = {
        .device = "ESP32_7c7038", .reset_reason = "POWERON",
        .boot_id = 123456789u, .uptime_s = 142u, .free_heap = 145000u,
        .mqtt_connected = true,
    };
    char small[16];
    TEST_ASSERT_EQUAL_INT(-1, format_heartbeat_json(small, sizeof(small), &hb));
}

void run_heartbeat_format_tests(void) {
    printf("\n=== Heartbeat JSON Format Tests ===\n");
    RUN_TEST(test_hb_basic_json);
    RUN_TEST(test_hb_mqtt_false_and_wdt_reason);
    RUN_TEST(test_hb_null_args);
    RUN_TEST(test_hb_null_strings_safe);
    RUN_TEST(test_hb_truncation_returns_negative);
}
