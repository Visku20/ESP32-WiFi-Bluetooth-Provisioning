# ESP32-WiFi-Bluetooth-Provisioning

ESP32 Wi-Fi provisioning project using Classic Bluetooth SPP, a touch-triggered Bluetooth setup mode, and a 16x2 I2C LCD.

## Current working features

- ESP32 target with ESP-IDF 6.1-dev
- Classic Bluetooth SPP device name: `ESP32_WIFI_SETUP`
- Hold the touch input on GPIO4 for 3 seconds to start Bluetooth
- 16x2 I2C LCD at address `0x27`
- Wi-Fi station provisioning using:
  - `SSID:<network-name>`
  - `PASS:<password>`
  - `STATUS`
- LCD status messages for Bluetooth and Wi-Fi
- Wi-Fi reconnect retry handling

## Hardware used

- ESP32
- 16x2 I2C LCD backpack
- Touch input on GPIO4
- I2C LCD:
  - SDA: GPIO21
  - SCL: GPIO22
  - Address: 0x27

## Build

Use ESP-IDF 6.1-dev and set the target to ESP32:

```bash
idf.py set-target esp32
idf.py build
idf.py flash monitor
```

## Important Bluetooth configuration

Enable Bluedroid and Classic Bluetooth/SPP in `idf.py menuconfig`.

The controller must be configured for **BR/EDR (Classic Bluetooth)** to match:

```c
esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
```

## Project structure

```
.
├── CMakeLists.txt
├── README.md
├── .gitignore
└── main
    ├── CMakeLists.txt
    ├── main.c
    ├── lcd_i2c.c
    └── lcd_i2c.h
```

> The current working application source has been uploaded. The custom `lcd_i2c.c/.h` driver source was not present in the file set accessible from this ChatGPT session, so those two driver files still need to be added from the local working project before this repository is a complete standalone build.
