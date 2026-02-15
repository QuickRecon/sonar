idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES led power rs485 ping360 ms5837 canbus
                                wifi_ap mdns_service web_server esp_driver_i2c
                                nvs_flash esp_netif esp_event)
