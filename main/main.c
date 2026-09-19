#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/touch_pad.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_spp_api.h"

#include "lcd_i2c.h"

#define MAIN_TAG "MAIN"
#define TAG "Bluetooth"
//#define CONFIG_COMPILER_WARNINGS_AS_ERRORS 0
/* =========================================================
 * I2C LCD CONFIGURATION
 * ========================================================= */

#define I2C_SDA_GPIO       GPIO_NUM_21
#define I2C_SCL_GPIO       GPIO_NUM_22

#define LCD_I2C_ADDRESS    0x27
#define LCD_I2C_SPEED_HZ   100000

static lcd_i2c_config_t lcd;
static SemaphoreHandle_t lcd_mutex;
/* =========================================================
 * TOUCH CONFIGURATION
 * ========================================================= */

#define TOUCH_GPIO         GPIO_NUM_4
#define TOUCH_PAD          TOUCH_PAD_NUM0
#define TOUCH_THRESHOLD    400

#define BLUETOOTH_HOLD_TIME_MS 3000

static bool touch_was_active = false;
static int64_t touch_start_time_us = 0;

/* =========================================================
 * BLUETOOTH CONFIGURATION
 * ========================================================= */

#define BT_DEVICE_NAME     "ESP32_WIFI_SETUP"

static bool bluetooth_started = false;
static bool bluetooth_connected = false;
static uint32_t bluetooth_handle = 0;

/* Bluetooth received command buffer */
static char bluetooth_rx_buffer[256];
//static size_t bluetooth_rx_index = 0;

/* =========================================================
 * WIFI CONFIGURATION
 * ========================================================= */

static char wifi_ssid[33] = {0};
static char wifi_password[65] = {0};

static bool wifi_connected = false;
//static bool wifi_credentials_received = false;
static uint8_t wifi_retry_count = 0;
#define WIFI_MAX_RETRIES 10

/* Function prototypes */
static void bluetooth_send(const char *message);
static void process_bluetooth_command(const char *command);
/* =========================================================
 * LCD HELPERS
 * ========================================================= */

