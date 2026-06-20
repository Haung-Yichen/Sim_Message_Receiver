"""
Exhaustive unit tests for heartbeat_monitor (pure state machine + formatting).

Run:  python3 -m unittest test_heartbeat_monitor -v
"""
import unittest

from heartbeat_monitor import (
    HeartbeatMonitor, Alert, format_alert, format_duration, reason_text,
    STATE_UNKNOWN, STATE_ALIVE, STATE_DEAD,
)

TIMEOUT = 90.0


def kinds(alerts):
    return [a.kind for a in alerts]


class TestStateMachine(unittest.TestCase):

    def setUp(self):
        self.m = HeartbeatMonitor(timeout_s=TIMEOUT, started_at=0.0, device="ESP32_7c7038")

    # --- baseline / steady state ---------------------------------------

    def test_first_heartbeat_no_alert(self):
        alerts = self.m.on_heartbeat(now=10, boot_id=111, reset_reason="POWERON", uptime_s=10)
        self.assertEqual(alerts, [])
        self.assertEqual(self.m.state, STATE_ALIVE)
        self.assertEqual(self.m.last_boot_id, 111)

    def test_steady_heartbeats_same_boot_no_alert(self):
        self.m.on_heartbeat(now=10, boot_id=111, uptime_s=10)
        for t in (40, 70, 100, 130):
            self.assertEqual(self.m.on_heartbeat(now=t, boot_id=111, uptime_s=t), [])
        self.assertEqual(self.m.state, STATE_ALIVE)

    def test_check_before_any_heartbeat_within_grace(self):
        # UNKNOWN, not yet past timeout -> nothing
        self.assertEqual(self.m.check(now=TIMEOUT - 1), [])
        self.assertEqual(self.m.state, STATE_UNKNOWN)

    # --- DEAD detection -------------------------------------------------

    def test_timeout_raises_dead_once(self):
        self.m.on_heartbeat(now=10, boot_id=111, uptime_s=10)
        # just before timeout: nothing
        self.assertEqual(self.m.check(now=10 + TIMEOUT), [])
        self.assertEqual(self.m.state, STATE_ALIVE)
        # just after timeout: DEAD
        alerts = self.m.check(now=10 + TIMEOUT + 1)
        self.assertEqual(kinds(alerts), ["DEAD"])
        self.assertFalse(alerts[0].never_seen)
        self.assertEqual(self.m.state, STATE_DEAD)
        # subsequent checks: no repeat (no spam)
        self.assertEqual(self.m.check(now=10 + TIMEOUT + 100), [])
        self.assertEqual(self.m.check(now=10 + TIMEOUT + 1000), [])

    def test_dead_outage_duration(self):
        self.m.on_heartbeat(now=100, boot_id=1, uptime_s=5)
        alerts = self.m.check(now=100 + TIMEOUT + 30)
        self.assertEqual(kinds(alerts), ["DEAD"])
        self.assertAlmostEqual(alerts[0].outage_s, TIMEOUT + 30)

    def test_never_seen_dead(self):
        # No heartbeat ever; after timeout from startup -> DEAD never_seen
        alerts = self.m.check(now=TIMEOUT + 1)
        self.assertEqual(kinds(alerts), ["DEAD"])
        self.assertTrue(alerts[0].never_seen)
        self.assertEqual(self.m.state, STATE_DEAD)
        # then a heartbeat arrives -> RECOVERED
        rec = self.m.on_heartbeat(now=TIMEOUT + 50, boot_id=999, reset_reason="POWERON", uptime_s=3)
        self.assertEqual(kinds(rec), ["RECOVERED"])

    # --- RECOVERED ------------------------------------------------------

    def test_dead_then_recovered_same_boot(self):
        self.m.on_heartbeat(now=10, boot_id=111, uptime_s=10)
        self.m.check(now=10 + TIMEOUT + 1)            # -> DEAD
        alerts = self.m.on_heartbeat(now=300, boot_id=111, reset_reason="POWERON", uptime_s=300)
        self.assertEqual(kinds(alerts), ["RECOVERED"])
        self.assertFalse(alerts[0].restarted)         # same boot => not a restart
        self.assertEqual(self.m.state, STATE_ALIVE)

    def test_dead_then_recovered_new_boot_is_restart(self):
        self.m.on_heartbeat(now=10, boot_id=111, uptime_s=10)
        self.m.check(now=10 + TIMEOUT + 1)            # -> DEAD
        alerts = self.m.on_heartbeat(now=300, boot_id=222, reset_reason="TASK_WDT", uptime_s=8)
        self.assertEqual(kinds(alerts), ["RECOVERED"])
        self.assertTrue(alerts[0].restarted)
        self.assertEqual(alerts[0].reset_reason, "TASK_WDT")
        self.assertEqual(alerts[0].uptime_s, 8)

    def test_recovered_outage_from_last_heartbeat(self):
        self.m.on_heartbeat(now=100, boot_id=1, uptime_s=10)
        self.m.check(now=100 + TIMEOUT + 5)           # -> DEAD
        alerts = self.m.on_heartbeat(now=500, boot_id=1, uptime_s=500)
        # outage measured from last good heartbeat (t=100)
        self.assertAlmostEqual(alerts[0].outage_s, 400)

    # --- RESTARTED (fast reboot, never marked dead) --------------------

    def test_fast_restart_while_alive(self):
        self.m.on_heartbeat(now=10, boot_id=111, reset_reason="POWERON", uptime_s=10)
        # next heartbeat 30s later but with a NEW boot_id -> rebooted quickly
        alerts = self.m.on_heartbeat(now=40, boot_id=222, reset_reason="SW_WATCHDOG_MQTT", uptime_s=3)
        self.assertEqual(kinds(alerts), ["RESTARTED"])
        self.assertEqual(alerts[0].reset_reason, "SW_WATCHDOG_MQTT")
        self.assertEqual(alerts[0].uptime_s, 3)
        self.assertEqual(self.m.state, STATE_ALIVE)

    def test_first_heartbeat_never_restarted(self):
        # boot_id is set for the first time -> must NOT be treated as a restart
        alerts = self.m.on_heartbeat(now=10, boot_id=111, uptime_s=10)
        self.assertEqual(alerts, [])

    # --- full cycle -----------------------------------------------------

    def test_dead_recover_dead_again_cycle(self):
        self.m.on_heartbeat(now=10, boot_id=1, uptime_s=10)
        self.assertEqual(kinds(self.m.check(now=10 + TIMEOUT + 1)), ["DEAD"])
        self.assertEqual(kinds(self.m.on_heartbeat(now=300, boot_id=1, uptime_s=300)), ["RECOVERED"])
        # alive again; can go dead again
        self.assertEqual(self.m.check(now=300 + TIMEOUT - 1), [])
        self.assertEqual(kinds(self.m.check(now=300 + TIMEOUT + 1)), ["DEAD"])

    def test_device_id_updated_from_heartbeat(self):
        m = HeartbeatMonitor(timeout_s=TIMEOUT, device="ESP32")
        m.on_heartbeat(now=10, boot_id=1, device="ESP32_abc123", uptime_s=1)
        self.assertEqual(m.device, "ESP32_abc123")


