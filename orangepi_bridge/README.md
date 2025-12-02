# Orange Pi SMS to Telegram Bridge

這是一個 Python 腳本，用於在 Orange Pi 上運行，監聽來自 ESP32 的 MQTT 訊息，並將其轉發到 Telegram Bot。

## ⚠️ 重要安全提醒

**v1.0 已移除所有硬編碼憑證，請務必透過環境變數設定！**

## 🚀 快速開始

### 1. 準備 Telegram Bot

1. 在 Telegram 中搜尋 `@BotFather`
2. 輸入 `/newbot` 創建新機器人
3. 獲取 **API Token** (例如: `123456789:ABCdefGHIjklMNOpqrsTUVwxyz`)
4. 獲取你的 **Chat ID**:
   - 搜尋 `@userinfobot`
   - 發送 `/start` 獲取你的 ID

### 2. 安裝依賴

```bash
# 更新系統
sudo apt update
sudo apt install python3 python3-pip mosquitto mosquitto-clients

# 安裝 Python 套件
cd orangepi_bridge
pip3 install -r requirements.txt
```

### 3. 安裝 MQTT Broker (Mosquitto)

```bash
# 安裝 Mosquitto
sudo apt install mosquitto mosquitto-clients

# 啟用開機自啟
sudo systemctl enable mosquitto
sudo systemctl start mosquitto

# 驗證運行
sudo systemctl status mosquitto
```

### 4. 配置環境變數

```bash
# 方法 1: 臨時設定 (僅當前 Session)
export TELEGRAM_BOT_TOKEN="你的_BOT_TOKEN"
export TELEGRAM_CHAT_IDS="你的_CHAT_ID_1,你的_CHAT_ID_2"

# 方法 2: 永久設定 (推薦)
# 編輯 ~/.bashrc 或 ~/.profile
echo 'export TELEGRAM_BOT_TOKEN="你的_BOT_TOKEN"' >> ~/.bashrc
echo 'export TELEGRAM_CHAT_IDS="你的_CHAT_ID_1,你的_CHAT_ID_2"' >> ~/.bashrc
source ~/.bashrc

# 方法 3: systemd 服務配置 (最安全)
# 見下方 systemd 服務設定
```

### 5. 測試執行

```bash
python3 sms_to_telegram.py
```

**預期輸出**:

```
2025-12-02 17:00:00 - INFO - Starting SMS to Telegram Bridge...
2025-12-02 17:00:01 - INFO - Forwarded SMS from System to Telegram (Chat ID: 你的ID).
2025-12-02 17:00:02 - INFO - Connected to MQTT Broker at localhost
2025-12-02 17:00:02 - INFO - Subscribed to topic: sim_bridge/sms
```

## 🔧 設定為 Systemd 服務 (開機自動啟動)

### 建立服務檔案

```bash
sudo nano /etc/systemd/system/sms-bridge.service
```

### 服務內容

**方法 1: 環境變數內嵌 (簡單)**

```ini
[Unit]
Description=SMS to Telegram Bridge
After=network.target mosquitto.service

[Service]
Type=simple
User=orangepi
WorkingDirectory=/home/orangepi/orangepi_bridge
Environment="TELEGRAM_BOT_TOKEN=你的_BOT_TOKEN"
Environment="TELEGRAM_CHAT_IDS=你的_CHAT_ID_1,你的_CHAT_ID_2"
ExecStart=/usr/bin/python3 /home/orangepi/orangepi_bridge/sms_to_telegram.py
Restart=always
RestartSec=10
Environment=PYTHONUNBUFFERED=1

[Install]
WantedBy=multi-user.target
```

**方法 2: 使用 .env 檔案 (更安全)**

```ini
[Unit]
Description=SMS to Telegram Bridge
After=network.target mosquitto.service

[Service]
Type=simple
User=orangepi
WorkingDirectory=/home/orangepi/orangepi_bridge
EnvironmentFile=/home/orangepi/orangepi_bridge/.env
ExecStart=/usr/bin/python3 /home/orangepi/orangepi_bridge/sms_to_telegram.py
Restart=always
RestartSec=10
Environment=PYTHONUNBUFFERED=1

[Install]
WantedBy=multi-user.target
```

然後建立 `.env` 檔案：

```bash
cd /home/orangepi/orangepi_bridge
nano .env
```

內容：

```
TELEGRAM_BOT_TOKEN=你的_BOT_TOKEN
TELEGRAM_CHAT_IDS=你的_CHAT_ID_1,你的_CHAT_ID_2
```

