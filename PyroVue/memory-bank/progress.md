# Project Progress

## What Works

*   **TFT Display:** The display now correctly initializes and displays dynamic data (temperature readings) from the main loop.
*   **Thermocouple:** The MAX31855 thermocouple is providing accurate temperature readings.
*   **Dual SPI Communication:** Both the TFT display and the thermocouple are working simultaneously on separate, dedicated hardware SPI buses (`HSPI` and `FSPI`), which has resolved all previous conflicts.

## What's Left to Build

*   Implement a more sophisticated UI for the display (e.g., graphs, firing schedule status).
*   Make the display fancier, maybe by only clearing the number that needs to be updated instead of the whole screen.
*   Remove serial monitor wait.
*   Test Wi-Fi connection.
*   Test making a temperature history chart on the display.
*   Implement a firing schedule system (reading from a file or hardcoded).
*   Implement closed-loop fuel control (requires additional hardware).

## Current Status

The project has achieved a major milestone: the core hardware components (display and thermocouple) are fully integrated and working reliably. The system can now read a sensor value and display it in real-time, which is the foundational loop for the entire kiln controller. The next step is to build out the user interface and the logic for managing firing schedules.

## Known Issues

*   None at this time.

## Evolution of Project Decisions

*   **Build System:** Added `-I include` to `platformio.ini` to ensure the `include` directory is available during compilation.
*   **SPI Bus Architecture:** After significant troubleshooting, a dual hardware SPI architecture was implemented. This was the critical change that enabled both the display and the thermocouple to work simultaneously without conflicts.
    *   The built-in TFT display uses the default `HSPI` bus, managed automatically by the Adafruit library.
    *   The external MAX31855 thermocouple is explicitly assigned to the `FSPI` bus.
    *   This approach is more robust and reliable than using Software SPI or a shared SPI bus for these devices.
*   **Object Ownership:** The `DisplayManager` was refactored to accept a reference to the global `Adafruit_ST7789` object instead of creating its own. This ensures a single point of control for the display hardware.