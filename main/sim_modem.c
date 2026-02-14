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
#include "pdu_decoder.h"

static const char *TAG = "SIM_MODEM";

// UART Configuration
#define EX_UART_NUM UART_NUM_2
#define BUF_SIZE (2048)
#define RD_BUF_SIZE (BUF_SIZE)
#define TXD_PIN SIM_UART_TX_PIN
#define RXD_PIN SIM_UART_RX_PIN

static QueueHandle_t uart0_queue;
static SemaphoreHandle_t flush_sem = NULL;

// --- Multipart SMS Assembly (PDU Mode) ---
#define SMS_FRAGMENT_TIMEOUT_MS     10000   // 片段逾時時間 (加長到10秒)
#define SMS_MAX_FRAGMENTS           10      // 每則訊息最大片段數
#define SMS_COMBINED_MSG_SIZE       2048    // 組合後訊息最大長度

// 改進的分段簡訊緩衝結構 (使用 ref_num 正確識別)
typedef struct {
    char sender[64];                        // 發送者
    uint16_t ref_num;                       // 分段參考號碼 (關鍵識別)
    uint8_t total_parts;                    // 預期總片段數
    uint8_t received_parts;                 // 已收到片段數
    bool part_received[SMS_MAX_FRAGMENTS];  // 各片段接收狀態 (1-indexed)
    char fragments[SMS_MAX_FRAGMENTS][512]; // 片段內容 (按 part_num 索引)
    int indices[SMS_MAX_FRAGMENTS];         // 各片段的 SIM 儲存索引 (用於刪除)
    int64_t first_fragment_time;            // 第一個片段的接收時間
    bool active;                            // 此緩衝槽是否使用中
} sms_assembly_buffer_t;

#define SMS_ASSEMBLY_SLOTS 4
static sms_assembly_buffer_t s_assembly_buffers[SMS_ASSEMBLY_SLOTS] = {0};

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

// --- Multipart SMS Assembly Functions ---

static int64_t get_time_ms(void) {
    return (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

// 尋找現有的組合緩衝槽 (使用 sender + ref_num)
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

// 取得或建立組合緩衝槽
static sms_assembly_buffer_t* get_or_create_assembly_buffer(const char *sender, uint16_t ref_num, uint8_t total_parts) {
    sms_assembly_buffer_t *buf = find_assembly_buffer(sender, ref_num);
    if (buf) return buf;
    
    // 尋找空的槽
    for (int i = 0; i < SMS_ASSEMBLY_SLOTS; i++) {
        if (!s_assembly_buffers[i].active) {
            memset(&s_assembly_buffers[i], 0, sizeof(sms_assembly_buffer_t));
            memset(s_assembly_buffers[i].indices, -1, sizeof(s_assembly_buffers[i].indices));
            s_assembly_buffers[i].active = true;
            s_assembly_buffers[i].ref_num = ref_num;
            s_assembly_buffers[i].total_parts = total_parts;
            strncpy(s_assembly_buffers[i].sender, sender, sizeof(s_assembly_buffers[i].sender) - 1);
            s_assembly_buffers[i].first_fragment_time = get_time_ms();
            ESP_LOGI(TAG, "Created assembly buffer for ref=%d, total=%d", ref_num, total_parts);
            return &s_assembly_buffers[i];
        }
    }
    
    // 找不到空槽，覆蓋最舊的
    int oldest_idx = 0;
    int64_t oldest_time = s_assembly_buffers[0].first_fragment_time;
    for (int i = 1; i < SMS_ASSEMBLY_SLOTS; i++) {
        if (s_assembly_buffers[i].first_fragment_time < oldest_time) {
            oldest_time = s_assembly_buffers[i].first_fragment_time;
            oldest_idx = i;
        }
    }
    
    ESP_LOGW(TAG, "Assembly buffer full, overwriting oldest slot");
    memset(&s_assembly_buffers[oldest_idx], 0, sizeof(sms_assembly_buffer_t));
    memset(s_assembly_buffers[oldest_idx].indices, -1, sizeof(s_assembly_buffers[oldest_idx].indices));
    s_assembly_buffers[oldest_idx].active = true;
    s_assembly_buffers[oldest_idx].ref_num = ref_num;
    s_assembly_buffers[oldest_idx].total_parts = total_parts;
    strncpy(s_assembly_buffers[oldest_idx].sender, sender, sizeof(s_assembly_buffers[oldest_idx].sender) - 1);
    s_assembly_buffers[oldest_idx].first_fragment_time = get_time_ms();
    return &s_assembly_buffers[oldest_idx];
}

// 發布單則 SMS (非分段)
static void publish_single_sms(const char *sender, const char *message, int sms_index) {
    ESP_LOGI(TAG, "Publishing single SMS from %s: %s", sender, message);
    
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

// 發布組合後的完整訊息
static void publish_assembled_sms(sms_assembly_buffer_t *buf) {
    if (!buf || buf->received_parts == 0) return;
    
    char combined_msg[SMS_COMBINED_MSG_SIZE] = {0};
    
    // 按正確順序組合所有片段 (part_num 是 1-indexed)
    for (int i = 1; i <= buf->total_parts && i <= SMS_MAX_FRAGMENTS; i++) {
        if (buf->part_received[i] && strlen(buf->fragments[i]) > 0) {
            if (strlen(combined_msg) + strlen(buf->fragments[i]) < SMS_COMBINED_MSG_SIZE - 1) {
                strcat(combined_msg, buf->fragments[i]);
            }
        }
    }
    
    ESP_LOGI(TAG, "Publishing assembled SMS from %s (%d/%d parts): %s", 
             buf->sender, buf->received_parts, buf->total_parts, combined_msg);
    
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
                    // 刪除所有相關的 SMS
                    for (int i = 1; i <= buf->total_parts && i <= SMS_MAX_FRAGMENTS; i++) {
                        if (buf->part_received[i] && buf->indices[i] >= 0) {
                            delete_sms(buf->indices[i]);
                        }
                    }
                } else {
                    ESP_LOGE(TAG, "Failed to publish assembled SMS, keeping in SIM");
                }
            }
            cJSON_Delete(root);
        }
    } else {
        ESP_LOGW(TAG, "MQTT not connected, keeping assembled SMS in SIM");
    }
    
    // 清空緩衝槽
    memset(buf, 0, sizeof(sms_assembly_buffer_t));
}

