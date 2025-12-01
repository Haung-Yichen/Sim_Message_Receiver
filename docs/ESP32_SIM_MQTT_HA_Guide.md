# ESP32 讀取 SIM 模組信息並透過 MQTT 傳送至 Home Assistant 指南

這份文件說明如何將 SIM 模組 (如 A7670C) 連接到 ESP32，讀取其狀態信息 (如信號強度、運營商等)，並透過 MQTT 協議發送到部署在 Orange Pi 上的 Home Assistant。

## 1. 系統架構

*   **ESP32**: 負責控制 SIM 模組，讀取數據，並連網發送 MQTT 訊息。
*   **SIM 模組**: 提供行動網路資訊 (透過 UART 與 ESP32 通訊)。
*   **MQTT Broker (Mosquitto)**: 運行於 Orange Pi，作為訊息中介。
*   **Home Assistant**: 運行於 Orange Pi，訂閱 MQTT 主題以顯示資訊。

## 2. 硬體連接

將 SIM 模組透過 UART 連接到 ESP32。

*   **SIM Module TX** -> **ESP32 RX** (例如 GPIO 16 / RX2)
*   **SIM Module RX** -> **ESP32 TX** (例如 GPIO 17 / TX2)
*   **GND** -> **GND** (共地非常重要)
*   **VCC** -> 外部電源 (SIM 模組通常需要較大電流，建議不要直接由 ESP32 供電)

## 3. Orange Pi 端設定 (MQTT Broker)

確保 Orange Pi 上已安裝並運行 MQTT Broker (如 Mosquitto)。

```bash
# 安裝 Mosquitto
sudo apt update
sudo apt install mosquitto mosquitto-clients

# 啟動服務
sudo systemctl enable mosquitto
sudo systemctl start mosquitto
```

## 4. ESP32 程式碼範例 (Arduino IDE)

你需要安裝以下 Library:
1.  `PubSubClient` (by Nick O'Leary) - 用於 MQTT
2.  `TinyGSM` (可選，或直接用 Serial 指令) - 這裡示範直接用 Serial 指令以便理解原理。

```cpp
#include <WiFi.h>
#include <PubSubClient.h>

// --- 設定 ---
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* mqtt_server = "192.168.X.X"; // Orange Pi 的 IP 地址
const int mqtt_port = 1883;

// SIM 模組 Serial (使用 HardwareSerial 2)
#define RXD2 16
#define TXD2 17
#define SIM_BAUD 115200

WiFiClient espClient;
PubSubClient client(espClient);

void setup() {
  Serial.begin(115200);
  Serial2.begin(SIM_BAUD, SERIAL_8N1, RXD2, TXD2);
  
  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
}

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("WiFi connected");
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    if (client.connect("ESP32_SIM_Client")) { // Client ID
      Serial.println("connected");
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      delay(5000);
    }
  }
}

String sendATCommand(String cmd, int timeout) {
  String response = "";
  Serial2.println(cmd);
  long int time = millis();
  while ((time + timeout) > millis()) {
    while (Serial2.available()) {
      char c = Serial2.read();
      response += c;
    }
  }
  return response;
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  // 每 10 秒讀取一次信息
  static unsigned long lastMsg = 0;
  unsigned long now = millis();
  if (now - lastMsg > 10000) {
    lastMsg = now;

    // 1. 讀取信號強度 (CSQ)
    // 回傳範例: +CSQ: 20,99
    String csqResponse = sendATCommand("AT+CSQ", 1000);
    
    // 簡單解析 (實際應用建議用更嚴謹的字串處理)
    int csqIndex = csqResponse.indexOf("+CSQ: ");
    if (csqIndex != -1) {
      String csqVal = csqResponse.substring(csqIndex + 6, csqResponse.indexOf(",", csqIndex));
      client.publish("home/sim_module/signal", csqVal.c_str());
    }

    // 2. 讀取運營商 (COPS)
    String copsResponse = sendATCommand("AT+COPS?", 1000);
    client.publish("home/sim_module/raw_cops", copsResponse.c_str());
    
    Serial.println("Data published to MQTT");
  }
}
```

## 5. Home Assistant 設定

在 Home Assistant 中，你可以透過 MQTT Integration 自動發現 (如果 ESP32 發送 Discovery 訊息)，或手動在 `configuration.yaml` 中添加 Sensor。

**方法：修改 configuration.yaml**

```yaml
mqtt:
  sensor:
    - name: "SIM Signal Strength"
      state_topic: "home/sim_module/signal"
      unit_of_measurement: "asu" # CSQ 值通常是 ASU
      icon: "mdi:signal"
      
    - name: "SIM Operator Raw"
      state_topic: "home/sim_module/raw_cops"
```

## 6. 測試流程

1.  將程式碼燒錄至 ESP32。
2.  打開 Serial Monitor 觀察 Log。
3.  使用 MQTT Explorer (電腦端軟體) 或在 Orange Pi 上使用 `mosquitto_sub -h localhost -t "home/sim_module/#" -v` 監聽訊息。
4.  確認 Home Assistant 的 Developer Tools -> States 中是否出現 `sensor.sim_signal_strength`。
