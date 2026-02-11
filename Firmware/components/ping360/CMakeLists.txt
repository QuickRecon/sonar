idf_component_register(SRCS "ping360.c"
                       INCLUDE_DIRS "include"
                       REQUIRES rs485 ping_protocol freertos)
