# Project Progress

## What Works

*   **TFT Display:** The display now correctly initializes and displays dynamic data (temperature readings) from the main loop.
*   **Thermocouple:** The MAX31855 thermocouple is providing accurate temperature readings.
*   **Dual SPI Communication:** Both the TFT display and the thermocouple are working simultaneously on separate, dedicated hardware SPI buses (`HSPI` and `FSPI`), which has resolved all previous conflicts.
*   **WiFi Access Point:** The ESP32 can now host its own Wi-Fi network.
*   **Real-time WiFi Status:** The display now shows the number of connected clients in real-time.
*   **Efficient Display Updates:** The screen no longer flickers, as only the changed temperature value is redrawn.
*   **Fault Tolerance:** The system now gracefully handles thermocouple errors and recovers the display when the connection is restored.

## What's Left to Build

*   Implement a more sophisticated UI for the display (e.g., graphs, firing schedule status).
*   Test making a temperature history chart on the display.
*   Implement a firing schedule system (reading from a file or hardcoded).
*   Implement closed-loop fuel control (requires additional hardware).
*   Send sensor data over WiFi to a connected client.

## Current Status

The project has achieved a major milestone: the core hardware components (display and thermocouple) are fully integrated and working reliably. The system can now read a sensor value and display it in real-time, which is the foundational loop for the entire kiln controller. The next step is to build out the user interface and the logic for managing firing schedules.

## Known Issues

*   **Filesystem Instability:** An attempt to add a web server UI failed due to persistent and unresolvable issues with both the SPIFFS and LittleFS filesystems on the ESP32-S3. The board would either fail to mount the filesystem or enter a boot loop when attempting to format it. All related code has been reverted to restore stability.

## Evolution of Project Decisions

*   **Build System:** Added `-I include` to `platformio.ini` to ensure the `include` directory is available during compilation.
*   **SPI Bus Architecture:** After significant troubleshooting, a dual hardware SPI architecture was implemented. This was the critical change that enabled both the display and the thermocouple to work simultaneously without conflicts.
    *   The built-in TFT display uses the default `HSPI` bus, managed automatically by the Adafruit library.
    *   The external MAX31855 thermocouple is explicitly assigned to the `FSPI` bus.
    *   This approach is more robust and reliable than using Software SPI or a shared SPI bus for these devices.
*   **Object Ownership:** The `DisplayManager` was refactored to accept a reference to the global `Adafruit_ST7789` object instead of creating its own. This ensures a single point of control for the display hardware.
*   **Display Logic:** Moved to a partial screen update approach, where only the temperature value is cleared and rewritten. This eliminates flickering and is more efficient.
*   **WiFi Mode:** Implemented the ESP32 as a Wi-Fi Access Point (AP) to allow direct connection from other devices without needing an external router.
*   **State Management:** Refactored all fault-handling and system state logic out of the main loop and into a dedicated `StatusManager` class. This greatly simplifies the main loop and centralizes state-transition logic.
*   **Web Server (Reverted):** An attempt to implement a web server using ESPAsyncWebServer and an onboard filesystem (first SPIFFS, then LittleFS) was made. This effort was plagued by critical, low-level errors, including filesystem mount failures and boot loops during formatting. After extensive troubleshooting (including full flash erases and various configuration changes), the feature was deemed unstable on the current hardware/software combination. All related code and configuration have been reverted to the last known-good state to ensure project stability.