# System Patterns

## System Architecture
*   Use PlatformIO's `lib` directory for custom libraries.
*   Create separate libraries for distinct hardware components or functionalities (e.g., `DisplayManager`, `StatusManager`).
*   Use a `config.h` file in the `include` directory for shared constants and pin definitions.
*   Keep `src/main.cpp` focused on high-level orchestration (`setup()` and `loop()`), delegating tasks to the libraries.
*   **Dual Hardware SPI:** For multiple SPI devices, utilize the ESP32-S3's two hardware SPI controllers (HSPI and FSPI) by explicitly assigning each device to a separate `SPIClass` instance. This prevents conflicts and ensures reliable communication.

## Key Technical Decisions
*   **SPI Bus Allocation:** The built-in TFT display is implicitly assigned to the default `HSPI` bus by its library. The external MAX31855 thermocouple is explicitly assigned to the `FSPI` bus. This clear separation is crucial for stable operation.
*   **Display Object Management:** The `Adafruit_ST7789` object is declared globally in `main.cpp`, and the `DisplayManager` class receives a reference to it. This ensures a single, authoritative instance of the display driver.
*   **Asynchronous Event Handling:** Use the ESP32's event system (`WiFi.onEvent()`) to trigger actions (like updating the display) in response to system events (like a client connecting to the AP). This is more efficient than polling for status changes in the main loop.

## Design Patterns in Use
*   **Singleton-like Pattern (for Hardware Drivers):** While not strictly a Singleton, the approach of declaring hardware driver objects (like `tft` and `thermocouple`) globally and passing references to other classes ensures that only one instance controls the physical hardware.
*   **Observer Pattern (via WiFi Events):** The use of `onAPClientConnected` and `onAPClientDisconnected` callbacks is a form of the Observer pattern. The main code registers observers (the callback functions) that are automatically notified by the subject (the WiFi driver) when a relevant event occurs.
*   **State Machine:** The `StatusManager` class implements a simple state machine. It uses the `SystemState` enum to define the possible states (NORMAL, FAULT_NAN, etc.) and manages the transitions between them based on sensor input. This centralizes state logic and makes the main loop cleaner.

## Component Relationships
*   `main.cpp` orchestrates the initialization and the main loop. It delegates all state management to `StatusManager` and all direct screen manipulation to `DisplayManager`.
*   `StatusManager` holds a reference to `DisplayManager` so it can command screen changes (e.g., show errors, clear the screen) in response to state changes.
*   `DisplayManager` encapsulates all display-specific logic.
*   `config.h` centralizes all hardware pin definitions and WiFi credentials.

## Critical Implementation Paths
*   **I2C Initialization:** Always call `Wire.begin(SDA_PIN, SCL_PIN)` *before* attempting to initialize I2C devices like the IMU.
*   **Serial Monitor:** Remember the RESET button press after upload.
*   **PlatformIO Library Structure:** For the build system to find and compile custom libraries, they must follow a specific structure. Each library needs its own folder inside `lib/` (e.g., `lib/DisplayManager/`), and the `.h` and `.cpp` files must be placed inside that folder. Placing source files directly in `lib/` or the project root will cause `undefined reference` linker errors.
*   **Screen Rotation:** Be mindful of physical vs. logical screen dimensions after rotation. Perform calculations based on logical dimensions.
*   **Build System Includes:** When a library needs to access headers from the project's `include` directory, add `build_flags = -I include` to the `platformio.ini` file. This ensures the compiler can find the necessary header files.
*   **Fault Recovery:** When implementing fault conditions (e.g., for sensors), also implement the recovery logic. Ensure the system returns to a normal state and updates the UI accordingly once the fault is cleared.
*   **Filesystem Warning (ESP32-S3):** The onboard flash on this specific board has shown extreme instability with both SPIFFS and LittleFS. Attempts to format or mount the filesystem can lead to boot loops or persistent mount failures that are not resolved by a full flash erase. Avoid using the onboard flash for filesystem storage until the root cause (potential core library bug or hardware issue) is better understood. Future attempts at a web UI should prioritize methods that do not rely on an onboard filesystem.
