#pragma once

#include <stdint.h>

namespace APB8202Monitor {

// Passive boot-signal capture for APB8202 / CW6638M.
// GPIO18 is input-only. GPIO17 is input/high-Z.
// Serial1 is never started and nothing is transmitted to the APB.
bool begin();
void update();

} // namespace APB8202Monitor