// 檢查並處理逾時的片段緩衝
static void check_assembly_timeouts(void) {
    int64_t now = get_time_ms();
    for (int i = 0; i < SMS_ASSEMBLY_SLOTS; i++) {
        if (s_assembly_buffers[i].active) {
            if ((now - s_assembly_buffers[i].first_fragment_time) > SMS_FRAGMENT_TIMEOUT_MS) {
                ESP_LOGI(TAG, "Assembly timeout for ref=%d, publishing %d/%d fragments",
                         s_assembly_buffers[i].ref_num,
                         s_assembly_buffers[i].received_parts,
                         s_assembly_buffers[i].total_parts);
                publish_assembled_sms(&s_assembly_buffers[i]);
            }
        }
    }
}

// 處理解碼後的 PDU SMS
static void handle_decoded_sms(pdu_sms_t *sms, int sms_index) {
    if (!sms->is_multipart) {
        // 單則簡訊，直接發布
        publish_single_sms(sms->sender, sms->message, sms_index);
    } else {
        // 分段簡訊，加入組合緩衝
        if (sms->part_num < 1 || sms->part_num > SMS_MAX_FRAGMENTS) {
            ESP_LOGE(TAG, "Invalid part number: %d", sms->part_num);
            return;
        }
        
        sms_assembly_buffer_t *buf = get_or_create_assembly_buffer(
            sms->sender, sms->ref_num, sms->total_parts);
        
        if (!buf) return;
        
        // 存入正確位置 (使用 part_num 作為索引)
        if (!buf->part_received[sms->part_num]) {
            buf->part_received[sms->part_num] = true;
            strncpy(buf->fragments[sms->part_num], sms->message, 
                    sizeof(buf->fragments[sms->part_num]) - 1);
            buf->indices[sms->part_num] = sms_index;
            buf->received_parts++;
            
            ESP_LOGI(TAG, "Stored fragment %d/%d for ref=%d", 
                     sms->part_num, sms->total_parts, sms->ref_num);
            
            // 檢查是否收齊所有片段
            if (buf->received_parts >= buf->total_parts) {
                ESP_LOGI(TAG, "All parts received for ref=%d, assembling", sms->ref_num);
                publish_assembled_sms(buf);
            }
        } else {
            ESP_LOGW(TAG, "Duplicate fragment %d for ref=%d, ignoring", 
                     sms->part_num, sms->ref_num);
            // 仍需刪除重複訊息
            delete_sms(sms_index);
        }
    }
}

