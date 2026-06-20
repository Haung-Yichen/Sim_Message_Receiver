"""
Integration tests for sms_to_telegram wiring (no network, no real Telegram).

Stubs send_telegram_raw to capture outgoing messages and drives a controllable
monotonic clock, so the full coordination flow can be asserted:
  SMS routing, heartbeat alive, DEAD on timeout, RECOVERED-via-restart, fast
  restart, and malformed payloads.

Run:  python3 -m unittest test_bridge_integration -v
"""
import json
import unittest

import sms_to_telegram as bridge
from heartbeat_monitor import HeartbeatMonitor


class FakeMsg:
    def __init__(self, topic, payload):
        self.topic = topic
        self.payload = payload.encode("utf-8") if isinstance(payload, str) else payload


class TestBridgeIntegration(unittest.TestCase):

    def setUp(self):
        self.sent = []
        self._orig_raw = bridge.send_telegram_raw
        bridge.send_telegram_raw = lambda text, parse_mode=None: (
            self.sent.append((text, parse_mode)) or True
        )
        # controllable clock shared with the bridge module
        self.clock = [1000.0]
        self._orig_monotonic = bridge.time.monotonic
        bridge.time.monotonic = lambda: self.clock[0]
        bridge.monitor = HeartbeatMonitor(timeout_s=90.0, started_at=self.clock[0], device="ESP32")

    def tearDown(self):
        bridge.send_telegram_raw = self._orig_raw
        bridge.time.monotonic = self._orig_monotonic

    # helpers ----------------------------------------------------------------
    def advance(self, dt):
        self.clock[0] += dt

    def deliver_hb(self, boot_id, reason="POWERON", uptime=10, device="ESP32_7c7038"):
        payload = json.dumps({"device": device, "boot_id": boot_id,
                              "reset_reason": reason, "uptime_s": uptime, "mqtt": True})
        bridge.on_message(None, None, FakeMsg(bridge.HEARTBEAT_TOPIC, payload))

    def run_checker_once(self):
        # one iteration of heartbeat_watch_loop's body, without the thread/sleep
        with bridge.monitor_lock:
            alerts = bridge.monitor.check(bridge.time.monotonic())
        bridge.send_alerts(alerts)

    # tests ------------------------------------------------------------------
    def test_sms_routed_to_sms_path_markdown(self):
        msg = FakeMsg(bridge.MQTT_TOPIC, json.dumps({"sender": "105", "message": "code 1234"}))
        bridge.on_message(None, None, msg)
        self.assertEqual(len(self.sent), 1)
        text, pm = self.sent[0]
        self.assertEqual(pm, "Markdown")
        self.assertIn("code 1234", text)

    def test_first_heartbeat_no_alert(self):
        self.deliver_hb(boot_id=1)
        self.assertEqual(self.sent, [])

    def test_dead_alert_on_timeout_then_recovered_via_restart(self):
        self.deliver_hb(boot_id=1, reason="POWERON")
        self.assertEqual(self.sent, [])

        # silence past the timeout -> checker raises DEAD (once)
        self.advance(100)
        self.run_checker_once()
        self.assertEqual(len(self.sent), 1)
        self.assertIn("失聯", self.sent[0][0])
        # no Markdown parse_mode on alerts
        self.assertIsNone(self.sent[0][1])
        # no spam on subsequent checks
        self.advance(30)
        self.run_checker_once()
        self.assertEqual(len(self.sent), 1)

        # ESP32 watchdog-reboots and reports back with a NEW boot_id
        self.sent.clear()
        self.advance(20)
        self.deliver_hb(boot_id=2, reason="TASK_WDT", uptime=8)
        self.assertEqual(len(self.sent), 1)
        recovered = self.sent[0][0]
        self.assertIn("已恢復", recovered)
        self.assertIn("rx_task", recovered)   # the watchdog reason is shown

    def test_fast_restart_reports_without_dead(self):
        self.deliver_hb(boot_id=1)
        self.sent.clear()
        self.advance(30)  # well within the 90s timeout
        self.deliver_hb(boot_id=2, reason="SW_WATCHDOG_MQTT", uptime=3)
        self.assertEqual(len(self.sent), 1)
        self.assertIn("已重啟", self.sent[0][0])
        self.assertIn("MQTT", self.sent[0][0])

    def test_never_seen_dead_then_recovered(self):
        # No heartbeat at all -> after timeout the checker flags DEAD(never seen)
        self.advance(100)
        self.run_checker_once()
        self.assertEqual(len(self.sent), 1)
        self.assertIn("仍未收到任何心跳", self.sent[0][0])
        # then the device shows up
        self.sent.clear()
        self.deliver_hb(boot_id=7, reason="POWERON", uptime=2)
        self.assertEqual(len(self.sent), 1)
        self.assertIn("已恢復", self.sent[0][0])

    def test_bad_heartbeat_json_does_not_crash(self):
        bridge.on_message(None, None, FakeMsg(bridge.HEARTBEAT_TOPIC, "{not json"))
        self.assertEqual(self.sent, [])

    def test_bad_sms_json_does_not_crash(self):
        bridge.on_message(None, None, FakeMsg(bridge.MQTT_TOPIC, "{not json"))
        self.assertEqual(self.sent, [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