static void lcd_show(const char *line1, const char *line2)
{
    char lcd_line1[17];
    char lcd_line2[17];

    memset(lcd_line1, ' ', 16);
    memset(lcd_line2, ' ', 16);

    lcd_line1[16] = '\0';
    lcd_line2[16] = '\0';

    if (line1 != NULL) {
        strncpy(lcd_line1, line1, 16);
    }

    if (line2 != NULL) {
        strncpy(lcd_line2, line2, 16);
    }

    if (lcd_mutex != NULL &&
        xSemaphoreTake(lcd_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {

        lcd_i2c_clear(&lcd);

        lcd_i2c_print_at(&lcd, 0, 0, lcd_line1);
        lcd_i2c_print_at(&lcd, 1, 0, lcd_line2);

        xSemaphoreGive(lcd_mutex);
    }
}

/* =========================================================
 * TOUCH FUNCTIONS
 * ========================================================= */

static esp_err_t touch_init(void)
{
    esp_err_t err;

    err = touch_pad_init();

    if (err != ESP_OK)
    {
        ESP_LOGE(
            MAIN_TAG,
            "touch_pad_init failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    err = touch_pad_config(
        TOUCH_PAD,
        0
    );

    if (err != ESP_OK)
    {
        ESP_LOGE(
            MAIN_TAG,
            "touch_pad_config failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    err = touch_pad_set_fsm_mode(
        TOUCH_FSM_MODE_SW
    );

    if (err != ESP_OK)
    {
        ESP_LOGE(
            MAIN_TAG,
            "touch_pad_set_fsm_mode failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    err = touch_pad_sw_start();

    if (err != ESP_OK)
    {
        ESP_LOGE(
            MAIN_TAG,
            "touch_pad_sw_start failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(
        MAIN_TAG,
        "Touch initialized on GPIO4"
    );

    return ESP_OK;
}

static esp_err_t touch_read(uint16_t *touch_value)
{
    if (touch_value == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return touch_pad_read(
        TOUCH_PAD,
        touch_value
    );
}

/* =========================================================
 * LCD INITIALIZATION
 * ========================================================= */

static esp_err_t lcd_init(void)
{
    lcd_i2c_config_t lcd_config =
    {
        .bus_handle = NULL,
        .device_handle = NULL,

        .address = LCD_I2C_ADDRESS,
        .scl_speed_hz = LCD_I2C_SPEED_HZ,

        .backlight = 1,
        .display_control = 0,
        .display_mode = 0,
    };

    return lcd_i2c_init(
        &lcd_config,
        &lcd
    );
}

/* =========================================================
 * WIFI EVENT HANDLER
 * ========================================================= */

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START) {

        wifi_retry_count = 0;
        wifi_connected = false;

        ESP_LOGI(TAG, "Wi-Fi station started");

        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Initial Wi-Fi connect failed: %s",
                     esp_err_to_name(err));
        }
    }

    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {

        wifi_event_sta_disconnected_t *event =
            (wifi_event_sta_disconnected_t *)event_data;

        wifi_connected = false;

        ESP_LOGW(
            TAG,
            "Wi-Fi disconnected, reason=%d, retry=%u/%u",
            event ? event->reason : -1,
            wifi_retry_count,
            WIFI_MAX_RETRIES
        );

        lcd_show("WiFi Discon.", "Retrying...");

        if (wifi_retry_count < WIFI_MAX_RETRIES) {
            wifi_retry_count++;

            vTaskDelay(pdMS_TO_TICKS(1000));

            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Reconnect failed: %s",
                         esp_err_to_name(err));
            }
        } else {
            ESP_LOGE(TAG, "Maximum Wi-Fi retries reached");
            lcd_show("WiFi Failed", "Check router");
        }
    }

    else if (event_base == IP_EVENT &&
         event_id == IP_EVENT_STA_GOT_IP)
        {
            ip_event_got_ip_t *event =
                (ip_event_got_ip_t *)event_data;

            char ip_message[64];

            snprintf(ip_message,
                    sizeof(ip_message),
                    "Wi-Fi connected!\r\nIP: " IPSTR "\r\n",
                    IP2STR(&event->ip_info.ip));

            ESP_LOGI(TAG,
                    "Wi-Fi connected! IP: " IPSTR,
                    IP2STR(&event->ip_info.ip));

            bluetooth_send(ip_message);

            wifi_retry_count = 0;
        }
}

/* =========================================================
 * WIFI INITIALIZATION
 * ========================================================= */

static esp_err_t wifi_init(void)
{
    esp_err_t err;

    err = esp_netif_init();

    if (err != ESP_OK &&
        err != ESP_ERR_INVALID_STATE)
    {
        return err;
    }

    err = esp_event_loop_create_default();

    if (err != ESP_OK &&
        err != ESP_ERR_INVALID_STATE)
    {
        return err;
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_config =
        WIFI_INIT_CONFIG_DEFAULT();

    err = esp_wifi_init(
        &wifi_config
    );

    if (err != ESP_OK)
    {
        return err;
    }

    err = esp_event_handler_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &wifi_event_handler,
        NULL
    );

    if (err != ESP_OK)
    {
        return err;
    }

    err = esp_event_handler_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &wifi_event_handler,
        NULL
    );

    if (err != ESP_OK)
    {
        return err;
    }

    return ESP_OK;
}

/* =========================================================
 * WIFI SCAN HELPER
 * ========================================================= */

static void __attribute__((unused)) scan_wifi_networks(void)
{
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300
    };

    ESP_LOGI(MAIN_TAG, "Starting Wi-Fi scan...");

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(MAIN_TAG, "Wi-Fi scan failed: %s",
                 esp_err_to_name(err));
        return;
    }

    uint16_t ap_count = 0;
    err = esp_wifi_scan_get_ap_num(&ap_count);
    if (err != ESP_OK) {
        ESP_LOGE(MAIN_TAG, "Could not get AP count: %s",
                 esp_err_to_name(err));
        return;
    }

    wifi_ap_record_t *ap_list =
        calloc(ap_count, sizeof(wifi_ap_record_t));

    if (ap_list == NULL) {
        ESP_LOGE(MAIN_TAG, "Could not allocate scan list");
        return;
    }

    err = esp_wifi_scan_get_ap_records(&ap_count, ap_list);
    if (err == ESP_OK) {
        for (uint16_t i = 0; i < ap_count; i++) {
            ESP_LOGI(
                MAIN_TAG,
                "SSID: %-32s RSSI: %d Channel: %d Auth: %d",
                (char *)ap_list[i].ssid,
                ap_list[i].rssi,
                ap_list[i].primary,
                ap_list[i].authmode
            );
        }
    }

    free(ap_list);
}

