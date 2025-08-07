#ifndef STATUS_MANAGER_H
#define STATUS_MANAGER_H

#include <stdint.h>
#include "DisplayManager.h"
#include "Adafruit_MAX31855.h"

// Enum to represent the system's operational state
enum SystemState {
    STATE_NORMAL,
    STATE_FAULT_NAN,
    STATE_FAULT_OPEN,
    STATE_FAULT_GND,
    STATE_FAULT_VCC
};

class StatusManager {
public:
    // Constructor requires a reference to the DisplayManager
    StatusManager(DisplayManager& displayMgr);

    // Call this in the main loop to update the system state
    void update(double tempCelsius, uint8_t fault);

    // Get the current state
    SystemState getCurrentState();

private:
    DisplayManager& displayManager;
    SystemState currentState;
    SystemState previousState;

    // Handles the logic for when a state change is detected
    void handleStateChange(SystemState newState);
};

#endif // STATUS_MANAGER_H
