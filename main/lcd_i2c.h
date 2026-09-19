#ifndef LCD_I2C_H
#define LCD_I2C_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    i2c_master_bus_handle_t bus_handle;
    i2c_master_dev_handle_t device_handle;

    uint8_t address;
    uint32_t scl_speed_hz;

    uint8_t backlight;
    uint8_t display_control;
    uint8_t display_mode;
} lcd_i2c_config_t;

/**
 * Initialize the LCD.
 *
 * The I2C bus is created inside this function.
 */
esp_err_t lcd_i2c_init(
    const lcd_i2c_config_t *config,
    lcd_i2c_config_t *lcd
);

/**
 * Clear LCD display.
 */
esp_err_t lcd_i2c_clear(lcd_i2c_config_t *lcd);

/**
 * Set cursor position.
 *
 * row: 0 or 1
 * col: 0 to 15
 */
esp_err_t lcd_i2c_set_cursor(
    lcd_i2c_config_t *lcd,
    uint8_t row,
    uint8_t col
);

/**
 * Print text on LCD.
 */
esp_err_t lcd_i2c_print(
    lcd_i2c_config_t *lcd,
    const char *text
);

/**
 * Print text at a specific position.
 */
esp_err_t lcd_i2c_print_at(
    lcd_i2c_config_t *lcd,
    uint8_t row,
    uint8_t col,
    const char *text
);

/**
 * Turn LCD backlight on/off.
 */
esp_err_t lcd_i2c_backlight(
    lcd_i2c_config_t *lcd,
    bool enabled
);

/**
 * Turn display on/off.
 */
esp_err_t lcd_i2c_display(
    lcd_i2c_config_t *lcd,
    bool enabled
);

/**
 * Create a custom character in CGRAM.
 *
 * location: 0 to 7
 * pattern: 8 bytes, each containing 5-bit pixel data
 */
esp_err_t lcd_i2c_create_char(
    lcd_i2c_config_t *lcd,
    uint8_t location,
    const uint8_t pattern[8]
);

/**
 * Write a raw character byte.
 */
esp_err_t lcd_i2c_write_char(
    lcd_i2c_config_t *lcd,
    uint8_t value
);

#ifdef __cplusplus
}
#endif

#endif