/* =========================================================
 * CONNECT TO WIFI
 * ========================================================= */

static esp_err_t connect_to_wifi(
    const char *ssid,
    const char *password
)
{
    if (ssid == NULL || password == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t wifi_config = {0};

    strncpy(
        (char *)wifi_config.sta.ssid,
        ssid,
        sizeof(wifi_config.sta.ssid) - 1
    );

    strncpy(
        (char *)wifi_config.sta.password,
        password,
        sizeof(wifi_config.sta.password) - 1
    );

    /*
     * WPA2-Personal is recommended for the first test.
     * The router must broadcast a 2.4 GHz network.
     */
    wifi_config.sta.threshold.authmode =
        WIFI_AUTH_WPA2_PSK;

    esp_err_t err = esp_wifi_set_mode(
        WIFI_MODE_STA
    );

    if (err != ESP_OK)
    {
        return err;
    }

    err = esp_wifi_set_config(
        WIFI_IF_STA,
        &wifi_config
    );

    if (err != ESP_OK)
    {
        return err;
    }

    err = esp_wifi_start();

    if (err != ESP_OK &&
        err != ESP_ERR_INVALID_STATE)
    {
        return err;
    }

    ESP_LOGI(
        MAIN_TAG,
        "Connecting to Wi-Fi SSID: %s",
        ssid
    );

    char ssid_display[15];

    snprintf(
        ssid_display,
        sizeof(ssid_display),
        "%.14s",
        wifi_ssid
    );
    lcd_show(
        "SSID",
        ssid_display
    );

    return ESP_OK;
}

/* =========================================================
 * BLUETOOTH SEND FUNCTION
 * ========================================================= */

static void bluetooth_send(const char *message)
{
    if (!bluetooth_connected)
    {
        return;
    }

    esp_err_t err = esp_spp_write(
        bluetooth_handle,
        strlen(message),
        (uint8_t *)message
    );

    if (err != ESP_OK)
    {
        ESP_LOGE(
            MAIN_TAG,
            "Bluetooth send failed: %s",
            esp_err_to_name(err)
        );
    }
}

/* =========================================================
 * PROCESS BLUETOOTH COMMANDS
 * ========================================================= */

static void process_bluetooth_command(
    const char *command
)
{
    if (command == NULL)
    {
        return;
    }

    /*
     * Remove CR/LF characters sent by Bluetooth terminal apps.
     * This prevents commands such as "PASS:password\\r\\n"
     * from being stored with hidden characters.
     */
    char clean_command[sizeof(bluetooth_rx_buffer)];

    strncpy(clean_command,
            command,
            sizeof(clean_command) - 1);

    clean_command[sizeof(clean_command) - 1] = '\0';

    clean_command[strcspn(clean_command, "\r\n")] = '\0';

    command = clean_command;

    ESP_LOGI(
        MAIN_TAG,
        "Bluetooth command: %s",
        command
    );

    /*
     * Expected:
     *
     * SSID:YourNetwork
     * PASS:YourPassword
     */

    if (strncmp(command, "SSID:", 5) == 0)
    {
        const char *ssid_value = command + 5;

        strncpy(wifi_ssid, ssid_value, sizeof(wifi_ssid) - 1);
        wifi_ssid[sizeof(wifi_ssid) - 1] = '\0';

        ESP_LOGI(TAG, "SSID received: %s", wifi_ssid);
    }
    else if (strncmp(command, "PASS:", 5) == 0)
    {
        const char *password_value = command + 5;

        strncpy(wifi_password, password_value, sizeof(wifi_password) - 1);
        wifi_password[sizeof(wifi_password) - 1] = '\0';

        ESP_LOGI(TAG, "Wi-Fi password received");
        ESP_LOGI(TAG,
                "SSID length=%d, password length=%d",
                strlen(wifi_ssid),
                strlen(wifi_password));

        connect_to_wifi(wifi_ssid, wifi_password);
    }
    else if (strcmp(command, "STATUS") == 0)
    {
        if (wifi_connected)
        {
            bluetooth_send(
                "WiFi connected\r\n"
            );
        }
        else
        {
            bluetooth_send(
                "WiFi not connected\r\n"
            );
        }
    }
    else
    {
        bluetooth_send(
            "Unknown command\r\n"
        );

        bluetooth_send(
            "Use SSID:name or PASS:password\r\n"
        );
    }
}

/* =========================================================
 * BLUETOOTH CALLBACK
 * ========================================================= */

static void bluetooth_callback(
    esp_spp_cb_event_t event,
    esp_spp_cb_param_t *param
)
{
    switch (event) {
            case ESP_SPP_INIT_EVT:
            ESP_LOGI(TAG, "SPP initialized");

            esp_spp_start_srv(
                ESP_SPP_SEC_NONE,
                ESP_SPP_ROLE_SLAVE,
                0,
                "ESP32_SPP_SERVER"
            );
            break;

        case ESP_SPP_START_EVT:
            ESP_LOGI(TAG, "SPP server started");
            break;

        case ESP_SPP_SRV_OPEN_EVT:
            bluetooth_connected = true;
            bluetooth_handle = param->srv_open.handle;

            ESP_LOGI(TAG, "Bluetooth client connected");

            lcd_show("BT Connected", "Send WiFi..");
            break;

        case ESP_SPP_CLOSE_EVT:
            bluetooth_connected = false;
            bluetooth_handle = 0;

            ESP_LOGI(
                TAG,
                "Bluetooth client disconnected"
            );

            lcd_show(
                "BT Discon...",
                "Touch 3 sec"
            );
            break;

        case ESP_SPP_DATA_IND_EVT:
            {
                size_t received_length = param->data_ind.len;

                if (received_length >= sizeof(bluetooth_rx_buffer)) {
                    received_length = sizeof(bluetooth_rx_buffer) - 1;
                }

                memcpy(
                    bluetooth_rx_buffer,
                    param->data_ind.data,
                    received_length
                );

                bluetooth_rx_buffer[received_length] = '\0';

                ESP_LOGI(
                    TAG,
                    "Received: %s",
                    bluetooth_rx_buffer
                );

                process_bluetooth_command(
                    bluetooth_rx_buffer
                );

                break;
            }

        default:
            ESP_LOGI(TAG, "SPP event: %d", event);
            break;
    }
}

/* =========================================================
 * BLUETOOTH INITIALIZATION
 * ========================================================= */
static esp_err_t bluetooth_init(void)
{
    esp_err_t ret;

    /*
     * Prevent accidental repeated initialization.
     */
    if (bluetooth_started) {
        ESP_LOGI(TAG, "Bluetooth already started");
        return ESP_OK;
    }

    /*
     * Release BLE memory because this application
     * only uses Classic Bluetooth / SPP.
     */
    ret = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);

    if (ret != ESP_OK &&
        ret != ESP_ERR_INVALID_STATE) {

        ESP_LOGE(
            TAG,
            "BLE memory release failed: %s",
            esp_err_to_name(ret)
        );
    }

    /*
     * Bluetooth controller configuration.
     */
    esp_bt_controller_config_t bt_cfg =
        BT_CONTROLLER_INIT_CONFIG_DEFAULT();

    /*
     * Initialize controller.
     */

    ESP_LOGI(TAG, "BT config mode = %d", bt_cfg.mode);
    ESP_LOGI(TAG, "Requested mode = %d", ESP_BT_MODE_CLASSIC_BT);
    ret = esp_bt_controller_init(&bt_cfg);

    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Bluetooth controller init failed: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }

    ESP_LOGI(
        TAG,
        "Bluetooth controller initialized"
    );

    /*
     * Enable Classic Bluetooth.
     */
    ret = esp_bt_controller_enable(
        ESP_BT_MODE_CLASSIC_BT
    );

    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Bluetooth controller enable failed: %s",
            esp_err_to_name(ret)
        );

        /*
         * Clean up controller if enable failed.
         */
        esp_bt_controller_deinit();

        return ret;
    }

    ESP_LOGI(
        TAG,
        "Bluetooth controller enabled"
    );

    /*
     * Initialize Bluedroid.
     */
    ret = esp_bluedroid_init();

    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Bluedroid init failed: %s",
            esp_err_to_name(ret)
        );

        esp_bt_controller_disable();
        esp_bt_controller_deinit();

        return ret;
    }

    /*
     * Enable Bluedroid.
     */
    ret = esp_bluedroid_enable();

    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Bluedroid enable failed: %s",
            esp_err_to_name(ret)
        );

        esp_bluedroid_deinit();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();

        return ret;
    }

    /*
     * Set Bluetooth device name.
     */
    ret = esp_bt_gap_set_device_name(
        BT_DEVICE_NAME
    );

    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Device name failed: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }

    /*
     * Register SPP callback.
     */
    ret = esp_spp_register_callback(
        bluetooth_callback
    );

    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "SPP callback registration failed: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }

    /*
     * Configure SPP.
     */
    esp_spp_cfg_t spp_config = {
        .mode = ESP_SPP_MODE_CB,
        .enable_l2cap_ertm = true,
        .tx_buffer_size = 0
    };

    /*
     * Initialize SPP.
     */
    ret = esp_spp_enhanced_init(
        &spp_config
    );

    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "SPP init failed: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }

    /*
     * Make ESP32 discoverable and connectable.
     */
    ret = esp_bt_gap_set_scan_mode(
        ESP_BT_CONNECTABLE,
        ESP_BT_GENERAL_DISCOVERABLE
    );

    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Could not enable discoverable mode: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }

    bluetooth_started = true;

    ESP_LOGI(
        TAG,
        "================================"
    );

    ESP_LOGI(
        TAG,
        "Bluetooth started successfully"
    );

    ESP_LOGI(
        TAG,
        "Device name: %s",
        BT_DEVICE_NAME
    );

    ESP_LOGI(
        TAG,
        "Mode: Classic Bluetooth SPP"
    );

    ESP_LOGI(
        TAG,
        "================================"
    );

    return ESP_OK;
}

