idf_component_register(SRCS "pdu_decoder.c" "main.c" "wifi_mqtt.c" "sim_modem.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES nvs_flash esp_wifi esp_event esp_netif mqtt json driver)