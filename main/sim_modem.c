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
#define SMS_FRAGMENT_TIMEOUT_MS     30000   // 片段逾時 30 秒 (給更多時間等所有分段)
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

// --- 已處理索引追蹤 (防止重複處理) ---
#define PROCESSED_RING_SIZE 32
static int s_processed_ring[PROCESSED_RING_SIZE];
static int s_processed_ring_count = 0;

static bool is_index_processed(int index) {
    for (int i = 0; i < s_processed_ring_count; i++) {
        if (s_processed_ring[i] == index) return true;
    }
    return false;
}

static void mark_index_processed(int index) {
    if (s_processed_ring_count < PROCESSED_RING_SIZE) {
        s_processed_ring[s_processed_ring_count++] = index;
    } else {
        // Ring buffer 滿了，shift 掉最舊的
        memmove(s_processed_ring, s_processed_ring + 1, 
                (PROCESSED_RING_SIZE - 1) * sizeof(int));
        s_processed_ring[PROCESSED_RING_SIZE - 1] = index;
    }
}

static void clear_processed_ring(void) {
    s_processed_ring_count = 0;
}

// --- Flush 狀態控制 ---
static int64_t s_last_flush_time = 0;
#define FLUSH_COOLDOWN_MS 3000  // flush 之間最少間隔 3 秒

// --- 延遲刪除佇列 ---
#define DELETE_QUEUE_SIZE 16
static int s_delete_queue[DELETE_QUEUE_SIZE];
static int s_delete_queue_count = 0;

static void queue_delete_sms(int index) {
    if (s_delete_queue_count < DELETE_QUEUE_SIZE) {
        // 避免重複加入
        for (int i = 0; i < s_delete_queue_count; i++) {
            if (s_delete_queue[i] == index) return;
        }
        s_delete_queue[s_delete_queue_count++] = index;
    }
}

static void send_at_command(const char *cmd)
{
    uart_write_bytes(EX_UART_NUM, cmd, strlen(cmd));
    uart_write_bytes(EX_UART_NUM, "\r\n", 2);
    ESP_LOGI(TAG, "Sent: %s", cmd);
}

