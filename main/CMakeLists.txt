idf_component_register(
    SRCS
        "main.c"
        "lcd_i2c.c"
    INCLUDE_DIRS
        "."

    REQUIRES
        esp_driver_gpio
        esp_driver_i2c
        bt
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        driver
)

target_compile_options(
    ${COMPONENT_LIB}
    PRIVATE
        -Wno-error=cpp
)