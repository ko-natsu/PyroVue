# ESP32-S3 TFT Module Startup Guide

This document provides essential information for starting new projects using the Adafruit Feather ESP32-S3 TFT module, capturing key configurations, libraries, and lessons learned.

## 1. Hardware Overview

*   **Module:** Adafruit Feather ESP32-S3 TFT
*   **Microcontroller:** ESP32-S3
*   **Integrated Peripherals:**
    *   **IMU:** QMI8658C (Accelerometer + Gyroscope)
    *   **Display:** ST7789 TFT LCD (1.14 inch, 135x240 physical resolution)
    *   **Connectivity:** Wi-Fi, Bluetooth LE
    *   **Other:** STEMMA QT/Qwiic connector, LiPo battery connector & charging

## 2. Development Environment Setup

*   **IDE:** Visual Studio Code (VSCode)
*   **Build System:** PlatformIO extension for VSCode
*   **PlatformIO Board ID:** `adafruit_feather_esp32s3_tft`
*   **Framework:** `arduino`
*   **Serial Monitor:**
    *   Set `monitor_speed = 115200` in `platformio.ini`.
    *   **IMPORTANT:** After uploading code via PlatformIO, you typically need to manually press the **RESET** button on the board *after* the upload completes to start the program execution and see Serial output. Sometimes holding **BOOT** then pressing **RESET** (then releasing BOOT) might be needed if the board doesn't enter bootloader mode automatically.

## 3. Core Peripherals & Initialization

### 3.1. TFT Display (ST7789)

*   **Driver Libraries:**
    *   `adafruit/Adafruit GFX Library`
    *   `adafruit/Adafruit ST7735 and ST7789 Library`
*   **Pin Configuration (Hardware SPI):**
    *   `TFT_CS`: 7
    *   `TFT_DC`: 39
    *   `TFT_RST`: 40 (Use -1 if sharing the ESP32 reset pin)
    *   `TFT_BL`: 45 (Backlight control)
*   **Initialization:** Use `Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);`
*   **Resolution & Rotation:**
    *   Physical Resolution: 135 (Width) x 240 (Height)
    *   Common Usage (Landscape): Call `tft.setRotation(1);` in `setup()`.
    *   **Logical Dimensions (After `setRotation(1)`):** Width = 240, Height = 135. Use these logical dimensions for all drawing calculations (e.g., centering elements).
    *   Define constants (e.g., via `build_flags` in `platformio.ini` or a `config.h`) for physical dimensions if needed, but perform calculations based on logical dimensions after rotation. Example `build_flags`:
        ```ini
        build_flags =
          -D TFT_WIDTH=135
          -D TFT_HEIGHT=240
          ; Add other flags here
        ```
*   **Color Format:** Uses 16-bit color (RGB565). Use `tft.color565(r, g, b)` or predefined constants (e.g., `ST77XX_RED`).


## 3. Common Quirks & Nuances

*   **I2C Initialization:** Always call `Wire.begin(SDA_PIN, SCL_PIN)` *before* attempting to initialize I2C devices like the IMU.
*   **Serial Monitor:** Remember the RESET button press after upload.
*   **Screen Rotation:** Be mindful of physical vs. logical screen dimensions after rotation. Perform calculations based on logical dimensions.

## 4. Recommended Project Structure (Based on IMU Visualizer)

*   Use PlatformIO's `lib` directory for custom libraries.
*   Create separate libraries for distinct hardware components or functionalities (e.g., `Display`, `IMU`, `VisualizationLogic`).
*   Use a `config.h` file in the `include` directory for shared constants and pin definitions.
*   Keep `src/main.cpp` focused on orchestration (`setup()` and `loop()`), delegating tasks to the libraries.

## 5. AI Assistant Interaction Protocol Reminder

*   When code changes require building/uploading, the assistant **must** pause and wait for user confirmation on success/failure.
*   The assistant **cannot** directly build or upload code.
*   Pause and ask the user a question also if unexpected errors appear (potential Intellisense lag), waiting for the user to trigger a build and confirm.
