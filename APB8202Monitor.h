#pragma once

#include <stdint.h>

namespace APB8202Monitor {

// Two-way raw UART discovery console for APB8202 / CW6638M.
// Nothing is transmitted automatically at boot. Use :autoscan for the
// automatic H4 HCI Reset baud sweep.
bool begin();
void update();

bool setBaud(uint32_t baud);
bool nextBaud();

uint32_t baudRate();
uint32_t bytesSeen();
uint32_t burstsSeen();

void clearCounters();

} // namespace APB8202Monitor
