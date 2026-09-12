idf_component_register(
    SRCS "uart_sock_server.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_event
        nvs_flash
        driver
)