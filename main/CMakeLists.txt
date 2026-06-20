idf_component_register(SRCS "pdu_decoder.c" "main.c" "wifi_mqtt.c" "sim_modem.c" "health_logic.c" "health_monitor.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES nvs_flash esp_wifi esp_event esp_netif mqtt cjson esp_driver_uart esp_driver_gpio esp_timer esp_system esp_hw_support)