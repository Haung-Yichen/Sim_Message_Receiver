/**
 * @file health_monitor.h
 * @brief Application-level software watchdog.
 *
 * Complements the hardware watchdogs (Interrupt WDT, Task WDT): those catch a
 * frozen CPU / wedged task, this catches "logic death" -- the device is alive
 * and looping but can no longer deliver messages (WiFi/MQTT down for too long,
 * or the SIM rx_task has silently stopped making progress). On such conditions
 * it triggers a clean esp_restart().
 */
#pragma once

/** Start the background health-monitor task. Call once after the SIM and
 *  WiFi/MQTT subsystems have been started. */
void health_monitor_start(void);

/** Called by the SIM rx_task on every loop iteration to prove it is alive. */
void health_notify_sim_alive(void);