// 執行延遲刪除（在主循環中呼叫，每次刪一個並等回應）
static void process_delete_queue(void) {
    if (s_delete_queue_count > 0) {
        int index = s_delete_queue[0];
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", index);
        send_at_command(cmd);
        ESP_LOGI(TAG, "Deleted SMS at index %d (%d remaining)", 
                 index, s_delete_queue_count - 1);
        
        // 移除佇列頭
        memmove(s_delete_queue, s_delete_queue + 1, 
                (s_delete_queue_count - 1) * sizeof(int));
        s_delete_queue_count--;
        
        // 標記為已處理，防止未來 CMGL 再讀到
        mark_index_processed(index);
    }
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
                    // 加入延遲刪除佇列 (而非立即刪除)
                    mark_index_processed(sms_index);
                    queue_delete_sms(sms_index);
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
    
    // 使用 static 避免 stack overflow (rx_task stack 有限)
    static char combined_msg[SMS_COMBINED_MSG_SIZE];
    memset(combined_msg, 0, sizeof(combined_msg));
    
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
                    // 標記所有分段為已處理，加入延遲刪除佇列
                    for (int i = 1; i <= buf->total_parts && i <= SMS_MAX_FRAGMENTS; i++) {
                        if (buf->part_received[i] && buf->indices[i] >= 0) {
                            mark_index_processed(buf->indices[i]);
                            queue_delete_sms(buf->indices[i]);
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
            // 標記為已處理並加入刪除佇列
            mark_index_processed(sms_index);
            queue_delete_sms(sms_index);
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
    
    // 解析標頭 — 嘗試多種格式
    if (sscanf(cmgl_ptr, "+CMGL: %d,%d,,%d", &index, &stat, &pdu_len) < 2) {
        // 可能有 alpha 欄位: +CMGL: 0,1,"",25
        if (sscanf(cmgl_ptr, "+CMGL: %d,%d,", &index, &stat) < 2) {
            ESP_LOGW(TAG, "Failed to parse CMGL header");
            return;
        }
    }
    
    // 檢查是否已處理過此索引
    if (is_index_processed(index)) {
        ESP_LOGI(TAG, "Skipping already processed SMS at index %d", index);
        // 仍加入刪除佇列確保從 SIM 移除
        queue_delete_sms(index);
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
    
    // Debounce: 收到 +CMTI 後延遲一段時間再 flush，讓所有分段到齊
    static int64_t cmti_pending_time = 0;  // 0 = 沒有 pending
    static const int CMTI_DEBOUNCE_MS = 2000; // 等 2 秒讓後續分段到達
    
    // 上次處理刪除佇列的時間
    static int64_t last_delete_time = 0;
    static const int DELETE_INTERVAL_MS = 500; // 每 500ms 處理一個刪除
    
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
    
    // 設定訊息儲存位置為 SIM 卡
    send_at_command("AT+CPMS=\"SM\",\"SM\",\"SM\"");
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // *** PDU Mode ***
    send_at_command("AT+CMGF=0");
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Store messages in SIM (SM), notify with +CMTI
    send_at_command("AT+CNMI=2,1,0,0,0"); 
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "SIM Init Done (PDU Mode). Waiting for messages...");
    
    // Initial flush if already connected
    if (g_app_state == APP_STATE_MQTT_CONNECTED) {
        sim_modem_trigger_flush();
    }

    for (;;) {
        int64_t now = get_time_ms();
        
        // 檢查分段簡訊組合逾時
        check_assembly_timeouts();
        
        // 處理延遲刪除佇列（每次只刪一個，避免指令衝突）
        if (s_delete_queue_count > 0 && (now - last_delete_time) >= DELETE_INTERVAL_MS) {
            process_delete_queue();
            last_delete_time = now;
        }
        
        // CMTI debounce: 等待一段時間後再觸發 flush
        // 但如果有刪除佇列未完成，延後 flush (避免 CMGD 和 CMGL 衝突)
        if (cmti_pending_time > 0 && s_delete_queue_count == 0) {
            if ((now - cmti_pending_time) >= CMTI_DEBOUNCE_MS) {
                cmti_pending_time = 0;
                if (g_app_state == APP_STATE_MQTT_CONNECTED) {
                    // 確保 flush cooldown
                    if ((now - s_last_flush_time) >= FLUSH_COOLDOWN_MS) {
                        ESP_LOGI(TAG, "CMTI debounce expired, flushing stored messages...");
                        clear_processed_ring();
                        send_at_command("AT+CMGL=4");
                        s_last_flush_time = now;
                    } else {
                        // cooldown 尚未到，延後
                        cmti_pending_time = now;
                    }
                }
            }
        }
        
        // Check if we need to flush messages (from MQTT connect or external trigger)
        // 同樣需要等刪除佇列清空
        if (xSemaphoreTake(flush_sem, 0) == pdTRUE) {
             if (g_app_state == APP_STATE_MQTT_CONNECTED && s_delete_queue_count == 0) {
                 if ((now - s_last_flush_time) >= FLUSH_COOLDOWN_MS) {
                     ESP_LOGI(TAG, "Flushing stored messages...");
                     clear_processed_ring();
                     send_at_command("AT+CMGL=4");
                     s_last_flush_time = now;
                 } else {
                     // 重新排程
                     xSemaphoreGive(flush_sem);
                 }
             } else if (s_delete_queue_count > 0) {
                 // 刪除佇列未清，重新排程
                 xSemaphoreGive(flush_sem);
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
                            
                            // === 最優先：先處理 +CMGL 回應 (PDU Mode) ===
                            while (1) {
                                char *cmgl_start = strstr(uart_buffer, "+CMGL:");
                                if (!cmgl_start) break;

                                // 找標頭後的換行 (PDU 在下一行)
                                char *header_end = strchr(cmgl_start, '\n');
                                if (!header_end) break;

                                // 找 PDU 結尾
                                char *pdu_start = header_end + 1;
                                char *pdu_end = strstr(pdu_start, "\r\n");
                                
                                if (pdu_end) {
                                    // 安全地 null-terminate：計算 pdu_end+2 是否在 buffer 範圍內
                                    int end_offset = (pdu_end + 2) - uart_buffer;
                                    if (end_offset <= uart_buffer_pos) {
                                        char saved = uart_buffer[end_offset];
                                        uart_buffer[end_offset] = 0;
                                        
                                        parse_pdu_cmgl(cmgl_start);
                                        
                                        uart_buffer[end_offset] = saved;
                                        
                                        // Shift buffer: 消費掉已處理的部分
                                        int remain = uart_buffer_pos - end_offset;
                                        if (remain > 0) {
                                            memmove(uart_buffer, uart_buffer + end_offset, remain);
                                            uart_buffer_pos = remain;
                                            uart_buffer[uart_buffer_pos] = 0;
                                        } else {
                                            uart_buffer_pos = 0;
                                            uart_buffer[0] = 0;
                                        }
                                        continue; // 繼續找更多 +CMGL
                                    }
                                }
                                break; // PDU 還沒完全收到，等下次
                            }
                            
                            // === 處理 +CMTI (新訊息通知) ===
                            while (1) {
                                char *cmti_start = strstr(uart_buffer, "+CMTI:");
                                if (!cmti_start) break;
                                
                                // 找到 +CMTI 行的結尾
                                char *cmti_end = strstr(cmti_start, "\r\n");
                                if (!cmti_end) {
                                    cmti_end = strchr(cmti_start, '\n');
                                    if (!cmti_end) break;
                                }
                                
                                ESP_LOGI(TAG, "New Message Indication received");
                                
                                // 設定 debounce timer (用最後一次 +CMTI 的時間)
                                cmti_pending_time = get_time_ms();
                                
                                // 從 buffer 裡移除這行 +CMTI
                                char *consume_end = cmti_end;
                                if (consume_end[0] == '\r') consume_end++;
                                if (consume_end[0] == '\n') consume_end++;
                                
                                int consumed = consume_end - uart_buffer;
                                int remain = uart_buffer_pos - consumed;
                                if (remain > 0) {
                                    memmove(uart_buffer, uart_buffer + consumed, remain);
                                    uart_buffer_pos = remain;
                                    uart_buffer[uart_buffer_pos] = 0;
                                } else {
                                    uart_buffer_pos = 0;
                                    uart_buffer[0] = 0;
                                }
                            }
                            
                            // === 清理：移除已知的非重要回應 ===
                            while (1) {
                                // 清除開頭的空白和換行
                                while (uart_buffer_pos > 0 && 
                                       (uart_buffer[0] == '\r' || uart_buffer[0] == '\n' || uart_buffer[0] == ' ')) {
                                    memmove(uart_buffer, uart_buffer + 1, uart_buffer_pos - 1);
                                    uart_buffer_pos--;
                                    uart_buffer[uart_buffer_pos] = 0;
                                }
                                
                                char *ok_str = strstr(uart_buffer, "OK\r\n");
                                if (ok_str) {
                                    char *after = ok_str + 4;
                                    int consumed = after - uart_buffer;
                                    int remain = uart_buffer_pos - consumed;
                                    if (remain > 0) {
                                        memmove(uart_buffer, after, remain);
                                        uart_buffer_pos = remain;
                                        uart_buffer[uart_buffer_pos] = 0;
                                    } else {
                                        uart_buffer_pos = 0;
                                        uart_buffer[0] = 0;
                                    }
                                    continue;
                                }
                                
                                char *err_str = strstr(uart_buffer, "ERROR\r\n");
                                if (err_str) {
                                    char *after = err_str + 7;
                                    int consumed = after - uart_buffer;
                                    int remain = uart_buffer_pos - consumed;
                                    if (remain > 0) {
                                        memmove(uart_buffer, after, remain);
                                        uart_buffer_pos = remain;
                                        uart_buffer[uart_buffer_pos] = 0;
                                    } else {
                                        uart_buffer_pos = 0;
                                        uart_buffer[0] = 0;
                                    }
                                    continue;
                                }
                                
                                // 清理 +CPMS 回應等 (AT+CPMS 回應格式: +CPMS: ...)
                                char *cpms_str = strstr(uart_buffer, "+CPMS:");
                                if (cpms_str) {
                                    char *cpms_end = strstr(cpms_str, "\r\n");
                                    if (cpms_end) {
                                        char *after = cpms_end + 2;
                                        int consumed = after - uart_buffer;
                                        int remain = uart_buffer_pos - consumed;
                                        if (remain > 0) {
                                            memmove(uart_buffer, after, remain);
                                            uart_buffer_pos = remain;
                                            uart_buffer[uart_buffer_pos] = 0;
                                        } else {
                                            uart_buffer_pos = 0;
                                            uart_buffer[0] = 0;
                                        }
                                        continue;
                                    }
                                }
                                
                                break;
                            }
                            
                            // 防止 buffer 累積過多
                            if (uart_buffer_pos > 2048) {
                                ESP_LOGW(TAG, "UART buffer overflow, resetting (%d bytes)", uart_buffer_pos);
                                uart_buffer_pos = 0;
                                uart_buffer[0] = 0;
                            }
                        } else {
                            ESP_LOGW(TAG, "UART buffer full, resetting");
                            uart_buffer_pos = 0;
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
    // 增加 stack 到 8192 (publish_assembled_sms 使用 static combined_msg,
    // 但 cJSON + pdu_decode call chain 仍需足夠 stack)
    xTaskCreate(rx_task, "uart_rx_task", 8192, NULL, 5, NULL);
}
