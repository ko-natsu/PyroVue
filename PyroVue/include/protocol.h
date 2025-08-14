#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

struct TempSample {
  uint32_t ms;
  float t_c;
  uint8_t fault;
};

#endif // PROTOCOL_H
