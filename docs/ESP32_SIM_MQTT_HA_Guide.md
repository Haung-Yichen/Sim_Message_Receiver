# ESP32 SIM 模組 MQTT 簡訊接收器指南

本專案使用 ESP32 與 SIM 模組 (如 A7670C/SIM7600) 透過 UART 通訊，接收 SMS 簡訊 (支援長短信/分段短信)，並透過 MQTT 發送到 Home Assistant 或其他 Broker。

## 1. 系統架構

*   **ESP32 (ESP-IDF)**: 核心控制器。
*   **SIM 模組**: 運行於 PDU 模式，負責接收 SMS。
*   **MQTT Broker**: 接收解碼後的 JSON 格式簡訊。

## 2. 功能特點

*   **PDU 模式解碼**: 完整支援 GSM 03.40 PDU 格式，可正確解析中文 (UCS2) 與英文 (7-bit) 簡訊。
*   **長短信自動重組 (Multipart SMS)**: 支援超過 140 byte 的長短信，自動緩衝並按順序重組後再一次性發送。
*   **可靠性設計**:
    *   **指令佇列**: 避免 AT 指令衝突 (如 `AT+CMGL` 與 `AT+CMGD`)。
    *   **防重複機制**: 透過 Ring Buffer 追蹤已處理的短信索引。
    *   **Debounce**: 防止連續通知造成的頻繁讀取。
*   **MQTT 整合**: 以 JSON 格式發布發送者與內容。

## 3. 硬體連接

*   **SIM TX** -> **ESP32 RX** (GPIO 16 / RXD2 - 可在 config.h 修改)
*   **SIM RX** -> **ESP32 TX** (GPIO 17 / TXD2 - 可在 config.h 修改)
*   **GND** -> **GND** (共地)
*   **VCC** -> 外部獨立電源 (建議 2A 以上)

## 4. 軟體配置 (config.h)

請在 `main/config.h` 中設定：

```c
// WiFi 設定
#define WIFI_SSID "YOUR_SSID"
#define WIFI_PASSWORD "YOUR_PASSWORD"

// MQTT Broker 設定
#define MQTT_BROKER_URI "mqtt://192.168.1.X:1883"

// UART Pin 腳
#define SIM_UART_TX_PIN 17
#define SIM_UART_RX_PIN 16
```

## 5. MQTT 協議

ESP32 連接上 MQTT Broker 後，會發布到以下 Topic：

*   **Topic**: `sim_bridge/sms`
*   **Payload (JSON)**:

```json
{
  "sender": "+886912345678",
  "message": "這是一則測試簡訊"
}
```

## 6. 核心邏輯說明 (main/sim_modem.c)

系統啟動後會執行以下初始化：
1.  **AT+CMGF=0**: 設定為 PDU 模式。
2.  **AT+CPMS="SM","SM","SM"**: 設定短信存儲於 SIM 卡。
3.  **AT+CNMI=2,1,0,0,0**: 設定新短信通知 (收到 `+CMTI` 後觸發讀取)。

### 長短信處理流程
1.  收到 `+CMTI` 通知。
2.  等待 Debounce 時間 (2秒) 以確保所有分段到達。
3.  發送 `AT+CMGL=4` 讀取所有短信。
4.  解析 PDU，檢查 UDH (User Data Header)。
5.  若是分段短信，存入 `s_assembly_buffers`。
6.  當所有分段到齊，組合內容並發布 MQTT。
7.  將原短信索引加入刪除佇列 (`s_delete_queue`)，延遲執行 `AT+CMGD` 以免阻塞 UART。

## 7. 編譯與燒錄

使用 ESP-IDF 環境：

```bash
idf.py build
idf.py -p COMx flash monitor
```
