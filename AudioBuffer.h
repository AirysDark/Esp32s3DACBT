#pragma once

#include <stdint.h>

struct StereoFrame {
  int16_t left;
  int16_t right;
};

namespace AudioBuffer {

uint32_t available();
void clear();

bool push(const StereoFrame &frame);
bool pop(StereoFrame &frame);

uint32_t droppedFrames();
uint32_t starvedFrames();

void incrementStarved();

} // namespace AudioBuffer
