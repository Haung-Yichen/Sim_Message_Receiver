import json
import logging
import os
import sys
import threading
import time
import requests
import paho.mqtt.client as mqtt

from heartbeat_monitor import HeartbeatMonitor, format_alert

# --- Configuration ---
# You can set these via environment variables or edit directly
MQTT_BROKER = os.getenv('MQTT_BROKER', 'localhost') # Assumes running on the same Pi as Mosquitto
MQTT_PORT = int(os.getenv('MQTT_PORT', 1883))
MQTT_TOPIC = "sim_bridge/sms"
HEARTBEAT_TOPIC = os.getenv('HEARTBEAT_TOPIC', "sim_bridge/heartbeat")

# 心跳監控：ESP32 每 ~30s 發一次心跳，超過 timeout 沒收到即視為失聯。
HEARTBEAT_TIMEOUT_S = float(os.getenv('HEARTBEAT_TIMEOUT_S', '90'))
HEARTBEAT_CHECK_INTERVAL_S = float(os.getenv('HEARTBEAT_CHECK_INTERVAL_S', '15'))

TELEGRAM_BOT_TOKEN = os.getenv('TELEGRAM_BOT_TOKEN', "8592100909:AAHdiDrQ0KKoiPRPu9lgqoSg9oPgnwmBEfA")
# 支援多個 Chat ID，用逗號分隔
TELEGRAM_CHAT_IDS = os.getenv('TELEGRAM_CHAT_IDS', "2009374036,8542774724").split(',')
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

# --- Heartbeat monitor (shared between MQTT callback thread and checker thread) ---
monitor = HeartbeatMonitor(timeout_s=HEARTBEAT_TIMEOUT_S, started_at=time.monotonic())
monitor_lock = threading.Lock()


def send_telegram_raw(text, parse_mode=None):
    """Send arbitrary text to all configured chat IDs. Returns True if all OK."""
    if not TELEGRAM_BOT_TOKEN or TELEGRAM_BOT_TOKEN == "YOUR_TELEGRAM_BOT_TOKEN":
        logger.error("Telegram Bot Token not configured!")
        return False
    if not TELEGRAM_CHAT_IDS:
        logger.error("No Telegram Chat IDs configured!")
        return False

    url = f"https://api.telegram.org/bot{TELEGRAM_BOT_TOKEN}/sendMessage"
    all_ok = True
    for chat_id in TELEGRAM_CHAT_IDS:
        payload = {"chat_id": chat_id, "text": text}
        if parse_mode:
            payload["parse_mode"] = parse_mode
        try:
            response = requests.post(url, json=payload, timeout=10)
            response.raise_for_status()
        except requests.exceptions.RequestException as e:
            all_ok = False
            logger.error(f"Failed to send Telegram message to {chat_id}: {e}")
    return all_ok


def send_telegram_message(sender, message):
    """Sends the SMS content to Telegram to all configured chat IDs."""
    text = f"📩 *New SMS Received*\n\n" \
           f"👤 *From:* `{sender}`\n" \
           f"📄 *Message:*\n{message}"
    if send_telegram_raw(text, parse_mode="Markdown"):
        logger.info(f"Forwarded SMS from {sender} to Telegram.")


def send_alerts(alerts):
    """Send heartbeat-monitor alerts to Telegram as plain text (no parse_mode)."""
    for alert in alerts:
        text = format_alert(alert)
        logger.warning(f"Heartbeat alert: {alert.kind} ({alert.device})")
        send_telegram_raw(text)  # plain text: avoids Markdown parse errors


def handle_heartbeat(payload_str):
    """Process a heartbeat JSON payload and dispatch any resulting alerts."""
    try:
        data = json.loads(payload_str)
    except json.JSONDecodeError:
        logger.error("Failed to decode heartbeat JSON")
        return

    now = time.monotonic()
    with monitor_lock:
        alerts = monitor.on_heartbeat(
            now=now,
            boot_id=data.get("boot_id"),
            reset_reason=data.get("reset_reason", ""),
            uptime_s=data.get("uptime_s", 0),
            device=data.get("device"),
        )
    logger.info(f"Heartbeat from {data.get('device')} "
                f"(boot_id={data.get('boot_id')}, uptime={data.get('uptime_s')}s, "
                f"reset={data.get('reset_reason')})")
    send_alerts(alerts)


def heartbeat_watch_loop():
    """Background thread: periodically check for liveness timeouts."""
    while True:
        time.sleep(HEARTBEAT_CHECK_INTERVAL_S)
        now = time.monotonic()
        with monitor_lock:
            alerts = monitor.check(now)
        send_alerts(alerts)


def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        logger.info(f"Connected to MQTT Broker at {MQTT_BROKER}")
        client.subscribe(MQTT_TOPIC)
        client.subscribe(HEARTBEAT_TOPIC)
        logger.info(f"Subscribed to topics: {MQTT_TOPIC}, {HEARTBEAT_TOPIC}")
    else:
        logger.error(f"Failed to connect to MQTT, return code {reason_code}")


def on_message(client, userdata, msg):
    try:
        # 心跳走獨立路徑
        if msg.topic == HEARTBEAT_TOPIC:
            handle_heartbeat(msg.payload.decode('utf-8', errors='replace'))
            return

        payload_str = msg.payload.decode('utf-8', errors='replace')
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

    send_telegram_message("System", "🚀 SMS to Telegram Bridge started successfully.")

    # 啟動心跳監控背景執行緒
    watcher = threading.Thread(target=heartbeat_watch_loop, name="heartbeat_watch", daemon=True)
    watcher.start()
    logger.info(f"Heartbeat monitor started (timeout {HEARTBEAT_TIMEOUT_S}s, "
                f"check every {HEARTBEAT_CHECK_INTERVAL_S}s)")

    while True:
        try:
            client.connect(MQTT_BROKER, MQTT_PORT, 60)
            client.loop_forever()
        except Exception as e:
            logger.error(f"Connection failed: {e}. Retrying in 5 seconds...")
            time.sleep(5)


if __name__ == "__main__":
    main()
