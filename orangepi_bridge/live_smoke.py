"""
Live end-to-end smoke test against the REAL MQTT broker (no production impact).

- Uses a dedicated test heartbeat topic, short timeouts, and stubs Telegram so
  nothing is actually sent.
- Exercises real paho pub/sub + the background checker thread + the alert path.

Run on the Pi:  python3 live_smoke.py
Expect to see (in order): heartbeat (no alert) -> DEAD -> RECOVERED(restarted).
"""
import os
os.environ["HEARTBEAT_TOPIC"] = "test/heartbeat"
os.environ["HEARTBEAT_TIMEOUT_S"] = "5"
os.environ["HEARTBEAT_CHECK_INTERVAL_S"] = "1"

import json
import threading
import time
import paho.mqtt.client as mqtt

import sms_to_telegram as bridge

captured = []
bridge.send_telegram_raw = lambda text, parse_mode=None: (
    captured.append(text) or print(f"  >> TELEGRAM: {text.splitlines()[0]}") or True
)

# background liveness checker (same loop the service runs)
threading.Thread(target=bridge.heartbeat_watch_loop, daemon=True).start()

# subscriber wired with the real bridge callbacks
sub = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
sub.on_connect = bridge.on_connect
sub.on_message = bridge.on_message
sub.connect("localhost", 1883, 60)
sub.loop_start()
time.sleep(1.0)

pub = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
pub.connect("localhost", 1883, 60)
pub.loop_start()


def hb(boot_id, reason="POWERON", uptime=10):
    pub.publish("test/heartbeat", json.dumps({
        "device": "ESP32_test", "boot_id": boot_id,
        "reset_reason": reason, "uptime_s": uptime, "mqtt": True}))


print("[t=0] sending first heartbeat (boot_id=1) -> expect NO alert")
hb(1)
time.sleep(2)

print("[t=2] stop heartbeats, wait past 5s timeout -> expect DEAD alert")
time.sleep(6)

print("[t=8] device reboots (watchdog), heartbeat boot_id=2 TASK_WDT -> expect RECOVERED(restarted)")
hb(2, reason="TASK_WDT", uptime=8)
time.sleep(2)

sub.loop_stop(); pub.loop_stop()

dead = [m for m in captured if "失聯" in m]
recovered = [m for m in captured if "已恢復" in m]
print("\nRESULT:")
print(f"  DEAD alerts:      {len(dead)}")
print(f"  RECOVERED alerts: {len(recovered)}")
ok = len(dead) == 1 and len(recovered) == 1 and "rx_task" in recovered[0]
print("  SMOKE TEST:", "PASS" if ok else "FAIL")