/* =========================================================
 * MAIN APPLICATION
 * ========================================================= */

void app_main(void)
{
    ESP_LOGI(
        MAIN_TAG,
        "Starting ESP32 WiFi/Bluetooth controller"
    );

    /*
     * Initialize NVS.
     */
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(
            nvs_flash_erase()
        );

        err = nvs_flash_init();
    }

    if (err != ESP_OK)
    {
        ESP_LOGE(
            MAIN_TAG,
            "NVS initialization failed: %s",
            esp_err_to_name(err)
        );

        return;
    }
    lcd_mutex = xSemaphoreCreateMutex();

    if (lcd_mutex == NULL) {
        ESP_LOGE("LCD", "Failed to create LCD mutex");
        return;
    }

    /*
     * Initialize LCD.
     */
    err = lcd_init();

    if (err != ESP_OK)
    {
        ESP_LOGE(
            MAIN_TAG,
            "LCD initialization failed: %s",
            esp_err_to_name(err)
        );

        return;
    }

    lcd_show(
        "ESP32 Ready",
        "Touch 3 sec"
    );

    /*
     * Initialize Wi-Fi driver.
     */
    err = wifi_init();

    if (err != ESP_OK)
    {
        ESP_LOGE(
            MAIN_TAG,
            "WiFi initialization failed: %s",
            esp_err_to_name(err)
        );

        lcd_show(
            "WiFi Init Err",
            "Check monitor"
        );

        return;
    }

    /*
     * Initialize touch.
     */
    err = touch_init();

    if (err != ESP_OK)
    {
        ESP_LOGE(
            MAIN_TAG,
            "Touch initialization failed: %s",
            esp_err_to_name(err)
        );

        lcd_show(
            "Touch Error",
            "Check monitor"
        );

        return;
    }

    while (1)
    {
        uint16_t touch_value = 0;

        esp_err_t touch_err = touch_read(
            &touch_value
        );

        if (touch_err != ESP_OK)
        {
            ESP_LOGE(
                MAIN_TAG,
                "Touch read failed: %s",
                esp_err_to_name(touch_err)
            );

            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        bool touched =
            touch_value < TOUCH_THRESHOLD;

        int64_t now_us =
            esp_timer_get_time();

        /*
         * Touch started.
         */
        if (touched && !touch_was_active)
        {
            touch_start_time_us =
                now_us;

            touch_was_active = true;

            ESP_LOGI(
                MAIN_TAG,
                "Touch started"
            );
        }

        /*
         * Touch is being held.
         */
        if (touched && touch_was_active && !bluetooth_started)
        {
            int64_t duration_us =
                now_us - touch_start_time_us;

            uint32_t duration_ms =
                duration_us / 1000;

            uint32_t duration_sec =
                duration_ms / 1000;

            uint32_t remaining_ms =
                duration_ms % 1000;

            char duration_text[17];

            snprintf(
                duration_text,
                sizeof(duration_text),
                "%lu.%03lu S",
                (unsigned long)duration_sec,
                (unsigned long)remaining_ms
            );

            ESP_LOGI(
                MAIN_TAG,
                "Touch value: %u, Duration: %lu sec",
                touch_value,
                (unsigned long)duration_sec
            );

            if (!bluetooth_started && duration_ms % 500 < 100) {
                lcd_show("TOUCHED", duration_text);
            }

            /*
             * Start Bluetooth after holding for 3 seconds.
             * Only start it once per touch.
             */
            if (duration_ms >= BLUETOOTH_HOLD_TIME_MS &&
                !bluetooth_started)
            {
                ESP_LOGI(
                    MAIN_TAG,
                    "3-second touch detected"
                );

                lcd_show(
                    "Starting BT..",
                    "Please wait"
                );

                bluetooth_init();
            }
        }

        /*
         * Touch released.
         */
        if (!touched && touch_was_active)
        {
            int64_t duration_us =
                now_us - touch_start_time_us;

            uint32_t duration_ms =
                duration_us / 1000;

            uint32_t duration_sec =
                duration_ms / 1000;

            uint32_t remaining_ms =
                duration_ms % 1000;

            ESP_LOGI(
                MAIN_TAG,
                "Touch released after %lu.%03lu sec",
                (unsigned long)duration_sec,
                (unsigned long)remaining_ms
            );

            char released_text[17];

            snprintf(
                released_text,
                sizeof(released_text),
                "%lu.%03lu S",
                (unsigned long)duration_sec,
                (unsigned long)remaining_ms
            );

            if (bluetooth_started) {
                if (bluetooth_connected) {
                    lcd_show("BT Connected", "Send WiFi..");
                } else {
                    lcd_show("BT Ready", "Connect phone");
                }
            } else {
                lcd_show("RELEASED", released_text);
            }

            touch_was_active = false;
            touch_start_time_us = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}