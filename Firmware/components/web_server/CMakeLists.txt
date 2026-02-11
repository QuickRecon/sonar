idf_component_register(SRCS "web_server.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server spiffs ping360 esp_event)
