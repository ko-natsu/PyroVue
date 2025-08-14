#include "StatusManager.h"
#include <Arduino.h> // For isnan()

StatusManager::StatusManager(DisplayManager& displayMgr)
    : displayManager(displayMgr),
      currentState(STATE_NORMAL),
      previousState(STATE_NORMAL) {
}

void StatusManager::update(double tempCelsius, uint8_t fault) {
    // Determine the new state based on sensor readings
    SystemState newState = STATE_NORMAL; // Assume normal unless a fault is found
    if (fault) {
        if (fault & MAX31855_FAULT_OPEN) newState = STATE_FAULT_OPEN;
        else if (fault & MAX31855_FAULT_SHORT_GND) newState = STATE_FAULT_GND;
        else if (fault & MAX31855_FAULT_SHORT_VCC) newState = STATE_FAULT_VCC;
    } else if (isnan(tempCelsius)) {
        newState = STATE_FAULT_NAN;
    }

    // Update the internal state and trigger actions only if the state has changed
    if (newState != currentState) {
        previousState = currentState;
        currentState = newState;
        handleStateChange(newState);
    }

    // If we are in a normal state, continue to display the temperature
    if (currentState == STATE_NORMAL) {
        displayManager.displayTemperature(tempCelsius);
    }
}

void StatusManager::handleStateChange(SystemState newState) {
    // If we are recovering from any fault state back to normal
    if (newState == STATE_NORMAL && previousState != STATE_NORMAL) {
        displayManager.clearFullScreen();
        displayManager.displayNetworkStatus(0); // Redraw wifi status after clearing
    }

    // Display the specific error message for the new fault state
    switch (newState) {
        case STATE_FAULT_NAN:
            displayManager.displayError("NaN");
            break;
        case STATE_FAULT_OPEN:
            displayManager.displayError("FAULT: OPEN");
            break;
        case STATE_FAULT_GND:
            displayManager.displayError("FAULT: GND");
            break;
        case STATE_FAULT_VCC:
            displayManager.displayError("FAULT: VCC");
            break;
        case STATE_NORMAL:
            // No action needed here, handled by recovery logic and main update
            break;
    }
}

SystemState StatusManager::getCurrentState() {
    return currentState;
}
