import json
import logging
import os
import sys
import time
import requests
import paho.mqtt.client as mqtt

# --- Configuration ---
# You can set these via environment variables or edit directly
MQTT_BROKER = os.getenv('MQTT_BROKER', 'localhost') # Assumes running on the same Pi as Mosquitto
MQTT_PORT = int(os.getenv('MQTT_PORT', 1883))
MQTT_TOPIC = "sim_bridge/sms"

TELEGRAM_BOT_TOKEN = "YOUR_TELEGRAM_BOT_TOKEN"
TELEGRAM_CHAT_ID = "YOUR_TELEGRAM_CHAT_ID"

# --- Logging Setup ---
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s',
    handlers=[
        logging.StreamHandler(sys.stdout)
    ]
)
logger = logging.getLogger(__name__)

def send_telegram_message(sender, message):
    """Sends the SMS content to Telegram."""
    if not TELEGRAM_BOT_TOKEN or TELEGRAM_BOT_TOKEN == "YOUR_TELEGRAM_BOT_TOKEN":
        logger.error("Telegram Bot Token not configured!")
        return

    url = f"https://api.telegram.org/bot{TELEGRAM_BOT_TOKEN}/sendMessage"
    
    # Format the message nicely
    text = f"📩 *New SMS Received*\n\n" \
           f"👤 *From:* `{sender}`\n" \
           f"📄 *Message:*\n{message}"
    
    payload = {
        "chat_id": TELEGRAM_CHAT_ID,
        "text": text,
        "parse_mode": "Markdown"
    }

    try:
        response = requests.post(url, json=payload, timeout=10)
        response.raise_for_status()
        logger.info(f"Forwarded SMS from {sender} to Telegram.")
    except requests.exceptions.RequestException as e:
        logger.error(f"Failed to send Telegram message: {e}")

def on_connect(client, userdata, flags, rc):
    if rc == 0:
        logger.info(f"Connected to MQTT Broker at {MQTT_BROKER}")
        client.subscribe(MQTT_TOPIC)
        logger.info(f"Subscribed to topic: {MQTT_TOPIC}")
    else:
        logger.error(f"Failed to connect to MQTT, return code {rc}")

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
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message

    logger.info("Starting SMS to Telegram Bridge...")
    
    while True:
        try:
            client.connect(MQTT_BROKER, MQTT_PORT, 60)
            client.loop_forever()
        except Exception as e:
            logger.error(f"Connection failed: {e}. Retrying in 5 seconds...")
            time.sleep(5)

if __name__ == "__main__":
    main()
