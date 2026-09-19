#include "lcd_i2c.h"

#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"

#define LCD_TAG "LCD_I2C"

/* LCD commands */
#define LCD_CMD_CLEAR_DISPLAY       0x01
#define LCD_CMD_RETURN_HOME         0x02
#define LCD_CMD_ENTRY_MODE_SET      0x04
#define LCD_CMD_DISPLAY_CONTROL     0x08
#define LCD_CMD_CURSOR_SHIFT        0x10
#define LCD_CMD_FUNCTION_SET        0x20
#define LCD_CMD_SET_CGRAM_ADDR      0x40
#define LCD_CMD_SET_DDRAM_ADDR      0x80

/* Entry mode flags */
#define LCD_ENTRY_LEFT              0x02
#define LCD_ENTRY_SHIFT_DECREMENT   0x00

/* Display control flags */
#define LCD_DISPLAY_ON              0x04
#define LCD_CURSOR_ON               0x02
#define LCD_BLINK_ON                0x01

/* Function set flags */
#define LCD_4BIT_MODE               0x00
#define LCD_2LINE                   0x08
#define LCD_5X8_DOTS                0x00

/* PCF8574 backpack pin mapping */
#define LCD_RS                      0x01
#define LCD_RW                      0x02
#define LCD_EN                      0x04
#define LCD_BACKLIGHT               0x08

static esp_err_t lcd_write_byte(
    lcd_i2c_config_t *lcd,
    uint8_t value
);

static esp_err_t lcd_write_command(
    lcd_i2c_config_t *lcd,
    uint8_t command
);

static esp_err_t lcd_write_data(
    lcd_i2c_config_t *lcd,
    uint8_t data
);

static esp_err_t lcd_write_nibble(
    lcd_i2c_config_t *lcd,
    uint8_t nibble,
    uint8_t control
);

static esp_err_t lcd_send_enable_pulse(
    lcd_i2c_config_t *lcd,
    uint8_t data
);

/**
 * Send one byte to PCF8574 backpack.
 */
