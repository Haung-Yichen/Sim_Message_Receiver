#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "cJSON.h"

static const char *TAG = "SMS_MQTT";

// --- Configuration ---
// TODO: Update these with your actual credentials
#define WIFI_SSID      "69N"
#define WIFI_PASS      "0938177577"
#define MQTT_BROKER_URI "mqtt://192.168.1.44" // Update with your Orange Pi IP

// UART Configuration for SIM Module
#define EX_UART_NUM UART_NUM_2
#define PATTERN_CHR_NUM    (3)
#define BUF_SIZE (1024)
#define RD_BUF_SIZE (BUF_SIZE)
#define TXD_PIN (GPIO_NUM_17)
#define RXD_PIN (GPIO_NUM_16)

static esp_mqtt_client_handle_t mqtt_client = NULL;
static QueueHandle_t uart0_queue;

// --- WiFi Event Handler ---
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "retry to connect to the AP");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static void wifi_init_sta(void)
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
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");
}

// --- MQTT Event Handler ---
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%ld", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        break;
    default:
        break;
    }
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

// --- UART & SMS Handling ---

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

static void parse_and_publish_sms(char *data)
{
    // Check for +CMT (Live SMS) or +CMGL (Stored SMS)
    char *cmt_ptr = strstr(data, "+CMT:");
    char *cmgl_ptr = strstr(data, "+CMGL:");
    
    char *header_start = NULL;
    int sender_quote_index = 0; // Which quoted string contains the sender?

    if (cmt_ptr) {
        header_start = cmt_ptr;
        sender_quote_index = 1; // +CMT: "SENDER",...
    } else if (cmgl_ptr) {
        header_start = cmgl_ptr;
        sender_quote_index = 2; // +CMGL: 1,"STATUS","SENDER",...
    } else {
        return;
    }

    if (header_start) {
        // Find the Nth quoted string for sender
        char *sender_start = header_start;
        for (int i = 0; i < sender_quote_index; i++) {
            sender_start = strchr(sender_start, '"');
            if (!sender_start) return;
            if (i < sender_quote_index - 1) {
                sender_start++; // Move past this quote to find the next start
                sender_start = strchr(sender_start, '"'); // Find end of this quote
                if (!sender_start) return;
                sender_start++; // Move past the end quote
            }
        }

        if (sender_start) {
            char *sender_end = strchr(sender_start + 1, '"');
            if (sender_end) {
                int sender_hex_len = sender_end - sender_start - 1;
                char sender_hex[64] = {0};
                if (sender_hex_len > sizeof(sender_hex) - 1) {
                    sender_hex_len = sizeof(sender_hex) - 1;
                }
                strncpy(sender_hex, sender_start + 1, sender_hex_len);
                
                // Decode Sender (UCS2 Hex -> UTF8)
                char sender[32] = {0};
                ucs2_to_utf8(sender_hex, sender, sizeof(sender));

                // Find message content (after the newline following the header line)
                char *msg_start = strchr(sender_end, '\n'); 
                if (msg_start) {
                    msg_start++; // Skip \n
                    
                    // The message content is everything after the header line until the end of the buffer
                    // However, we need to trim trailing \r\n which are part of the AT command response
                    
                    char msg_hex_buf[1024] = {0};
                    strncpy(msg_hex_buf, msg_start, sizeof(msg_hex_buf) - 1);
                    
                    // Trim trailing CR/LF
                    int len = strlen(msg_hex_buf);
                    while (len > 0 && (msg_hex_buf[len-1] == '\r' || msg_hex_buf[len-1] == '\n')) {
                        msg_hex_buf[len-1] = 0;
                        len--;
                    }
                    
                    // Decode Message (UCS2 Hex -> UTF8)
                    char msg_utf8[1024] = {0};
                    ucs2_to_utf8(msg_hex_buf, msg_utf8, sizeof(msg_utf8));
                    
                    ESP_LOGI(TAG, "SMS From: %s, Msg: %s", sender, msg_utf8);

                    if (mqtt_client) {
                        cJSON *root = cJSON_CreateObject();
                        cJSON_AddStringToObject(root, "sender", sender);
                        cJSON_AddStringToObject(root, "message", msg_utf8);
                        char *json_str = cJSON_PrintUnformatted(root);
                        
                        esp_mqtt_client_publish(mqtt_client, "sim_bridge/sms", json_str, 0, 1, 0);
                        
                        free(json_str);
                        cJSON_Delete(root);
                    }
                }
            }
        }
    }
}

