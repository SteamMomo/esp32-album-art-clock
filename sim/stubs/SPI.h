// SPI stand-in. Bytes sent on the default bus go to the simulated ILI9341.
#pragma once
#include <Arduino.h>

#define SPI_MODE0 0
#define SPI_MODE3 3
#define MSBFIRST 1
#define VSPI 3
#define HSPI 2

struct SPISettings {
  SPISettings() {}
  SPISettings(uint32_t, uint8_t, uint8_t) {}
};

namespace sim {
void onSpiByte(uint8_t value);
}

class SPIClass {
 public:
  explicit SPIClass(int bus = VSPI) : display_(bus == VSPI) {}
  void begin(int8_t = -1, int8_t = -1, int8_t = -1, int8_t = -1) {}
  void end() {}
  void beginTransaction(SPISettings) {}
  void endTransaction() {}
  void setFrequency(uint32_t) {}
  void setHwCs(bool) {}
  void setDataMode(uint8_t) {}
  void setBitOrder(uint8_t) {}
  uint8_t transfer(uint8_t value) {
    if (display_) sim::onSpiByte(value);
    return 0;
  }
  uint16_t transfer16(uint16_t value) {
    transfer(value >> 8);
    transfer(value & 0xFF);
    return 0;
  }
  void transfer(void* data, uint32_t size) {
    uint8_t* bytes = static_cast<uint8_t*>(data);
    while (size--) transfer(*bytes++);
  }
  void write(uint8_t v) { transfer(v); }
  void write16(uint16_t v) { transfer16(v); }
  void write32(uint32_t v) { transfer16(v >> 16); transfer16(v); }
  void writeBytes(const uint8_t* data, uint32_t size) {
    while (size--) transfer(*data++);
  }

 private:
  bool display_;
};
extern SPIClass SPI;
