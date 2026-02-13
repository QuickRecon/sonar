idf_component_register(SRCS "web_server.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server ping360 esp_event
                       EMBED_TXTFILES "${PROJECT_DIR}/web/index.html"
                                      "${PROJECT_DIR}/web/style.css"
                                      "${PROJECT_DIR}/web/sonar.js")
