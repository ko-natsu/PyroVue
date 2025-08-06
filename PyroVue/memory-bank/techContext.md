# Technical Context

## Technologies Used
*   **Hardware:** Adafruit Feather ESP32-S3 TFT
*   **Microcontroller:** ESP32-S3
*   **Integrated Peripherals:**
    *   **IMU:** QMI8658C (Accelerometer + Gyroscope)
    *   **Display:** ST7789 TFT LCD (1.14 inch, 135x240 physical resolution)
    *   **Connectivity:** Wi-Fi, Bluetooth LE
    *   **Other:** STEMMA QT/Qwiic connector, LiPo battery connector & charging
*   **IDE:** Visual Studio Code with the PlatformIO extension.
*   **Framework:** Arduino
*   **PlatformIO Board ID:** `adafruit_feather_esp32s3_tft`

## Development Setup
*   **Serial Monitor Speed:** `monitor_speed = 115200` in `platformio.ini`.
*   **Physical Reset:** After uploading code, the **RESET** button on the board must be manually pressed to start the program and see serial output. Sometimes holding **BOOT** then pressing **RESET** (then releasing BOOT) might be needed if the board's auto-reset doesn't work.

## Pinouts

### TFT Display (ST7789)

*   **Driver Libraries:**
    *   `adafruit/Adafruit GFX Library`
    *   `adafruit/Adafruit ST7735 and ST7789 Library`
*   **SPI Bus:** Uses the default `HSPI` controller.
*   **Pin Configuration (Hardware SPI):**
    *   `TFT_CS`: 7
    *   `TFT_DC`: 39
    *   `TFT_RST`: 40 (Use -1 if sharing the ESP32 reset pin)
    *   `TFT_BL`: 45 (Backlight control)
    *   **Note:** The SCLK and MOSI pins for the TFT are hardwired and managed by the library when using the default constructor.
*   **Initialization:** Use `Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);`
*   **Resolution & Rotation:**
    *   Physical Resolution: 135 (Width) x 240 (Height)
    *   Common Usage (Landscape): Call `tft.setRotation(1);` in `setup()`.
    *   **Logical Dimensions (After `setRotation(1)`):** Width = 240, Height = 135. Use these logical dimensions for all drawing calculations (e.g., centering elements).
*   **Color Format:** Uses 16-bit color (RGB565). Use `tft.color565(r, g, b)` or predefined constants (e.g., `ST77XX_RED`).

### Thermocouple (MAX31855)

*   **Driver Library:** `adafruit/Adafruit MAX31855 library`
*   **SPI Bus:** Uses the `FSPI` controller.
*   **Pin Configuration (Hardware SPI):**
    *   `THERMOCOUPLE_CS`: 8
    *   `THERMOCOUPLE_MOSI`: 35
    *   `THERMOCOUPLE_MISO`: 37
    *   `THERMOCOUPLE_SCK`: 36
*   **Initialization:** Requires explicit `SPIClass` initialization for `FSPI` (e.g., `spi_thermo.begin(THERMOCOUPLE_SCK, THERMOCOUPLE_MISO, THERMOCOUPLE_MOSI);`) before initializing the `Adafruit_MAX31855` object.

## Dependencies
*   `adafruit/Adafruit GFX Library`
*   `adafruit/Adafruit ST7735 and ST7789 Library`
*   `adafruit/Adafruit MAX31855 library`

## Tool Usage Patterns
*   **PlatformIO:** `platformio.ini` is configured to include the `include` directory for shared headers (`build_flags = -I include`).