**設定檔案權限** (重要！):

```bash
chmod 600 .env
```

### 啟用服務

```bash
# 重新載入 systemd
sudo systemctl daemon-reload

# 啟用開機自啟
sudo systemctl enable sms-bridge.service

# 啟動服務
sudo systemctl start sms-bridge.service

# 檢查狀態
sudo systemctl status sms-bridge.service

# 查看即時日誌
journalctl -u sms-bridge.service -f
```

### 服務管理指令

```bash
# 停止服務
sudo systemctl stop sms-bridge.service

# 重啟服務
sudo systemctl restart sms-bridge.service

# 停用開機自啟
sudo systemctl disable sms-bridge.service

# 檢視日誌 (最近 100 行)
journalctl -u sms-bridge.service -n 100
```

## 🔍 測試 MQTT 連線

### 手動訂閱測試

```bash
# 訂閱 MQTT 主題
mosquitto_sub -h localhost -t sim_bridge/sms
```

### 手動發布測試

```bash
# 發布測試訊息
mosquitto_pub -h localhost -t sim_bridge/sms \
  -m '{"sender":"測試號碼","message":"這是測試簡訊"}'
```

你應該在 Telegram 收到訊息。

## 🧪 進階配置

### 自訂 MQTT Broker

如果 Mosquitto 運行在其他伺服器：

```bash
export MQTT_BROKER="192.168.1.100"
export MQTT_PORT=1883
```

### 支援多個 Chat ID

```bash
# 用逗號分隔多個 ID
export TELEGRAM_CHAT_IDS="1234567890,9876543210,5555555555"
```

### 使用 MQTT TLS (加密)

修改 `sms_to_telegram.py`:

```python
import ssl

client.tls_set(ca_certs="/path/to/ca.crt",
               certfile="/path/to/client.crt",
               keyfile="/path/to/client.key",
               cert_reqs=ssl.CERT_REQUIRED)
client.connect(MQTT_BROKER, 8883, 60)  # TLS 預設 port 8883
```

## 🛡️ 安全最佳實踐

1. ✅ **使用非 root 使用者運行服務**
2. ✅ **保護 .env 檔案權限** (`chmod 600`)
3. ✅ **定期更新依賴** (`pip3 install --upgrade -r requirements.txt`)
4. ✅ **啟用 MQTT 認證**
5. ⚠️ **不要將 .env 提交到 Git** (已加入 .gitignore)

## 🐞 故障排除

### 服務無法啟動

```bash
# 檢查詳細錯誤
sudo systemctl status sms-bridge.service

# 檢視完整日誌
journalctl -xe -u sms-bridge.service
```

### MQTT 連線失敗

```bash
# 檢查 Mosquitto 狀態
sudo systemctl status mosquitto

# 檢查 Port 是否開放
sudo netstat -tlnp | grep 1883

# 測試本地連線
mosquitto_sub -h localhost -t test
```

### Telegram 未收到訊息

1. 驗證 Token: 訪問 `https://api.telegram.org/bot你的TOKEN/getMe`
2. 驗證 Chat ID: 發送訊息給 Bot，訪問 `https://api.telegram.org/bot你的TOKEN/getUpdates`
3. 檢查網路: `curl https://api.telegram.org`

### 權限錯誤

```bash
# 確保檔案屬於正確使用者
sudo chown -R orangepi:orangepi /home/orangepi/orangepi_bridge

# 確保檔案可執行
chmod +x sms_to_telegram.py
```

## 📊 監控與日誌

### 查看系統負載

```bash
# CPU 與記憶體使用
top -p $(pgrep -f sms_to_telegram)

# 服務重啟次數
systemctl show sms-bridge.service | grep NRestarts
```

### 日誌輪轉

```bash
# 建議使用 logrotate 管理日誌
sudo nano /etc/logrotate.d/sms-bridge
```

內容：

```
/var/log/sms-bridge.log {
    weekly
    rotate 4
    compress
    missingok
    notifempty
}
```

## 🔗 相關連結

- [Mosquitto 文檔](https://mosquitto.org/documentation/)
- [paho-mqtt 文檔](https://www.eclipse.org/paho/index.php?page=clients/python/docs/index.php)
- [Telegram Bot API](https://core.telegram.org/bots/api)

---

**狀態**: ✅ 生產就緒  
**版本**: 1.0  
**最後更新**: 2025-12-02
