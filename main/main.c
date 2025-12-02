#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "config.h"
#include "app_common.h"
#include "wifi_mqtt.h"
#include "sim_modem.h"

static const char *TAG = "MAIN";

#define LED_PIN STATUS_LED_PIN

static void led_blink_task(void *arg)
{
    while(1) {
        switch (g_app_state) {
            case APP_STATE_INIT:
                // Solid On (Not connected to network)
                gpio_set_level(LED_PIN, 1);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
                
            case APP_STATE_WIFI_CONNECTED:
                // Fast Blink (Connected to Network, No MQTT)
                // 100ms ON, 100ms OFF
                gpio_set_level(LED_PIN, 1);
                vTaskDelay(pdMS_TO_TICKS(100));
                gpio_set_level(LED_PIN, 0);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
                
            case APP_STATE_MQTT_CONNECTED:
                // Slow Blink (Normal Operation)
                // 1Hz: 500ms ON, 500ms OFF
                gpio_set_level(LED_PIN, 1);
                vTaskDelay(pdMS_TO_TICKS(500));
                gpio_set_level(LED_PIN, 0);
                vTaskDelay(pdMS_TO_TICKS(500));
                break;
        }
    }
}

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Starting Application...");

    // Initialize WiFi & MQTT
    wifi_mqtt_init();

    // Initialize SIM Module UART
    sim_modem_init_uart();
    
    // Start SIM RX Task (Handles initialization sequence and message loop)
    sim_modem_start_task();
    
    // LED Blink Task
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    xTaskCreate(led_blink_task, "blink_task", 2048, NULL, 1, NULL);
    
    ESP_LOGI(TAG, "Application Started");
}