// 解析 PDU Mode 的 +CMGL 回應
static void parse_pdu_cmgl(char *data) {
    // PDU Mode 格式: +CMGL: <index>,<stat>,[alpha],<length>\r\n<pdu>\r\n
    char *cmgl_ptr = strstr(data, "+CMGL:");
    if (!cmgl_ptr) return;

    int index = -1;
    int stat = -1;
    int pdu_len = -1;
    
    // 解析標頭
    if (sscanf(cmgl_ptr, "+CMGL: %d,%d,,%d", &index, &stat, &pdu_len) < 2) {
        ESP_LOGW(TAG, "Failed to parse CMGL header");
        return;
    }
    
    // 找 PDU 內容 (在 \r\n 之後)
    char *pdu_start = strchr(cmgl_ptr, '\n');
    if (!pdu_start) return;
    pdu_start++; // Skip \n
    
    // 複製 PDU hex 字串
    char pdu_hex[768] = {0};
    int i = 0;
    while (pdu_start[i] && pdu_start[i] != '\r' && pdu_start[i] != '\n' && i < (int)sizeof(pdu_hex) - 1) {
        pdu_hex[i] = pdu_start[i];
        i++;
    }
    pdu_hex[i] = '\0';
    
    if (strlen(pdu_hex) < 20) {
        ESP_LOGW(TAG, "PDU too short: %s", pdu_hex);
        return;
    }
    
    ESP_LOGI(TAG, "Parsing PDU [%d]: %s", index, pdu_hex);
    
    // 解碼 PDU
    pdu_sms_t sms;
    if (pdu_decode(pdu_hex, &sms)) {
        handle_decoded_sms(&sms, index);
    } else {
        ESP_LOGE(TAG, "Failed to decode PDU at index %d", index);
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
    
    // *** PDU Mode (關鍵變更) ***
    send_at_command("AT+CMGF=0");
    vTaskDelay(pdMS_TO_TICKS(1000));

    // IMPORTANT: Store messages in SIM (SM), notify with +CMTI
    send_at_command("AT+CNMI=2,1,0,0,0"); 
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "SIM Init Done (PDU Mode). Waiting for messages...");
    
    // Initial flush if already connected
    if (g_app_state == APP_STATE_MQTT_CONNECTED) {
        sim_modem_trigger_flush();
    }

    for (;;) {
        // 檢查分段簡訊組合逾時
        check_assembly_timeouts();
        
        // Check if we need to flush messages
        if (xSemaphoreTake(flush_sem, 0) == pdTRUE) {
             if (g_app_state == APP_STATE_MQTT_CONNECTED) {
                 ESP_LOGI(TAG, "Flushing stored messages...");
                 // PDU Mode 使用數字狀態：4 = ALL
                 send_at_command("AT+CMGL=4");
             }
        }


        if (xQueueReceive(uart0_queue, (void *)&event, (TickType_t)100)) {
            switch (event.type) {
            case UART_DATA:
                {
                    memset(dtmp, 0, RD_BUF_SIZE);
                    int read_len = uart_read_bytes(EX_UART_NUM, dtmp, event.size, pdMS_TO_TICKS(100));
                    
                    if (read_len > 0) {
                        if (uart_buffer_pos + read_len < (int)sizeof(uart_buffer) - 1) {
                            memcpy(uart_buffer + uart_buffer_pos, dtmp, read_len);
                            uart_buffer_pos += read_len;
                            uart_buffer[uart_buffer_pos] = 0;
                            
                            // Check for +CMTI (New Message Indication)
                            if (strstr(uart_buffer, "+CMTI:")) {
                                ESP_LOGI(TAG, "New Message Indication received");
                                sim_modem_trigger_flush();
                                uart_buffer_pos = 0; 
                                uart_buffer[0] = 0;
                                continue;
                            }

                            // Process +CMGL responses (PDU Mode)
                            while (1) {
                                char *cmgl_start = strstr(uart_buffer, "+CMGL:");
                                if (!cmgl_start) {
                                    if (uart_buffer_pos > 2048) {
                                        uart_buffer_pos = 0;
                                        uart_buffer[0] = 0;
                                    }
                                    break;
                                }

                                // 找標頭後的換行 (PDU 在下一行)
                                char *header_end = strchr(cmgl_start, '\n');
                                if (!header_end) break;

                                // 找 PDU 結尾
                                char *pdu_start = header_end + 1;
                                char *pdu_end = strstr(pdu_start, "\r\n");
                                
                                if (pdu_end) {
                                    // Temporarily terminate
                                    char saved = pdu_end[2];
                                    pdu_end[2] = 0;
                                    
                                    parse_pdu_cmgl(cmgl_start);
                                    
                                    pdu_end[2] = saved;
                                    
                                    // Shift buffer
                                    int processed = (pdu_end + 2) - uart_buffer;
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