class TestFormatting(unittest.TestCase):

    def test_format_duration(self):
        self.assertEqual(format_duration(None), "未知")
        self.assertEqual(format_duration(0), "0 秒")
        self.assertEqual(format_duration(45), "45 秒")
        self.assertEqual(format_duration(90), "1 分 30 秒")
        self.assertEqual(format_duration(3600), "1 小時 0 分")
        self.assertEqual(format_duration(3725), "1 小時 2 分")

    def test_reason_text_known_and_unknown(self):
        self.assertEqual(reason_text("TASK_WDT"), "任務看門狗（rx_task 卡死）")
        self.assertEqual(reason_text("SW_WATCHDOG_MQTT"), "軟體看門狗：MQTT 離線過久")
        self.assertEqual(reason_text("WHATEVER"), "WHATEVER")
        self.assertEqual(reason_text(""), "未知")

    def test_format_dead(self):
        msg = format_alert(Alert("DEAD", device="ESP32_7c7038", outage_s=95))
        self.assertIn("🔴", msg)
        self.assertIn("ESP32_7c7038", msg)
        self.assertIn("失聯", msg)

    def test_format_dead_never_seen(self):
        msg = format_alert(Alert("DEAD", device="X", outage_s=95, never_seen=True))
        self.assertIn("仍未收到任何心跳", msg)

    def test_format_recovered_plain(self):
        msg = format_alert(Alert("RECOVERED", device="X", outage_s=120, restarted=False))
        self.assertIn("🟢", msg)
        self.assertIn("未重啟", msg)

    def test_format_recovered_restarted_shows_reason(self):
        msg = format_alert(Alert("RECOVERED", device="X", outage_s=120,
                                 restarted=True, reset_reason="SW_WATCHDOG_SIM", uptime_s=12))
        self.assertIn("曾重啟", msg)
        self.assertIn("SIM 任務卡死", msg)

    def test_format_restarted_shows_reason(self):
        msg = format_alert(Alert("RESTARTED", device="X", reset_reason="TASK_WDT", uptime_s=5))
        self.assertIn("🔄", msg)
        self.assertIn("rx_task 卡死", msg)

    def test_every_alert_kind_renders_nonempty_string(self):
        # Alerts are sent as PLAIN TEXT (no parse_mode), so any character is
        # safe; the invariant is simply that each kind renders a usable string.
        for a in (Alert("DEAD", outage_s=1),
                  Alert("DEAD", outage_s=1, never_seen=True),
                  Alert("RECOVERED", outage_s=1, restarted=False),
                  Alert("RECOVERED", outage_s=1, restarted=True, reset_reason="TASK_WDT"),
                  Alert("RESTARTED", reset_reason="PANIC"),
                  Alert("WEIRD_UNKNOWN_KIND")):
            msg = format_alert(a)
            self.assertIsInstance(msg, str)
            self.assertTrue(len(msg) > 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
