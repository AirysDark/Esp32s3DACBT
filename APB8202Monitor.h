#pragma once

#include <stdint.h>

namespace APB8202Monitor {

// Passive RX-only monitor for the APB8202 / CW6638M UART.
// No bytes are transmitted to the module.
bool begin();
void update();

// Change only the ESP32 receive baud. Useful while characterizing the unknown
// APB8202 UART/HCI firmware.
bool setBaud(uint32_t baud);
bool nextBaud();

uint32_t baudRate();
uint32_t bytesSeen();
uint32_t burstsSeen();

void clearCounters();

} // namespace APB8202Monitor
