idf_component_register(
    SRCS "main.c" "lcd_i2c.c"
    INCLUDE_DIRS "."
    REQUIRES
        bt
        esp_driver_i2c
        esp_driver_gpio
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        freertos
)
