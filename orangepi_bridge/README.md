# Orange Pi SMS to Telegram Bridge

這是一個 Python 腳本，用於在 Orange Pi 上運行，監聽來自 ESP32 的 MQTT 訊息，並將其轉發到 Telegram Bot。

## 1. 準備工作

### 申請 Telegram Bot

1. 在 Telegram 中搜尋 `@BotFather`。
2. 輸入 `/newbot` 創建新機器人。
3. 獲取 **API Token**。
4. 獲取你的 **Chat ID** (可以使用 `@userinfobot` 查詢)。

### 安裝環境

在 Orange Pi 上執行：

```bash
# 更新系統
sudo apt update
sudo apt install python3 python3-pip

# 安裝必要的 Python 套件
pip3 install -r requirements.txt
```

## 2. 設定腳本

打開 `sms_to_telegram.py`，修改以下變數：

```python
TELEGRAM_BOT_TOKEN = "你的_BOT_TOKEN"
TELEGRAM_CHAT_ID = "你的_CHAT_ID"
```

如果你的 MQTT Broker 不是運行在 localhost (本機)，請修改：

```python
MQTT_BROKER = "localhost" 
```

## 3. 運行

### 測試運行

```bash
python3 sms_to_telegram.py
```

### 設定為 Systemd 服務 (開機自動啟動)

1. 創建服務文件：

   ```bash
   sudo nano /etc/systemd/system/sms-bridge.service
   ```

2. 貼上以下內容 (請修改路徑為你的實際路徑)：

   ```ini
   [Unit]
   Description=SMS to Telegram Bridge
   After=network.target mosquitto.service

   [Service]
   Type=simple
   User=orangepi
   WorkingDirectory=/home/orangepi/orangepi_bridge
   ExecStart=/usr/bin/python3 /home/orangepi/orangepi_bridge/sms_to_telegram.py
   Restart=always
   RestartSec=10

   [Install]
   WantedBy=multi-user.target
   ```

3. 啟用並啟動服務：

   ```bash
   sudo systemctl daemon-reload
   sudo systemctl enable sms-bridge.service
   sudo systemctl start sms-bridge.service
   ```

4. 查看狀態：

   ```bash
   sudo systemctl status sms-bridge.service
   ```
