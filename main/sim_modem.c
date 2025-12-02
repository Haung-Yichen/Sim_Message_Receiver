#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "cJSON.h"
#include "mqtt_client.h"
#include "app_common.h"
#include "config.h"

static const char *TAG = "SIM_MODEM";

// UART Configuration
#define EX_UART_NUM UART_NUM_2
#define BUF_SIZE (2048)
#define RD_BUF_SIZE (BUF_SIZE)
#define TXD_PIN SIM_UART_TX_PIN
#define RXD_PIN SIM_UART_RX_PIN

static QueueHandle_t uart0_queue;
static SemaphoreHandle_t flush_sem = NULL;

// Helper to convert hex digit to int
static int hex2int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Helper to convert UCS2 Hex string to UTF-8
static void ucs2_to_utf8(const char *hex, char *out, size_t out_size) {
    size_t len = strlen(hex);
    size_t i = 0, j = 0;
    while (i + 3 < len && j + 4 < out_size) {
        int v1 = hex2int(hex[i]);
        int v2 = hex2int(hex[i+1]);
        int v3 = hex2int(hex[i+2]);
        int v4 = hex2int(hex[i+3]);
        
        if (v1 < 0 || v2 < 0 || v3 < 0 || v4 < 0) {
             i++; 
             continue;
        }
        
        uint16_t wc = (v1 << 12) | (v2 << 8) | (v3 << 4) | v4;
        i += 4;
        
        if (wc < 0x80) {
            out[j++] = (char)wc;
        } else if (wc < 0x800) {
            out[j++] = 0xC0 | (wc >> 6);
            out[j++] = 0x80 | (wc & 0x3F);
        } else {
            out[j++] = 0xE0 | (wc >> 12);
            out[j++] = 0x80 | ((wc >> 6) & 0x3F);
            out[j++] = 0x80 | (wc & 0x3F);
        }
    }
    out[j] = '\0';
}

static void send_at_command(const char *cmd)
{
    uart_write_bytes(EX_UART_NUM, cmd, strlen(cmd));
    uart_write_bytes(EX_UART_NUM, "\r\n", 2);
    ESP_LOGI(TAG, "Sent: %s", cmd);
}

static void delete_sms(int index)
{
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", index);
    send_at_command(cmd);
    ESP_LOGI(TAG, "Deleted SMS at index %d", index);
}

static void parse_and_publish_sms(char *data)
{
    // Format: +CMGL: <index>,"STAT","SENDER",...
    // Or +CMGR: "STAT","SENDER",... (But we use CMGL mostly)
    
    char *cmgl_ptr = strstr(data, "+CMGL:");
    if (!cmgl_ptr) return;

    // Extract Index
    int index = -1;
    if (sscanf(cmgl_ptr, "+CMGL: %d", &index) != 1) {
        ESP_LOGW(TAG, "Failed to parse SMS index");
        return;
    }

    // Find Sender (2nd quoted string usually, but let's be robust)
    // +CMGL: 1,"REC READ","+886...",...
    
    char *header_start = cmgl_ptr;
    // Skip to sender
    // Quote 1: Status
    // Quote 2: Sender
    
    char *sender_start = header_start;
    for (int i = 0; i < 2; i++) {
        sender_start = strchr(sender_start, '"');
        if (!sender_start) return;
        if (i < 1) {
            sender_start++; 
            sender_start = strchr(sender_start, '"'); 
            if (!sender_start) return;
            sender_start++; 
        }
    }

    if (sender_start) {
        char *sender_end = strchr(sender_start + 1, '"');
        if (sender_end) {
            int sender_hex_len = sender_end - sender_start - 1;
            char sender_hex[64] = {0};
            if (sender_hex_len > sizeof(sender_hex) - 1) sender_hex_len = sizeof(sender_hex) - 1;
            strncpy(sender_hex, sender_start + 1, sender_hex_len);
            
            char sender[32] = {0};
            ucs2_to_utf8(sender_hex, sender, sizeof(sender));

            // Message content
            char *msg_start = strchr(sender_end, '\n'); 
            if (msg_start) {
                msg_start++; // Skip \n
                
                char msg_hex_buf[1024] = {0};
                strncpy(msg_hex_buf, msg_start, sizeof(msg_hex_buf) - 1);
                
                // Trim trailing CR/LF
                int len = strlen(msg_hex_buf);
                while (len > 0 && (msg_hex_buf[len-1] == '\r' || msg_hex_buf[len-1] == '\n')) {
                    msg_hex_buf[len-1] = 0;
                    len--;
                }
                
                char msg_utf8[1024] = {0};
                ucs2_to_utf8(msg_hex_buf, msg_utf8, sizeof(msg_utf8));
                
                ESP_LOGI(TAG, "SMS [%d] From: %s, Msg: %s", index, sender, msg_utf8);

                if (mqtt_client && g_app_state == APP_STATE_MQTT_CONNECTED) {
                    cJSON *root = cJSON_CreateObject();
                    if (root) {
                        cJSON_AddStringToObject(root, "sender", sender);
                        cJSON_AddStringToObject(root, "message", msg_utf8);
                        char *json_str = cJSON_PrintUnformatted(root);
                        
                        if (json_str) {
                            int msg_id = esp_mqtt_client_publish(mqtt_client, "sim_bridge/sms", json_str, 0, 1, 0);
                            free(json_str);
                            
                            if (msg_id != -1) {
                                // Only delete if successfully queued for publish
                                delete_sms(index);
                            } else {
                                ESP_LOGE(TAG, "Failed to publish SMS, keeping in SIM");
                            }
                        }
                        cJSON_Delete(root);
                    }
                } else {
                    ESP_LOGW(TAG, "MQTT not connected, keeping SMS in SIM");
                }
            }
        }
    }
}

