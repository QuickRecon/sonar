idf_component_register(SRCS "rs485.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_uart esp_driver_gpio)
