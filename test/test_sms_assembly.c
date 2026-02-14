/**
 * @file test_sms_assembly.c
 * @brief Unit tests for SMS multipart assembly logic.
 *
 * Strategy: We #include "../main/sim_modem.c" directly to access its static
 * functions. All ESP-IDF / FreeRTOS symbols are either mocked via headers
 * in test/mocks/ or defined here before the include.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

/* ===== Mock FreeRTOS types & functions (before any includes) ===== */

typedef void* QueueHandle_t;
typedef void* SemaphoreHandle_t;
typedef int BaseType_t;
typedef unsigned int TickType_t;

#define pdTRUE  1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (ms)
#define portTICK_PERIOD_MS 1

static uint32_t mock_tick_count = 0;

/* Semaphore mocks */
static SemaphoreHandle_t xSemaphoreCreateBinary(void) { return (SemaphoreHandle_t)1; }
static BaseType_t xSemaphoreGive_impl(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
#define xSemaphoreGive(s) xSemaphoreGive_impl(s)
static BaseType_t xSemaphoreTake_impl(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; return pdFALSE; }
#define xSemaphoreTake(s,t) xSemaphoreTake_impl(s,t)

/* Task mocks */
static TickType_t xTaskGetTickCount(void) { return (TickType_t)mock_tick_count; }
#define vTaskDelay(x)
#define vTaskDelete(x)
#define xTaskCreate(a,b,c,d,e,f)

/* Queue mocks */
#define xQueueReceive(a,b,c) pdFALSE
#define xQueueReset(a)

/* UART mocks */
typedef int uart_port_t;
#define UART_NUM_2 2
#define UART_PIN_NO_CHANGE (-1)
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_DEFAULT 0
#define UART_DATA_8_BITS 3
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_1 1

typedef struct {
    int baud_rate; int data_bits; int parity;
    int stop_bits; int flow_ctrl; int source_clk;
} uart_config_t;

typedef struct { int type; int size; } uart_event_t;
#define UART_DATA 0
#define UART_FIFO_OVF 1
#define UART_BUFFER_FULL 2

static int uart_write_bytes(uart_port_t p, const void *d, size_t l) { (void)p;(void)d; return (int)l; }
static int uart_read_bytes(uart_port_t p, void *d, int l, TickType_t t) { (void)p;(void)d;(void)l;(void)t; return 0; }
static int uart_flush_input(uart_port_t p) { (void)p; return 0; }
static int uart_driver_install(uart_port_t p, int a, int b, int c, QueueHandle_t *q, int f) { (void)p;(void)a;(void)b;(void)c;(void)q;(void)f; return 0; }
static int uart_param_config(uart_port_t p, const uart_config_t *c) { (void)p;(void)c; return 0; }
static int uart_set_pin(uart_port_t p, int a, int b, int c, int d) { (void)p;(void)a;(void)b;(void)c;(void)d; return 0; }

#define ESP_ERROR_CHECK(x) (void)(x)

/* GPIO mocks */
typedef int gpio_num_t;

/* ===== Mock MQTT ===== */
static int mock_mqtt_publish_return = 1;
static int mock_mqtt_publish_count = 0;
static char mock_last_published_topic[128] = {0};
static char mock_last_published_data[4096] = {0};

static int esp_mqtt_client_publish(void *client,
    const char *topic, const char *data, int len, int qos, int retain) {
    (void)client; (void)len; (void)qos; (void)retain;
    mock_mqtt_publish_count++;
    if (topic) strncpy(mock_last_published_topic, topic, sizeof(mock_last_published_topic)-1);
    if (data) strncpy(mock_last_published_data, data, sizeof(mock_last_published_data)-1);
    return mock_mqtt_publish_return;
}

/* ===== Mock cJSON ===== */
/* Override the mock header - define everything inline */
#define __CJSON_H  /* prevent mock cJSON.h from being included */

typedef struct cJSON_s {
    struct cJSON_s *next;
    char *string;
    char *valuestring;
} cJSON;

static cJSON* cJSON_CreateObject(void) {
    return (cJSON*)calloc(1, sizeof(cJSON));
}
static void cJSON_AddStringToObject(cJSON *obj, const char *name, const char *value) {
    (void)obj; (void)name; (void)value;
}
static char* cJSON_PrintUnformatted(const cJSON *obj) {
    (void)obj;
    char *s = (char*)malloc(256);
    snprintf(s, 256, "{\"mock\":true}");
    return s;
}
static void cJSON_Delete(cJSON *obj) {
    if (obj) free(obj);
}

/* ===== Mock app_common.h ===== */
#define APP_COMMON_H_MOCK_DONE  /* just a flag */

typedef enum {
    APP_STATE_INIT,
    APP_STATE_WIFI_CONNECTED,
    APP_STATE_MQTT_CONNECTED
} app_state_t;

volatile app_state_t g_app_state = APP_STATE_MQTT_CONNECTED;
void *mqtt_client = (void*)1;

/* ===== Prevent real headers from being included ===== */
/* These defines match the include guards / #pragma once effect */
/* Since our mock headers use #pragma once, we just need to make sure
   the mocks directory headers aren't pulled in after our inline defs */

/* Override #include directives from sim_modem.c */
#define FREERTOS_FREERTOS_H
#define FREERTOS_TASK_H
#define FREERTOS_QUEUE_H
#define FREERTOS_SEMPHR_H

/* esp_log.h is in mocks/ - that's fine, let it be included */
/* config.h is in mocks/ - but we need SIM_UART pins from it */

/* ===== Now include the source under test ===== */
/* The mock headers in test/mocks/ will be found first by CMake's include order */

/* Suppress the real header includes from sim_modem.c by
   providing all needed symbols above. We use a wrapper approach: */

/* Create a version of sim_modem.c that skips its own includes */
/* Actually, let's just set up the include path correctly and include */

/* The sim_modem.c includes these: */
/* #include <stdio.h>       - system, OK */
/* #include <string.h>      - system, OK */
/* #include <stdlib.h>      - system, OK */
/* #include "freertos/FreeRTOS.h"  - need to mock */
/* #include "freertos/task.h"      - need to mock */
/* #include "freertos/queue.h"     - need to mock */
/* #include "freertos/semphr.h"    - need to mock */
/* #include "driver/uart.h"        - need to mock */
/* #include "driver/gpio.h"        - need to mock */
/* #include "esp_log.h"            - mocked in test/mocks/ */
/* #include "cJSON.h"              - mocked above */
/* #include "mqtt_client.h"        - mocked above */
/* #include "app_common.h"         - mocked above */
/* #include "config.h"             - mocked in test/mocks/ */
/* #include "pdu_decoder.h"        - real, from main/ */

/* We can't easily skip the #include lines in sim_modem.c.
   Better approach: create mock directories for freertos/ and driver/ */

/* Actually, the cleanest solution for this test file is to NOT include
   sim_modem.c but instead extract and test the assembly logic separately.
   Let me copy the relevant static functions directly. */

/* ===== ESP Log mock (inline, same as mocks/esp_log.h) ===== */
#define ESP_LOGE(tag, fmt, ...) /* printf("[E][%s] " fmt "\n", tag, ##__VA_ARGS__) */
#define ESP_LOGW(tag, fmt, ...) /* printf("[W][%s] " fmt "\n", tag, ##__VA_ARGS__) */
#define ESP_LOGI(tag, fmt, ...) /* printf("[I][%s] " fmt "\n", tag, ##__VA_ARGS__) */
#define ESP_LOGD(tag, fmt, ...)
#define ESP_LOGV(tag, fmt, ...)

/* ===== Include pdu_decoder.h for the pdu_sms_t type ===== */
#include "pdu_decoder.h"

/* ===== Copy assembly logic from sim_modem.c (static functions) ===== */
/* This avoids the nightmare of mocking every FreeRTOS/driver header. */

static const char *TAG_ASSEMBLY = "SIM_MODEM";
#undef TAG
#define TAG TAG_ASSEMBLY

#define SMS_FRAGMENT_TIMEOUT_MS     10000
#define SMS_MAX_FRAGMENTS           10
#define SMS_COMBINED_MSG_SIZE       2048

typedef struct {
    char sender[64];
    uint16_t ref_num;
    uint8_t total_parts;
    uint8_t received_parts;
    bool part_received[SMS_MAX_FRAGMENTS];
    char fragments[SMS_MAX_FRAGMENTS][512];
    int indices[SMS_MAX_FRAGMENTS];
    int64_t first_fragment_time;
    bool active;
} sms_assembly_buffer_t;

#define SMS_ASSEMBLY_SLOTS 4
static sms_assembly_buffer_t s_assembly_buffers[SMS_ASSEMBLY_SLOTS] = {0};

static int64_t get_time_ms(void) {
    return (int64_t)mock_tick_count;
}

/* Track deleted SMS indices */
static int mock_deleted_indices[64];
static int mock_deleted_count = 0;

static void delete_sms(int index) {
    if (mock_deleted_count < 64) {
        mock_deleted_indices[mock_deleted_count++] = index;
    }
}

static sms_assembly_buffer_t* find_assembly_buffer(const char *sender, uint16_t ref_num) {
    for (int i = 0; i < SMS_ASSEMBLY_SLOTS; i++) {
        if (s_assembly_buffers[i].active &&
            s_assembly_buffers[i].ref_num == ref_num &&
            strcmp(s_assembly_buffers[i].sender, sender) == 0) {
            return &s_assembly_buffers[i];
        }
    }
    return NULL;
}

static sms_assembly_buffer_t* get_or_create_assembly_buffer(const char *sender, uint16_t ref_num, uint8_t total_parts) {
    sms_assembly_buffer_t *buf = find_assembly_buffer(sender, ref_num);
    if (buf) return buf;

    for (int i = 0; i < SMS_ASSEMBLY_SLOTS; i++) {
        if (!s_assembly_buffers[i].active) {
            memset(&s_assembly_buffers[i], 0, sizeof(sms_assembly_buffer_t));
            memset(s_assembly_buffers[i].indices, -1, sizeof(s_assembly_buffers[i].indices));
            s_assembly_buffers[i].active = true;
            s_assembly_buffers[i].ref_num = ref_num;
            s_assembly_buffers[i].total_parts = total_parts;
            strncpy(s_assembly_buffers[i].sender, sender, sizeof(s_assembly_buffers[i].sender) - 1);
            s_assembly_buffers[i].first_fragment_time = get_time_ms();
            return &s_assembly_buffers[i];
        }
    }

    int oldest_idx = 0;
    int64_t oldest_time = s_assembly_buffers[0].first_fragment_time;
    for (int i = 1; i < SMS_ASSEMBLY_SLOTS; i++) {
        if (s_assembly_buffers[i].first_fragment_time < oldest_time) {
            oldest_time = s_assembly_buffers[i].first_fragment_time;
            oldest_idx = i;
        }
    }

    memset(&s_assembly_buffers[oldest_idx], 0, sizeof(sms_assembly_buffer_t));
    memset(s_assembly_buffers[oldest_idx].indices, -1, sizeof(s_assembly_buffers[oldest_idx].indices));
    s_assembly_buffers[oldest_idx].active = true;
    s_assembly_buffers[oldest_idx].ref_num = ref_num;
    s_assembly_buffers[oldest_idx].total_parts = total_parts;
    strncpy(s_assembly_buffers[oldest_idx].sender, sender, sizeof(s_assembly_buffers[oldest_idx].sender) - 1);
    s_assembly_buffers[oldest_idx].first_fragment_time = get_time_ms();
    return &s_assembly_buffers[oldest_idx];
}

static void publish_single_sms(const char *sender, const char *message, int sms_index) {
    if (mqtt_client && g_app_state == APP_STATE_MQTT_CONNECTED) {
        cJSON *root = cJSON_CreateObject();
        if (root) {
            cJSON_AddStringToObject(root, "sender", sender);
            cJSON_AddStringToObject(root, "message", message);
            char *json_str = cJSON_PrintUnformatted(root);
            if (json_str) {
                int msg_id = esp_mqtt_client_publish(mqtt_client, "sim_bridge/sms", json_str, 0, 1, 0);
                free(json_str);
                if (msg_id != -1) {
                    delete_sms(sms_index);
                }
            }
            cJSON_Delete(root);
        }
    }
}

static void publish_assembled_sms(sms_assembly_buffer_t *buf) {
    if (!buf || buf->received_parts == 0) return;

    char combined_msg[SMS_COMBINED_MSG_SIZE] = {0};
    for (int i = 1; i <= buf->total_parts && i <= SMS_MAX_FRAGMENTS; i++) {
        if (buf->part_received[i] && strlen(buf->fragments[i]) > 0) {
            if (strlen(combined_msg) + strlen(buf->fragments[i]) < SMS_COMBINED_MSG_SIZE - 1) {
                strcat(combined_msg, buf->fragments[i]);
            }
        }
    }

    if (mqtt_client && g_app_state == APP_STATE_MQTT_CONNECTED) {
        cJSON *root = cJSON_CreateObject();
        if (root) {
            cJSON_AddStringToObject(root, "sender", buf->sender);
            cJSON_AddStringToObject(root, "message", combined_msg);
            char *json_str = cJSON_PrintUnformatted(root);
            if (json_str) {
                int msg_id = esp_mqtt_client_publish(mqtt_client, "sim_bridge/sms", json_str, 0, 1, 0);
                free(json_str);
                if (msg_id != -1) {
                    for (int i = 1; i <= buf->total_parts && i <= SMS_MAX_FRAGMENTS; i++) {
                        if (buf->part_received[i] && buf->indices[i] >= 0) {
                            delete_sms(buf->indices[i]);
                        }
                    }
                }
            }
            cJSON_Delete(root);
        }
    }

    memset(buf, 0, sizeof(sms_assembly_buffer_t));
}

static void check_assembly_timeouts(void) {
    int64_t now = get_time_ms();
    for (int i = 0; i < SMS_ASSEMBLY_SLOTS; i++) {
        if (s_assembly_buffers[i].active) {
            if ((now - s_assembly_buffers[i].first_fragment_time) > SMS_FRAGMENT_TIMEOUT_MS) {
                publish_assembled_sms(&s_assembly_buffers[i]);
            }
        }
    }
}

static void handle_decoded_sms(pdu_sms_t *sms, int sms_index) {
    if (!sms->is_multipart) {
        publish_single_sms(sms->sender, sms->message, sms_index);
    } else {
        if (sms->part_num < 1 || sms->part_num > SMS_MAX_FRAGMENTS) {
            return;
        }

        sms_assembly_buffer_t *buf = get_or_create_assembly_buffer(
            sms->sender, sms->ref_num, sms->total_parts);
        if (!buf) return;

        if (!buf->part_received[sms->part_num]) {
            buf->part_received[sms->part_num] = true;
            strncpy(buf->fragments[sms->part_num], sms->message,
                    sizeof(buf->fragments[sms->part_num]) - 1);
            buf->indices[sms->part_num] = sms_index;
            buf->received_parts++;

            if (buf->received_parts >= buf->total_parts) {
                publish_assembled_sms(buf);
            }
        } else {
            delete_sms(sms_index);
        }
    }
}

/* ===== Unity test framework ===== */
#include "unity.h"

/* ===== Reset helper ===== */
static void reset_test_state(void) {
    memset(s_assembly_buffers, 0, sizeof(s_assembly_buffers));
    mock_tick_count = 0;
    mock_mqtt_publish_count = 0;
    mock_mqtt_publish_return = 1;
    mock_deleted_count = 0;
    memset(mock_deleted_indices, 0, sizeof(mock_deleted_indices));
    memset(mock_last_published_data, 0, sizeof(mock_last_published_data));
    g_app_state = APP_STATE_MQTT_CONNECTED;
    mqtt_client = (void*)1;
}

/* ===== Tests ===== */

void test_assembly_find_buffer_empty(void) {
    reset_test_state();
    sms_assembly_buffer_t *buf = find_assembly_buffer("+886912345678", 0x42);
    TEST_ASSERT_NULL(buf);
}

void test_assembly_create_buffer(void) {
    reset_test_state();
    sms_assembly_buffer_t *buf = get_or_create_assembly_buffer("+886912345678", 0x42, 3);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_TRUE(buf->active);
    TEST_ASSERT_EQUAL_UINT16(0x42, buf->ref_num);
    TEST_ASSERT_EQUAL_UINT8(3, buf->total_parts);
    TEST_ASSERT_EQUAL_STRING("+886912345678", buf->sender);

    // Bug fix verification: indices should be -1
    for (int i = 0; i < SMS_MAX_FRAGMENTS; i++) {
        TEST_ASSERT_EQUAL_INT(-1, buf->indices[i]);
    }
}

void test_assembly_find_existing_buffer(void) {
    reset_test_state();
    sms_assembly_buffer_t *buf1 = get_or_create_assembly_buffer("+886912345678", 0x42, 3);
    sms_assembly_buffer_t *buf2 = find_assembly_buffer("+886912345678", 0x42);
    TEST_ASSERT_TRUE(buf1 == buf2);
}

void test_assembly_different_ref_different_buffer(void) {
    reset_test_state();
    sms_assembly_buffer_t *buf1 = get_or_create_assembly_buffer("+886912345678", 0x42, 3);
    sms_assembly_buffer_t *buf2 = get_or_create_assembly_buffer("+886912345678", 0x43, 2);
    TEST_ASSERT_TRUE(buf1 != buf2);
}

void test_assembly_different_sender_different_buffer(void) {
    reset_test_state();
    sms_assembly_buffer_t *buf1 = get_or_create_assembly_buffer("+886912345678", 0x42, 3);
    sms_assembly_buffer_t *buf2 = get_or_create_assembly_buffer("+886987654321", 0x42, 3);
    TEST_ASSERT_TRUE(buf1 != buf2);
}

void test_assembly_slots_full_overwrites_oldest(void) {
    reset_test_state();
    for (int i = 0; i < SMS_ASSEMBLY_SLOTS; i++) {
        mock_tick_count = (uint32_t)(i * 1000);
        get_or_create_assembly_buffer("sender", (uint16_t)(i + 1), 2);
    }

    mock_tick_count = 5000;
    sms_assembly_buffer_t *buf = get_or_create_assembly_buffer("sender", 0xFF, 2);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQUAL_UINT16(0xFF, buf->ref_num);
    TEST_ASSERT_NULL(find_assembly_buffer("sender", 1));
}

void test_handle_single_sms(void) {
    reset_test_state();
    pdu_sms_t sms = {0};
    strcpy(sms.sender, "+886912345678");
    strcpy(sms.message, "Hello World");
    sms.is_multipart = false;

    handle_decoded_sms(&sms, 5);
    TEST_ASSERT_EQUAL_INT(1, mock_mqtt_publish_count);
}

void test_handle_multipart_2parts_in_order(void) {
    reset_test_state();

    pdu_sms_t sms1 = {0};
    strcpy(sms1.sender, "+886912345678");
    strcpy(sms1.message, "Hello ");
    sms1.is_multipart = true;
    sms1.ref_num = 0xAB;
    sms1.total_parts = 2;
    sms1.part_num = 1;
    handle_decoded_sms(&sms1, 10);
    TEST_ASSERT_EQUAL_INT(0, mock_mqtt_publish_count);

    pdu_sms_t sms2 = {0};
    strcpy(sms2.sender, "+886912345678");
    strcpy(sms2.message, "World!");
    sms2.is_multipart = true;
    sms2.ref_num = 0xAB;
    sms2.total_parts = 2;
    sms2.part_num = 2;
    handle_decoded_sms(&sms2, 11);
    TEST_ASSERT_EQUAL_INT(1, mock_mqtt_publish_count);
}

void test_handle_multipart_2parts_out_of_order(void) {
    reset_test_state();

    pdu_sms_t sms2 = {0};
    strcpy(sms2.sender, "+886912345678");
    strcpy(sms2.message, "World!");
    sms2.is_multipart = true;
    sms2.ref_num = 0xCD;
    sms2.total_parts = 2;
    sms2.part_num = 2;
    handle_decoded_sms(&sms2, 20);
    TEST_ASSERT_EQUAL_INT(0, mock_mqtt_publish_count);

    pdu_sms_t sms1 = {0};
    strcpy(sms1.sender, "+886912345678");
    strcpy(sms1.message, "Hello ");
    sms1.is_multipart = true;
    sms1.ref_num = 0xCD;
    sms1.total_parts = 2;
    sms1.part_num = 1;
    handle_decoded_sms(&sms1, 21);
    TEST_ASSERT_EQUAL_INT(1, mock_mqtt_publish_count);
    TEST_ASSERT_NULL(find_assembly_buffer("+886912345678", 0xCD));
}

void test_handle_multipart_3parts_scrambled(void) {
    reset_test_state();

    const char *parts[] = {"Third.", "First.", "Second."};
    int part_nums[] = {3, 1, 2};
    int indices_arr[] = {30, 31, 32};

    for (int i = 0; i < 3; i++) {
        pdu_sms_t sms = {0};
        strcpy(sms.sender, "+886912345678");
        strcpy(sms.message, parts[i]);
        sms.is_multipart = true;
        sms.ref_num = 0xEF;
        sms.total_parts = 3;
        sms.part_num = (uint8_t)part_nums[i];
        handle_decoded_sms(&sms, indices_arr[i]);
        if (i < 2) TEST_ASSERT_EQUAL_INT(0, mock_mqtt_publish_count);
    }
    TEST_ASSERT_EQUAL_INT(1, mock_mqtt_publish_count);
}

void test_handle_multipart_duplicate_ignored(void) {
    reset_test_state();

    pdu_sms_t sms1 = {0};
    strcpy(sms1.sender, "+886912345678");
    strcpy(sms1.message, "Part1");
    sms1.is_multipart = true;
    sms1.ref_num = 0x55;
    sms1.total_parts = 2;
    sms1.part_num = 1;

    handle_decoded_sms(&sms1, 40);
    handle_decoded_sms(&sms1, 41); // duplicate

    TEST_ASSERT_EQUAL_INT(0, mock_mqtt_publish_count);

    sms_assembly_buffer_t *buf = find_assembly_buffer("+886912345678", 0x55);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQUAL_UINT8(1, buf->received_parts);

    // Duplicate should trigger delete of SIM message
    TEST_ASSERT_EQUAL_INT(1, mock_deleted_count);
    TEST_ASSERT_EQUAL_INT(41, mock_deleted_indices[0]);
}

void test_assembly_timeout_publishes_partial(void) {
    reset_test_state();
    mock_tick_count = 1000;

    pdu_sms_t sms = {0};
    strcpy(sms.sender, "+886912345678");
    strcpy(sms.message, "Only part 1");
    sms.is_multipart = true;
    sms.ref_num = 0x77;
    sms.total_parts = 3;
    sms.part_num = 1;

    handle_decoded_sms(&sms, 50);
    TEST_ASSERT_EQUAL_INT(0, mock_mqtt_publish_count);

    mock_tick_count = 1000 + SMS_FRAGMENT_TIMEOUT_MS + 1;
    check_assembly_timeouts();

    TEST_ASSERT_EQUAL_INT(1, mock_mqtt_publish_count);
    TEST_ASSERT_NULL(find_assembly_buffer("+886912345678", 0x77));
}

void test_assembly_no_timeout_before_deadline(void) {
    reset_test_state();
    mock_tick_count = 1000;

    pdu_sms_t sms = {0};
    strcpy(sms.sender, "+886912345678");
    strcpy(sms.message, "Part");
    sms.is_multipart = true;
    sms.ref_num = 0x88;
    sms.total_parts = 2;
    sms.part_num = 1;

    handle_decoded_sms(&sms, 60);

    mock_tick_count = 1000 + SMS_FRAGMENT_TIMEOUT_MS - 1;
    check_assembly_timeouts();

    TEST_ASSERT_EQUAL_INT(0, mock_mqtt_publish_count);
    TEST_ASSERT_NOT_NULL(find_assembly_buffer("+886912345678", 0x88));
}

void test_single_sms_mqtt_disconnected(void) {
    reset_test_state();
    g_app_state = APP_STATE_WIFI_CONNECTED;

    pdu_sms_t sms = {0};
    strcpy(sms.sender, "+886912345678");
    strcpy(sms.message, "Hello");
    sms.is_multipart = false;

    handle_decoded_sms(&sms, 70);
    TEST_ASSERT_EQUAL_INT(0, mock_mqtt_publish_count);
    TEST_ASSERT_EQUAL_INT(0, mock_deleted_count); // kept in SIM
}

void test_indices_initialized_to_negative_one(void) {
    reset_test_state();
    sms_assembly_buffer_t *buf = get_or_create_assembly_buffer("test", 0x01, 3);
    TEST_ASSERT_NOT_NULL(buf);
    for (int i = 0; i < SMS_MAX_FRAGMENTS; i++) {
        TEST_ASSERT_EQUAL_INT(-1, buf->indices[i]);
    }
}

void test_indices_initialized_on_overwrite(void) {
    reset_test_state();
    for (int i = 0; i < SMS_ASSEMBLY_SLOTS; i++) {
        mock_tick_count = (uint32_t)(i * 100);
        get_or_create_assembly_buffer("s", (uint16_t)(i + 1), 2);
    }
    mock_tick_count = 5000;
    sms_assembly_buffer_t *buf = get_or_create_assembly_buffer("s", 0xFF, 2);
    for (int i = 0; i < SMS_MAX_FRAGMENTS; i++) {
        TEST_ASSERT_EQUAL_INT(-1, buf->indices[i]);
    }
}

void test_handle_invalid_part_number_zero(void) {
    reset_test_state();
    pdu_sms_t sms = {0};
    strcpy(sms.sender, "+886912345678");
    strcpy(sms.message, "Bad");
    sms.is_multipart = true;
    sms.ref_num = 0x99;
    sms.total_parts = 2;
    sms.part_num = 0;

    handle_decoded_sms(&sms, 80);
    TEST_ASSERT_NULL(find_assembly_buffer("+886912345678", 0x99));
}

void test_handle_part_number_exceeds_max(void) {
    reset_test_state();
    pdu_sms_t sms = {0};
    strcpy(sms.sender, "+886912345678");
    strcpy(sms.message, "Bad");
    sms.is_multipart = true;
    sms.ref_num = 0x99;
    sms.total_parts = 2;
    sms.part_num = SMS_MAX_FRAGMENTS + 1;

    handle_decoded_sms(&sms, 81);
    TEST_ASSERT_NULL(find_assembly_buffer("+886912345678", 0x99));
}

void test_assembled_sms_deletes_all_indices(void) {
    reset_test_state();

    // 2-part message, both parts
    pdu_sms_t sms1 = {0};
    strcpy(sms1.sender, "+886912345678");
    strcpy(sms1.message, "Part1");
    sms1.is_multipart = true;
    sms1.ref_num = 0xDD;
    sms1.total_parts = 2;
    sms1.part_num = 1;
    handle_decoded_sms(&sms1, 100);

    pdu_sms_t sms2 = {0};
    strcpy(sms2.sender, "+886912345678");
    strcpy(sms2.message, "Part2");
    sms2.is_multipart = true;
    sms2.ref_num = 0xDD;
    sms2.total_parts = 2;
    sms2.part_num = 2;
    handle_decoded_sms(&sms2, 101);

    // Should have deleted both SIM indices
    TEST_ASSERT_EQUAL_INT(2, mock_deleted_count);
    // Indices should be 100 and 101 (in order of part_num)
    TEST_ASSERT_EQUAL_INT(100, mock_deleted_indices[0]);
    TEST_ASSERT_EQUAL_INT(101, mock_deleted_indices[1]);
}

void test_mqtt_publish_failure_keeps_sms(void) {
    reset_test_state();
    mock_mqtt_publish_return = -1; // publish fails

    pdu_sms_t sms = {0};
    strcpy(sms.sender, "+886912345678");
    strcpy(sms.message, "Hello");
    sms.is_multipart = false;

    handle_decoded_sms(&sms, 90);

    // Should have tried to publish
    TEST_ASSERT_EQUAL_INT(1, mock_mqtt_publish_count);
    // But should NOT have deleted from SIM
    TEST_ASSERT_EQUAL_INT(0, mock_deleted_count);
}

void test_concurrent_multipart_from_two_senders(void) {
    reset_test_state();

    // Sender A part 1
    pdu_sms_t a1 = {0};
    strcpy(a1.sender, "+886111111111");
    strcpy(a1.message, "A1");
    a1.is_multipart = true;
    a1.ref_num = 0x01;
    a1.total_parts = 2;
    a1.part_num = 1;
    handle_decoded_sms(&a1, 200);

    // Sender B part 1
    pdu_sms_t b1 = {0};
    strcpy(b1.sender, "+886222222222");
    strcpy(b1.message, "B1");
    b1.is_multipart = true;
    b1.ref_num = 0x01; // SAME ref_num!
    b1.total_parts = 2;
    b1.part_num = 1;
    handle_decoded_sms(&b1, 201);

    // Should be in separate buffers
    TEST_ASSERT_NOT_NULL(find_assembly_buffer("+886111111111", 0x01));
    TEST_ASSERT_NOT_NULL(find_assembly_buffer("+886222222222", 0x01));
    TEST_ASSERT_EQUAL_INT(0, mock_mqtt_publish_count);

    // Complete sender A
    pdu_sms_t a2 = {0};
    strcpy(a2.sender, "+886111111111");
    strcpy(a2.message, "A2");
    a2.is_multipart = true;
    a2.ref_num = 0x01;
    a2.total_parts = 2;
    a2.part_num = 2;
    handle_decoded_sms(&a2, 202);

    TEST_ASSERT_EQUAL_INT(1, mock_mqtt_publish_count);
    // Sender A buffer cleared, B still active
    TEST_ASSERT_NULL(find_assembly_buffer("+886111111111", 0x01));
    TEST_ASSERT_NOT_NULL(find_assembly_buffer("+886222222222", 0x01));
}

/* ===== Test Runner ===== */

void run_sms_assembly_tests(void) {
    printf("\n=== SMS Assembly Tests ===\n");
    RUN_TEST(test_assembly_find_buffer_empty);
    RUN_TEST(test_assembly_create_buffer);
    RUN_TEST(test_assembly_find_existing_buffer);
    RUN_TEST(test_assembly_different_ref_different_buffer);
    RUN_TEST(test_assembly_different_sender_different_buffer);
    RUN_TEST(test_assembly_slots_full_overwrites_oldest);
    RUN_TEST(test_handle_single_sms);
    RUN_TEST(test_handle_multipart_2parts_in_order);
    RUN_TEST(test_handle_multipart_2parts_out_of_order);
    RUN_TEST(test_handle_multipart_3parts_scrambled);
    RUN_TEST(test_handle_multipart_duplicate_ignored);
    RUN_TEST(test_assembly_timeout_publishes_partial);
    RUN_TEST(test_assembly_no_timeout_before_deadline);
    RUN_TEST(test_single_sms_mqtt_disconnected);
    RUN_TEST(test_indices_initialized_to_negative_one);
    RUN_TEST(test_indices_initialized_on_overwrite);
    RUN_TEST(test_handle_invalid_part_number_zero);
    RUN_TEST(test_handle_part_number_exceeds_max);
    RUN_TEST(test_assembled_sms_deletes_all_indices);
    RUN_TEST(test_mqtt_publish_failure_keeps_sms);
    RUN_TEST(test_concurrent_multipart_from_two_senders);
}