static esp_err_t lcd_write_byte(
    lcd_i2c_config_t *lcd,
    uint8_t value
)
{
    if (lcd == NULL || lcd->device_handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return i2c_master_transmit(
        lcd->device_handle,
        &value,
        1,
        1000
    );
}

/**
 * Generate the EN pulse.
 */
static esp_err_t lcd_send_enable_pulse(
    lcd_i2c_config_t *lcd,
    uint8_t data
)
{
    esp_err_t err;

    err = lcd_write_byte(lcd, data | LCD_EN);
    if (err != ESP_OK)
    {
        return err;
    }

    esp_rom_delay_us(1);

    err = lcd_write_byte(lcd, data & ~LCD_EN);
    if (err != ESP_OK)
    {
        return err;
    }

    esp_rom_delay_us(50);

    return ESP_OK;
}

/**
 * Send a 4-bit nibble.
 */
static esp_err_t lcd_write_nibble(
    lcd_i2c_config_t *lcd,
    uint8_t nibble,
    uint8_t control
)
{
    uint8_t data;
    esp_err_t err;

    data = (nibble & 0xF0) | control;

    if (lcd->backlight)
    {
        data |= LCD_BACKLIGHT;
    }

    err = lcd_send_enable_pulse(lcd, data);

    return err;
}

/**
 * Send a command byte.
 */
static esp_err_t lcd_write_command(
    lcd_i2c_config_t *lcd,
    uint8_t command
)
{
    esp_err_t err;

    err = lcd_write_nibble(
        lcd,
        command & 0xF0,
        0
    );

    if (err != ESP_OK)
    {
        return err;
    }

    err = lcd_write_nibble(
        lcd,
        (command << 4) & 0xF0,
        0
    );

    if (err != ESP_OK)
    {
        return err;
    }

    if (command == LCD_CMD_CLEAR_DISPLAY ||
        command == LCD_CMD_RETURN_HOME)
    {
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    return ESP_OK;
}

/**
 * Send a data byte.
 */
static esp_err_t lcd_write_data(
    lcd_i2c_config_t *lcd,
    uint8_t data
)
{
    esp_err_t err;

    err = lcd_write_nibble(
        lcd,
        data & 0xF0,
        LCD_RS
    );

    if (err != ESP_OK)
    {
        return err;
    }

    err = lcd_write_nibble(
        lcd,
        (data << 4) & 0xF0,
        LCD_RS
    );

    return err;
}

/**
 * Initialize LCD and I2C bus.
 */
esp_err_t lcd_i2c_init(
    const lcd_i2c_config_t *config,
    lcd_i2c_config_t *lcd
)
{
    if (config == NULL || lcd == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (config->scl_speed_hz == 0)
    {
        ESP_LOGE(
            LCD_TAG,
            "Invalid I2C speed. scl_speed_hz cannot be zero."
        );

        return ESP_ERR_INVALID_ARG;
    }

    memset(lcd, 0, sizeof(lcd_i2c_config_t));

    lcd->address = config->address;
    lcd->scl_speed_hz = config->scl_speed_hz;
    lcd->backlight = 1;
    lcd->display_control =
        LCD_DISPLAY_ON |
        LCD_CURSOR_ON |
        LCD_BLINK_ON;

    lcd->display_mode =
        LCD_ENTRY_LEFT |
        LCD_ENTRY_SHIFT_DECREMENT;

    /*
     * I2C bus configuration.
     *
     * GPIO21 = SDA
     * GPIO22 = SCL
     */
    i2c_master_bus_config_t bus_config =
    {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_21,
        .scl_io_num = GPIO_NUM_22,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(
        &bus_config,
        &lcd->bus_handle
    );

    if (err != ESP_OK)
    {
        ESP_LOGE(
            LCD_TAG,
            "Failed to create I2C bus: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    /*
     * IMPORTANT:
     * The correct field is scl_speed_hz.
     * Do not use clk_speed_hz here.
     */
    i2c_device_config_t device_config =
    {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = lcd->address,
        .scl_speed_hz = lcd->scl_speed_hz,
    };

    err = i2c_master_bus_add_device(
        lcd->bus_handle,
        &device_config,
        &lcd->device_handle
    );

    if (err != ESP_OK)
    {
        ESP_LOGE(
            LCD_TAG,
            "Failed to add LCD device: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    /*
     * LCD power-up delay.
     */
    vTaskDelay(pdMS_TO_TICKS(50));

    /*
     * HD44780 initialization sequence.
     *
     * Start in 8-bit mode, then switch to 4-bit mode.
     */
    err = lcd_write_nibble(lcd, 0x30, 0);
    if (err != ESP_OK)
    {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(5));

    err = lcd_write_nibble(lcd, 0x30, 0);
    if (err != ESP_OK)
    {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(1));

    err = lcd_write_nibble(lcd, 0x30, 0);
    if (err != ESP_OK)
    {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(1));

    /*
     * Switch to 4-bit mode.
     */
    err = lcd_write_nibble(lcd, 0x20, 0);
    if (err != ESP_OK)
    {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(1));

    err = lcd_write_command(
        lcd,
        LCD_CMD_FUNCTION_SET |
        LCD_4BIT_MODE |
        LCD_2LINE |
        LCD_5X8_DOTS
    );

    if (err != ESP_OK)
    {
        return err;
    }

    err = lcd_write_command(
        lcd,
        LCD_CMD_DISPLAY_CONTROL |
        lcd->display_control
    );

    if (err != ESP_OK)
    {
        return err;
    }

    err = lcd_write_command(
        lcd,
        LCD_CMD_CLEAR_DISPLAY
    );

    if (err != ESP_OK)
    {
        return err;
    }

    err = lcd_write_command(
        lcd,
        LCD_CMD_ENTRY_MODE_SET |
        lcd->display_mode
    );

    if (err != ESP_OK)
    {
        return err;
    }

    ESP_LOGI(
        LCD_TAG,
        "LCD initialized. Address: 0x%02X, Speed: %lu Hz",
        lcd->address,
        (unsigned long)lcd->scl_speed_hz
    );

    return ESP_OK;
}

/**
 * Clear LCD.
 */
esp_err_t lcd_i2c_clear(
    lcd_i2c_config_t *lcd
)
{
    return lcd_write_command(
        lcd,
        LCD_CMD_CLEAR_DISPLAY
    );
}

/**
 * Set LCD cursor.
 */
esp_err_t lcd_i2c_set_cursor(
    lcd_i2c_config_t *lcd,
    uint8_t row,
    uint8_t col
)
{
    static const uint8_t row_offsets[] =
    {
        0x00,
        0x40
    };

    if (lcd == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (row > 1 || col > 15)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return lcd_write_command(
        lcd,
        LCD_CMD_SET_DDRAM_ADDR |
        (row_offsets[row] + col)
    );
}

/**
 * Print text.
 */
esp_err_t lcd_i2c_print(
    lcd_i2c_config_t *lcd,
    const char *text
)
{
    if (lcd == NULL || text == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    while (*text)
    {
        esp_err_t err = lcd_write_data(
            lcd,
            (uint8_t)*text
        );

        if (err != ESP_OK)
        {
            return err;
        }

        text++;
    }

    return ESP_OK;
}

/**
 * Print text at row/column.
 */
esp_err_t lcd_i2c_print_at(
    lcd_i2c_config_t *lcd,
    uint8_t row,
    uint8_t col,
    const char *text
)
{
    esp_err_t err;

    err = lcd_i2c_set_cursor(
        lcd,
        row,
        col
    );

    if (err != ESP_OK)
    {
        return err;
    }

    return lcd_i2c_print(
        lcd,
        text
    );
}

/**
 * Enable/disable backlight.
 */
esp_err_t lcd_i2c_backlight(
    lcd_i2c_config_t *lcd,
    bool enabled
)
{
    if (lcd == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    lcd->backlight = enabled ? 1 : 0;

    return lcd_write_byte(
        lcd,
        lcd->backlight ? LCD_BACKLIGHT : 0
    );
}

/**
 * Enable/disable display.
 */
esp_err_t lcd_i2c_display(
    lcd_i2c_config_t *lcd,
    bool enabled
)
{
    if (lcd == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (enabled)
    {
        lcd->display_control |= LCD_DISPLAY_ON;
    }
    else
    {
        lcd->display_control &= ~LCD_DISPLAY_ON;
    }

    return lcd_write_command(
        lcd,
        LCD_CMD_DISPLAY_CONTROL |
        lcd->display_control
    );
}

/**
 * Create a custom character.
 */
esp_err_t lcd_i2c_create_char(
    lcd_i2c_config_t *lcd,
    uint8_t location,
    const uint8_t pattern[8]
)
{
    esp_err_t err;

    if (lcd == NULL || pattern == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (location > 7)
    {
        return ESP_ERR_INVALID_ARG;
    }

    err = lcd_write_command(
        lcd,
        LCD_CMD_SET_CGRAM_ADDR |
        ((location & 0x07) << 3)
    );

    if (err != ESP_OK)
    {
        return err;
    }

    for (int i = 0; i < 8; i++)
    {
        err = lcd_write_data(
            lcd,
            pattern[i]
        );

        if (err != ESP_OK)
        {
            return err;
        }
    }

    return ESP_OK;
}

/**
 * Write raw character byte.
 */
esp_err_t lcd_i2c_write_char(
    lcd_i2c_config_t *lcd,
    uint8_t value
)
{
    return lcd_write_data(
        lcd,
        value
    );
}
