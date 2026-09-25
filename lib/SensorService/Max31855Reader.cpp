#include "Max31855Reader.h"

#if defined(ARDUINO)

#include <Arduino.h>
#include <SPI.h>

#include "protocol.h"

// Platform SPI bus selection. The MAX31855 sits on the general-purpose
// SPI controller, not the flash bus:
//  - ESP32 family (incl. ESP32-S3): instantiate the user FSPI controller
//    with the thermocouple pins; the global SPI object may be owned by
//    another device.
//  - ESP8266: only the global hardware SPI object exists (HSPI).
#if defined(ARDUINO_ARCH_ESP32)
#if defined(FSPI)
static SPIClass thermSpi(FSPI);
#else
static SPIClass thermSpi(SPI2_HOST);
#endif
static SPIClass& bus() { return thermSpi; }
#else
static SPIClass& bus() { return SPI; }
#endif

namespace {

constexpr uint32_t kMax31855Hz = 4000000;  // Chip limit is 5 MHz.
constexpr uint32_t kFaultBit = 1UL << 16;
constexpr uint8_t kOpenBit = 1 << 0;
constexpr uint8_t kShortGndBit = 1 << 1;
constexpr uint8_t kShortVccBit = 1 << 2;

}  // namespace

Max31855Reader::Max31855Reader(uint8_t csPin)
    : csPin_(csPin), started_(false) {}

void Max31855Reader::begin() {
  pinMode(csPin_, OUTPUT);
  digitalWrite(csPin_, HIGH);  // Deassert: conversion output is latched on /CS falling edge.

#if defined(ARDUINO_ARCH_ESP32)
  bus().begin(THERMOCOUPLE_SCK, THERMOCOUPLE_MISO, THERMOCOUPLE_MOSI, -1);
  bus().beginTransaction(SPISettings(kMax31855Hz, MSBFIRST, SPI_MODE0));
  bus().endTransaction();
#else
  bus().begin();
  bus().setBitOrder(MSBFIRST);
  bus().setDataMode(SPI_MODE0);
  bus().setFrequency(kMax31855Hz);
#endif

  started_ = true;
}

RawReading Max31855Reader::read() {
  RawReading reading;
  reading.tempC = 0.0f;
  reading.fault = 0;
  reading.valid = false;

  if (!started_) {
    reading.fault = SENSOR_FAULT_NAN;
    return reading;
  }

  uint32_t raw = readRaw();

  // All-zeros / all-ones means the chip never drove the bus.
  if (raw == 0x00000000UL || raw == 0xFFFFFFFFUL) {
    reading.fault = SENSOR_FAULT_NAN;
    return reading;
  }

  if (raw & kFaultBit) {
    if (raw & kOpenBit) reading.fault |= SENSOR_FAULT_OPEN;
    if (raw & kShortGndBit) reading.fault |= SENSOR_FAULT_SHORT_GND;
    if (raw & kShortVccBit) reading.fault |= SENSOR_FAULT_SHORT_VCC;
    return reading;  // Chip fault: no trustworthy temperature.
  }

  // 14-bit signed thermocouple temperature, 0.25 C resolution.
  int32_t t14 = static_cast<int32_t>((raw >> 18) & 0x3FFFUL);
  if (t14 & 0x2000) {
    t14 -= 0x4000;
  }
  reading.tempC = static_cast<float>(t14) * 0.25f;
  reading.valid = true;
  return reading;
}

uint32_t Max31855Reader::readRaw() {
#if defined(ARDUINO_ARCH_ESP32)
  bus().beginTransaction(SPISettings(kMax31855Hz, MSBFIRST, SPI_MODE0));
#endif
  digitalWrite(csPin_, LOW);

  uint32_t raw = 0;
  for (uint8_t i = 0; i < 4; ++i) {
    raw = (raw << 8) | static_cast<uint32_t>(bus().transfer(0x00));
  }

  digitalWrite(csPin_, HIGH);
#if defined(ARDUINO_ARCH_ESP32)
  bus().endTransaction();
#endif
  return raw;
}

#endif  // ARDUINO
