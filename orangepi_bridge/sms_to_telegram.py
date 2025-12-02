import json
import logging
import os
import sys
import time
import threading
import requests
import paho.mqtt.client as mqtt

# --- Configuration ---
# You can set these via environment variables or edit directly
MQTT_BROKER = os.getenv('MQTT_BROKER', 'localhost') # Assumes running on the same Pi as Mosquitto
MQTT_PORT = int(os.getenv('MQTT_PORT', 1883))
MQTT_TOPIC = "sim_bridge/sms"

TELEGRAM_BOT_TOKEN = os.getenv('TELEGRAM_BOT_TOKEN', "YOUR_TELEGRAM_BOT_TOKEN")
# 支援多個 Chat ID，用逗號分隔
TELEGRAM_CHAT_IDS = os.getenv('TELEGRAM_CHAT_IDS', "").split(',')
TELEGRAM_CHAT_IDS = [chat_id.strip() for chat_id in TELEGRAM_CHAT_IDS if chat_id.strip()]

# --- Logging Setup ---
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s',
    handlers=[
        logging.StreamHandler(sys.stdout)
    ]
)
logger = logging.getLogger(__name__)

def send_telegram_message_thread(sender, message):
    """Sends the SMS content to Telegram to all configured chat IDs (Worker Function)."""
    if not TELEGRAM_BOT_TOKEN or TELEGRAM_BOT_TOKEN == "YOUR_TELEGRAM_BOT_TOKEN":
        logger.error("Telegram Bot Token not configured!")
        return

    if not TELEGRAM_CHAT_IDS:
        logger.error("No Telegram Chat IDs configured!")
        return

    url = f"https://api.telegram.org/bot{TELEGRAM_BOT_TOKEN}/sendMessage"
    
    # Format the message nicely
    text = f"📩 *New SMS Received*\n\n" \
           f"👤 *From:* `{sender}`\n" \
           f"📄 *Message:*\n{message}"
    
    # 發送給所有設定的 Chat ID
    for chat_id in TELEGRAM_CHAT_IDS:
        payload = {
            "chat_id": chat_id,
            "text": text,
            "parse_mode": "Markdown"
        }

        try:
            response = requests.post(url, json=payload, timeout=10)
            response.raise_for_status()
            logger.info(f"Forwarded SMS from {sender} to Telegram (Chat ID: {chat_id}).")
        except requests.exceptions.RequestException as e:
            logger.error(f"Failed to send Telegram message to {chat_id}: {e}")

def send_telegram_message(sender, message):
    """Starts a thread to send the message."""
    t = threading.Thread(target=send_telegram_message_thread, args=(sender, message))
    t.start()

def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        logger.info(f"Connected to MQTT Broker at {MQTT_BROKER}")
        client.subscribe(MQTT_TOPIC)
        logger.info(f"Subscribed to topic: {MQTT_TOPIC}")
    else:
        logger.error(f"Failed to connect to MQTT, return code {reason_code}")

def on_message(client, userdata, msg):
    try:
        payload_str = msg.payload.decode('utf-8')
        logger.info(f"Received MQTT message: {payload_str}")
        
        data = json.loads(payload_str)
        sender = data.get("sender", "Unknown")
        content = data.get("message", "")
        
        if content:
            send_telegram_message(sender, content)
        else:
            logger.warning("Received empty message content.")
            
    except json.JSONDecodeError:
        logger.error("Failed to decode JSON payload")
    except Exception as e:
        logger.error(f"Error processing message: {e}")

def main():
    client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_message = on_message

    logger.info("Starting SMS to Telegram Bridge...")

    if not TELEGRAM_BOT_TOKEN or TELEGRAM_BOT_TOKEN == "YOUR_TELEGRAM_BOT_TOKEN":
        logger.warning("Telegram Bot Token is NOT configured. Messages will not be sent.")
    else:
        send_telegram_message("System", "🚀 SMS to Telegram Bridge started successfully.")
    
    while True:
        try:
            client.connect(MQTT_BROKER, MQTT_PORT, 60)
            client.loop_forever()
        except Exception as e:
            logger.error(f"Connection failed: {e}. Retrying in 5 seconds...")
            time.sleep(5)

if __name__ == "__main__":
    main()