void sim_modem_trigger_flush(void)
{
    if (flush_sem) {
        xSemaphoreGive(flush_sem);
    }
}

static void rx_task(void *arg)
{
    uart_event_t event;
    uint8_t *dtmp = (uint8_t *)malloc(RD_BUF_SIZE);
    static char uart_buffer[4096] = {0};
    static int uart_buffer_pos = 0;
    
    if (!dtmp) {
        vTaskDelete(NULL);
        return;
    }

    flush_sem = xSemaphoreCreateBinary();

    // --- Initialization ---
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Auto-baud
    for(int i=0; i<10; i++) {
        send_at_command("AT");
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    send_at_command("ATE0"); 
    vTaskDelay(pdMS_TO_TICKS(500));
    send_at_command("AT+CPIN?"); 
    vTaskDelay(pdMS_TO_TICKS(1000));
    send_at_command("AT+CMGF=1"); // Text Mode
    vTaskDelay(pdMS_TO_TICKS(1000));
    send_at_command("AT+CSCS=\"UCS2\""); 
    vTaskDelay(pdMS_TO_TICKS(1000));

    // IMPORTANT: Store messages in SIM (SM), notify with +CMTI
    send_at_command("AT+CNMI=2,1,0,0,0"); 
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "SIM Init Done. Waiting for messages...");
    
    // Initial flush if already connected (unlikely on fast boot, but safe)
    if (g_app_state == APP_STATE_MQTT_CONNECTED) {
        sim_modem_trigger_flush();
    }

    for (;;) {
        // Check if we need to flush messages (Triggered by MQTT Connect or +CMTI)
        if (xSemaphoreTake(flush_sem, 0) == pdTRUE) {
             if (g_app_state == APP_STATE_MQTT_CONNECTED) {
                 ESP_LOGI(TAG, "Flushing stored messages...");
                 send_at_command("AT+CMGL=\"ALL\"");
             }
        }

        if (xQueueReceive(uart0_queue, (void *)&event, (TickType_t)100)) { // 100ms timeout to check flush_sem
            switch (event.type) {
            case UART_DATA:
                {
                    memset(dtmp, 0, RD_BUF_SIZE);
                    int read_len = uart_read_bytes(EX_UART_NUM, dtmp, event.size, pdMS_TO_TICKS(100));
                    
                    if (read_len > 0) {
                        if (uart_buffer_pos + read_len < sizeof(uart_buffer) - 1) {
                            memcpy(uart_buffer + uart_buffer_pos, dtmp, read_len);
                            uart_buffer_pos += read_len;
                            uart_buffer[uart_buffer_pos] = 0;
                            
                            // Check for +CMTI (New Message Indication)
                            if (strstr(uart_buffer, "+CMTI:")) {
                                ESP_LOGI(TAG, "New Message Indication received");
                                // Trigger flush to read it
                                sim_modem_trigger_flush();
                                
                                // Clear buffer to avoid re-triggering immediately
                                // (Ideally we parse it out, but flushing ALL is safer/simpler)
                                uart_buffer_pos = 0; 
                                uart_buffer[0] = 0;
                                continue;
                            }

                            // Process +CMGL responses
                            while (1) {
                                char *cmgl_start = strstr(uart_buffer, "+CMGL:");
                                if (!cmgl_start) {
                                    // Cleanup junk
                                    if (uart_buffer_pos > 2048) {
                                        uart_buffer_pos = 0;
                                        uart_buffer[0] = 0;
                                    }
                                    break;
                                }

                                char *header_end = strchr(cmgl_start, '\n');
                                if (!header_end) break;

                                char *msg_start = header_end + 1;
                                char *msg_end = strstr(msg_start, "\r\n"); // End of message body
                                
                                // Note: CMGL might list multiple messages. 
                                // +CMGL: 1,...\r\nContent\r\n\r\n+CMGL: 2,...
                                // We need to be careful finding the true end.
                                // Usually AT command response ends with OK.
                                // But here we look for the next +CMGL or OK?
                                // Simplest: Look for \r\n after message content.
                                
                                if (msg_end) {
                                    // Temporarily terminate
                                    char saved = msg_end[2];
                                    msg_end[2] = 0;
                                    
                                    parse_and_publish_sms(cmgl_start);
                                    
                                    msg_end[2] = saved;
                                    
                                    // Shift buffer
                                    int processed = (msg_end + 2) - uart_buffer;
                                    int remain = uart_buffer_pos - processed;
                                    if (remain > 0) {
                                        memmove(uart_buffer, uart_buffer + processed, remain);
                                        uart_buffer_pos = remain;
                                        uart_buffer[uart_buffer_pos] = 0;
                                    } else {
                                        uart_buffer_pos = 0;
                                        uart_buffer[0] = 0;
                                    }
                                    continue;
                                }
                                break;
                            }
                        } else {
                            uart_buffer_pos = 0; // Overflow reset
                        }
                    }
                }
                break;
            case UART_FIFO_OVF:
            case UART_BUFFER_FULL:
                uart_flush_input(EX_UART_NUM);
                xQueueReset(uart0_queue);
                uart_buffer_pos = 0;
                break;
            default:
                break;
            }
        }
    }
    free(dtmp);
    vTaskDelete(NULL);
}

void sim_modem_init_uart(void)
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    ESP_ERROR_CHECK(uart_driver_install(EX_UART_NUM, BUF_SIZE * 2, BUF_SIZE * 2, 20, &uart0_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(EX_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(EX_UART_NUM, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

void sim_modem_start_task(void)
{
    xTaskCreate(rx_task, "uart_rx_task", 4096, NULL, 5, NULL);
}
