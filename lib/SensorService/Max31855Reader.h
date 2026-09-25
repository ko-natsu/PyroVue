#ifndef MAX31855_READER_H
#define MAX31855_READER_H

// Arduino-only adapter. The portable core (SensorService.h) never includes
// this file; native builds and tests use their own fake ISensorReader.
#if defined(ARDUINO)

#include <stdint.h>

#include "SensorService.h"

// Hardware-SPI MAX31855 front-end implementing ISensorReader.
// Uses the SPI pins from config.h (THERMOCOUPLE_SCK/MISO/CS); MOSI is
// unused by the MAX31855 (read-only device) and left unconfigured.
//
// read() issues one 32-bit SPI transaction at ~4 MHz (within the
// MAX31855's 5 MHz SCK limit) and decodes the thermocouple temperature
// and fault bits into a RawReading. It is non-blocking and allocation-free.
class Max31855Reader : public ISensorReader {
 public:
  explicit Max31855Reader(uint8_t csPin = THERMOCOUPLE_CS);

  // Configures CS as output (deasserted) and starts the platform SPI bus.
  void begin();

  RawReading read() override;

 private:
  uint32_t readRaw();

  uint8_t csPin_;
  bool started_;
};

#endif  // ARDUINO

#endif  // MAX31855_READER_H
