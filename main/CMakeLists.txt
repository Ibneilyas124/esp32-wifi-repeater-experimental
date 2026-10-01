idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_server json lwip esp_timer esp_hw_support driver esp_http_client esp-tls mbedtls
    EMBED_TXTFILES "login.html" "admin.html" "tools.html" "chat.html"
)