static void rx_task(void *arg)
{
    uart_event_t event;
    uint8_t *dtmp = (uint8_t *)malloc(RD_BUF_SIZE);
    static char uart_buffer[4096] = {0};  // Larger accumulation buffer
    static int uart_buffer_pos = 0;
    
    // Initial Setup for SIM Module
    vTaskDelay(pdMS_TO_TICKS(2000)); // Wait for system to settle

    // 1. Auto-baud / Sync
    int retry = 0;
    while (retry < 20) {
        send_at_command("AT");
        vTaskDelay(pdMS_TO_TICKS(500)); 
        retry++;
    }

    // 2. Basic Configuration
    send_at_command("ATE0"); // Echo off to simplify parsing
    vTaskDelay(pdMS_TO_TICKS(500));
    
    send_at_command("AT+CPIN?"); // Check SIM status
    vTaskDelay(pdMS_TO_TICKS(1000));

    send_at_command("AT+CREG?"); // Check Network Registration
    vTaskDelay(pdMS_TO_TICKS(1000));

    send_at_command("AT+CMGF=1"); // Text Mode
    vTaskDelay(pdMS_TO_TICKS(1000));

    send_at_command("AT+CSCS=\"UCS2\""); // Set Character Set to UCS2 for Chinese support
    vTaskDelay(pdMS_TO_TICKS(1000));

    send_at_command("AT+CNMI=2,2,0,0,0"); // Direct forward to UART
    vTaskDelay(pdMS_TO_TICKS(1000));

    // 3. Read stored messages (received while powered off)
    ESP_LOGI(TAG, "Checking for stored messages...");
    send_at_command("AT+CMGL=\"ALL\"");
    vTaskDelay(pdMS_TO_TICKS(5000)); // Give it time to dump all messages

    // Optional: Delete all read messages to prevent re-sending on next boot
    // send_at_command("AT+CMGD=1,4"); 

    ESP_LOGI(TAG, "SIM Module Initialization Sequence Completed");

    // Clear buffer to remove initialization responses (but keep CMGL output if it arrived late?)
    // Actually, CMGL output might be mixed with init. 
    // Since we wait 5s above, we assume CMGL output is in the buffer or processed.
    // But we haven't entered the loop yet! The buffer is empty because we haven't read anything yet.
    // Wait, uart_read_bytes is inside the loop.
    // The commands above just sent data to TX. The RX data is sitting in the UART hardware FIFO/Driver buffer.
    // We need to enter the loop to process it.
    
    // We should NOT clear the buffer blindly here if we expect CMGL output.
    // However, the previous logic cleared it to remove "OK".
    // Let's remove the blind clear and rely on the parser to skip junk.
    
    uart_buffer_pos = 0;
    uart_buffer[0] = 0;
    uart_flush(EX_UART_NUM);

    for (;;) {
        if (xQueueReceive(uart0_queue, (void *)&event, (TickType_t)portMAX_DELAY)) {
            switch (event.type) {
            case UART_DATA:
                {
                    memset(dtmp, 0, RD_BUF_SIZE);
                    int read_len = uart_read_bytes(EX_UART_NUM, dtmp, event.size, pdMS_TO_TICKS(100));
                    
                    if (read_len > 0) {
                        // Append to accumulation buffer
                        if (uart_buffer_pos + read_len < sizeof(uart_buffer) - 1) {
                            memcpy(uart_buffer + uart_buffer_pos, dtmp, read_len);
                            uart_buffer_pos += read_len;
                            uart_buffer[uart_buffer_pos] = 0;
                            
                            ESP_LOGI(TAG, "[UART RECV]: %d bytes (total: %d)", read_len, uart_buffer_pos);
                            
                            // 立即處理所有完整的消息
                            while (1) {
                                char *cmt_start = strstr(uart_buffer, "+CMT:");
                                char *cmgl_start = strstr(uart_buffer, "+CMGL:");
                                char *msg_header_start = NULL;

                                // Find the earliest occurrence
                                if (cmt_start && cmgl_start) {
                                    msg_header_start = (cmt_start < cmgl_start) ? cmt_start : cmgl_start;
                                } else if (cmt_start) {
                                    msg_header_start = cmt_start;
                                } else if (cmgl_start) {
                                    msg_header_start = cmgl_start;
                                }

                                if (!msg_header_start) {
                                    // 沒有 SMS header，檢查是否有 OK/ERROR 可清除
                                    if (strstr(uart_buffer, "OK") || strstr(uart_buffer, "ERROR")) {
                                        char *ok_end = strstr(uart_buffer, "\r\n");
                                        if (ok_end) {
                                            // 清除 OK/ERROR 回應
                                            int processed_len = (ok_end + 2) - uart_buffer;
                                            int remaining = uart_buffer_pos - processed_len;
                                            if (remaining > 0) {
                                                memmove(uart_buffer, uart_buffer + processed_len, remaining);
                                                uart_buffer_pos = remaining;
                                                uart_buffer[uart_buffer_pos] = 0;
                                            } else {
                                                uart_buffer_pos = 0;
                                                uart_buffer[0] = 0;
                                            }
                                            continue;
                                        }
                                    }
                                    break; // 沒有完整的訊息
                                }

                                // 找到 header 行的結束
                                char *header_end = strchr(msg_header_start, '\n');
                                if (!header_end) {
                                    ESP_LOGD(TAG, "Header incomplete, waiting for more data");
                                    break; // header 不完整，等待更多資料
                                }

                                // 訊息內容從 header 後開始
                                char *msg_start = header_end + 1;
                                
                                // 尋找訊息結束 (下一個 \r\n)
                                char *msg_end = strstr(msg_start, "\r\n");
                                
                                if (msg_end) {
                                    // 找到完整訊息！
                                    ESP_LOGI(TAG, "[COMPLETE SMS DETECTED]");
                                    
                                    // 暫時 null-terminate 以處理此訊息
                                    char saved_char = msg_end[2];
                                    msg_end[2] = 0;
                                    
                                    parse_and_publish_sms(msg_header_start);
                                    
                                    // 恢復並移除已處理的資料
                                    msg_end[2] = saved_char;
                                    int processed_len = (msg_end + 2) - uart_buffer;
                                    int remaining = uart_buffer_pos - processed_len;
                                    
                                    if (remaining > 0) {
                                        // 使用 memmove 將剩餘資料移到最前面
                                        memmove(uart_buffer, uart_buffer + processed_len, remaining);
                                        uart_buffer_pos = remaining;
                                        uart_buffer[uart_buffer_pos] = 0;
                                        ESP_LOGD(TAG, "Buffer shifted, %d bytes remaining", remaining);
                                    } else {
                                        uart_buffer_pos = 0;
                                        uart_buffer[0] = 0;
                                    }
                                    // 繼續檢查緩衝區中是否還有更多訊息
                                    continue;
                                }
                                
                                // 找到 header 但訊息不完整，等待更多資料
                                ESP_LOGD(TAG, "Message incomplete, waiting for more data");
                                break;
                            }
                        } else {
                            ESP_LOGW(TAG, "Buffer overflow, clearing");
                            uart_buffer_pos = 0;
                            uart_buffer[0] = 0;
                        }
                    }
                }
                break;
                
            case UART_FIFO_OVF:
                ESP_LOGW(TAG, "UART FIFO overflow");
                uart_flush_input(EX_UART_NUM);
                xQueueReset(uart0_queue);
                uart_buffer_pos = 0;
                uart_buffer[0] = 0;
                break;
                
            case UART_BUFFER_FULL:
                ESP_LOGW(TAG, "UART ring buffer full");
                uart_flush_input(EX_UART_NUM);
                xQueueReset(uart0_queue);
                uart_buffer_pos = 0;
                uart_buffer[0] = 0;
                break;
                
            case UART_BREAK:
                ESP_LOGD(TAG, "UART break");
                break;
                
            default:
                ESP_LOGD(TAG, "UART event type: %d", event.type);
                break;
            }
        }
    }
    free(dtmp);
    vTaskDelete(NULL);
}

static void uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    // Install UART driver, and get the queue.
    uart_driver_install(EX_UART_NUM, BUF_SIZE * 2, BUF_SIZE * 2, 20, &uart0_queue, 0);
    uart_param_config(EX_UART_NUM, &uart_config);
    uart_set_pin(EX_UART_NUM, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    
    ESP_LOGI(TAG, "UART initialized");
}

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();

    mqtt_app_start();

    uart_init();
    xTaskCreate(rx_task, "uart_rx_task", 4096, NULL, configMAX_PRIORITIES - 1, NULL);
}