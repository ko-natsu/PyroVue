# System Patterns

## System Architecture
*   Use PlatformIO's `lib` directory for custom libraries.
*   Create separate libraries for distinct hardware components or functionalities (e.g., `Display`, `IMU`, `VisualizationLogic`).
*   Use a `config.h` file in the `include` directory for shared constants and pin definitions.
*   Keep `src/main.cpp` focused on orchestration (`setup()` and `loop()`), delegating tasks to the libraries.
*   **Dual Hardware SPI:** For multiple SPI devices, utilize the ESP32-S3's two hardware SPI controllers (HSPI and FSPI) by explicitly assigning each device to a separate `SPIClass` instance. This prevents conflicts and ensures reliable communication.

## Key Technical Decisions
*   **SPI Bus Allocation:** The built-in TFT display is implicitly assigned to the default `HSPI` bus by its library. The external MAX31855 thermocouple is explicitly assigned to the `FSPI` bus. This clear separation is crucial for stable operation.
*   **Display Object Management:** The `Adafruit_ST7789` object is declared globally in `main.cpp`, and the `DisplayManager` class receives a reference to it. This ensures a single, authoritative instance of the display driver.

## Design Patterns in Use
*   **Singleton-like Pattern (for Hardware Drivers):** While not strictly a Singleton, the approach of declaring hardware driver objects (like `tft` and `thermocouple`) globally and passing references (or pointers) to other classes ensures that only one instance controls the physical hardware.

## Component Relationships
*   `main.cpp` orchestrates the initialization and interaction between `DisplayManager` and the `Adafruit_MAX31855` driver.
*   `DisplayManager` encapsulates display-specific logic and operates on a reference to the global `Adafruit_ST7789` object.
*   `config.h` centralizes all hardware pin definitions.

## Critical Implementation Paths
*   **I2C Initialization:** Always call `Wire.begin(SDA_PIN, SCL_PIN)` *before* attempting to initialize I2C devices like the IMU.
*   **Serial Monitor:** Remember the RESET button press after upload.
*   **PlatformIO Library Structure:** For the build system to find and compile custom libraries, they must follow a specific structure. Each library needs its own folder inside `lib/` (e.g., `lib/DisplayManager/`), and the `.h` and `.cpp` files must be placed inside that folder. Placing source files directly in `lib/` or the project root will cause `undefined reference` linker errors.
*   **Screen Rotation:** Be mindful of physical vs. logical screen dimensions after rotation. Perform calculations based on logical dimensions.
*   **Build System Includes:** When a library needs to access headers from the project's `include` directory, add `build_flags = -I include` to the `platformio.ini` file. This ensures the compiler can find the necessary header files.
