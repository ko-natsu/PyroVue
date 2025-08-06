# Project Brief: PyroVue Kiln Monitor & Controller

## Core Goal  
Develop a modular, open-source controller that can read kiln temperature with a K-type thermocouple, show the live reading on an onboard TFT, and grow toward basic closed-loop fuel control.

## Development Environment  
* **Hardware:** ESP32-S2/S3 Feather-class board with integrated TFT and SPI-based K-type thermocouple amplifier  
* **IDE:** Visual Studio Code with the PlatformIO extension  
* **Framework:** Arduino  

## Initial Requirements  
1. Create a clean repository structure:  
   * `firmware/esp32` for MCU code  
   * `web/` reserved for a future dashboard  
   * `docs/` for documentation (this file lives here)  
2. Read temperature from one K-type thermocouple and display the value on the TFT and over Serial.  
3. Integrate esp32StartupGuide.md info into a GEMINI.md file for future cross-chat referencing


## Future Scope (Potential)  
* Ring-buffer temperature history displayed on a minimal web dashboard
* Analysis and making a model of the relationship between temperature and fuel consumption rate/fuel air mix
* Analysis and making a mode of the relationship between temperature and exhaust vent closing
* Support for O2 and CO2 sensors for the exhaust, to determine the atmosphere inside (Oxidizing or Reducing)
* PID-based servo control of a gas valve for simple set-point regulation
* Support for multiple thermocouples
* Board-agnostic driver layer so other microcontrollers or SBCs can reuse the code  
* Over-the-air firmware updates and persistent data logging
* Web dashboard with a ring-buffer for temperature history.
* Analysis of temperature vs. fuel consumption and exhaust vent position.
* Support for O2 and CO2 sensors.
* PID-based servo control of a gas valve.
* Support for multiple thermocouples.
* Over-the-air firmware updates and data logging.