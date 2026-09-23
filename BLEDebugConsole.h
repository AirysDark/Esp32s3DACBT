#pragma once
#include <Arduino.h>

class BLEDebugConsole : public Stream {
public:
  bool begin(const char *deviceName);
  void update();
  bool connected() const;
  size_t write(uint8_t c) override;
  size_t write(const uint8_t *buffer, size_t size) override;
  int available() override;
  int read() override;
  int peek() override;
  void flush() override;
  using Print::write;
};

extern BLEDebugConsole DebugConsole;
