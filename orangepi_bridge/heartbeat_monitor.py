"""
heartbeat_monitor.py — ESP32 liveness state machine (pure, no I/O).

The ESP32 publishes a periodic heartbeat to MQTT. This module decides, from
those heartbeats plus the wall clock, when to raise alerts:

  - DEAD      : no heartbeat for `timeout_s` (raised once, no spam)
  - RECOVERED : a heartbeat arrives after the device was considered DEAD
  - RESTARTED : the boot_id changed while the device was still ALIVE
                (a reboot that completed fast enough to never look dead)

Both RECOVERED and RESTARTED carry the ESP32 `reset_reason`, so a watchdog
reboot ("TASK_WDT", "SW_WATCHDOG_MQTT", ...) is reported explicitly.

Time is passed in explicitly (monotonic seconds) so every decision is pure and
unit-testable. The caller performs the actual Telegram sending.
"""
from dataclasses import dataclass
from typing import List, Optional

STATE_UNKNOWN = "UNKNOWN"
STATE_ALIVE = "ALIVE"
STATE_DEAD = "DEAD"


@dataclass
class Alert:
    kind: str                          # "DEAD" | "RECOVERED" | "RESTARTED"
    device: str = "ESP32"
    reset_reason: str = ""
    uptime_s: int = 0
    outage_s: Optional[float] = None   # DEAD/RECOVERED: how long silent
    restarted: bool = False            # RECOVERED: came back on a new boot
    never_seen: bool = False           # DEAD: never received a first heartbeat


class HeartbeatMonitor:
    def __init__(self, timeout_s=90.0, started_at=0.0, device="ESP32"):
        self.timeout_s = timeout_s
        self.started_at = started_at
        self.device = device
        self.state = STATE_UNKNOWN
        self.last_hb_time: Optional[float] = None
        self.last_boot_id = None

    def on_heartbeat(self, now, boot_id, reset_reason="", uptime_s=0,
                     device=None) -> List[Alert]:
        """Process an incoming heartbeat; return any alerts to send."""
        if device:
            self.device = device

        alerts: List[Alert] = []
        boot_changed = (self.last_boot_id is not None and boot_id != self.last_boot_id)

        if self.state == STATE_DEAD:
            if self.last_hb_time is not None:
                outage = now - self.last_hb_time
            else:
                outage = now - self.started_at
            alerts.append(Alert("RECOVERED", device=self.device,
                                 reset_reason=reset_reason, uptime_s=uptime_s,
                                 outage_s=outage, restarted=boot_changed))
        elif boot_changed:
            # Was ALIVE but rebooted fast enough that we never marked it dead.
            alerts.append(Alert("RESTARTED", device=self.device,
                                 reset_reason=reset_reason, uptime_s=uptime_s))

        self.state = STATE_ALIVE
        self.last_boot_id = boot_id
        self.last_hb_time = now
        return alerts

    def check(self, now) -> List[Alert]:
        """Periodic liveness check; return a DEAD alert at most once per outage."""
        if self.state == STATE_ALIVE:
            if self.last_hb_time is not None and (now - self.last_hb_time) > self.timeout_s:
                self.state = STATE_DEAD
                return [Alert("DEAD", device=self.device,
                              outage_s=now - self.last_hb_time, never_seen=False)]
        elif self.state == STATE_UNKNOWN:
            # Never saw the device at all since startup.
            if (now - self.started_at) > self.timeout_s:
                self.state = STATE_DEAD
                return [Alert("DEAD", device=self.device,
                              outage_s=now - self.started_at, never_seen=True)]
        return []


# --- Presentation (also pure) ------------------------------------------------

RESET_REASON_TEXT = {
    "POWERON": "上電開機",
    "SW": "軟體重啟",
    "SW_WATCHDOG_MQTT": "軟體看門狗：MQTT 離線過久",
    "SW_WATCHDOG_SIM": "軟體看門狗：SIM 任務卡死",
    "TASK_WDT": "任務看門狗（rx_task 卡死）",
    "INT_WDT": "中斷看門狗",
    "WDT": "看門狗",
    "PANIC": "韌體當機 (panic)",
    "BROWNOUT": "電壓不足 (brownout)",
    "DEEPSLEEP": "深睡喚醒",
    "EXT": "外部重置",
    "OTHER": "其他",
}


def reason_text(reason: str) -> str:
    return RESET_REASON_TEXT.get(reason, reason or "未知")


def format_duration(seconds: Optional[float]) -> str:
    if seconds is None:
        return "未知"
    s = int(round(seconds))
    if s < 0:
        s = 0
    if s < 60:
        return f"{s} 秒"
    m, s = divmod(s, 60)
    if m < 60:
        return f"{m} 分 {s} 秒"
    h, m = divmod(m, 60)
    return f"{h} 小時 {m} 分"


def format_alert(alert: Alert) -> str:
    """Render an Alert as plain text (no Markdown, to avoid parse errors)."""
    d = alert.device or "ESP32"
    if alert.kind == "DEAD":
        if alert.never_seen:
            return (f"🔴 ESP32 失聯警報\n裝置：{d}\n"
                    f"監控啟動後 {format_duration(alert.outage_s)} 仍未收到任何心跳")
        return (f"🔴 ESP32 失聯警報\n裝置：{d}\n"
                f"已 {format_duration(alert.outage_s)} 未收到心跳")
    if alert.kind == "RECOVERED":
        if alert.restarted:
            return (f"🟢 ESP32 已恢復（曾重啟）\n裝置：{d}\n"
                    f"重啟原因：{reason_text(alert.reset_reason)}\n"
                    f"中斷約 {format_duration(alert.outage_s)}，"
                    f"目前已運行 {format_duration(alert.uptime_s)}")
        return (f"🟢 ESP32 已恢復連線\n裝置：{d}\n"
                f"中斷約 {format_duration(alert.outage_s)}（連線恢復，未重啟）")
    if alert.kind == "RESTARTED":
        return (f"🔄 ESP32 已重啟\n裝置：{d}\n"
                f"原因：{reason_text(alert.reset_reason)}\n"
                f"目前已運行 {format_duration(alert.uptime_s)}")
    return f"ESP32 狀態：{alert.kind}"
