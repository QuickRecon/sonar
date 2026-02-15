idf_component_register(SRCS "web_server.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server esp_https_server ping360 esp_event power
                       EMBED_TXTFILES "${PROJECT_DIR}/web/index.html"
                                      "${PROJECT_DIR}/web/style.css"
                                      "${PROJECT_DIR}/web/sonar.js"
                                      "${PROJECT_DIR}/certs/server.pem"
                                      "${PROJECT_DIR}/certs/server.key")
