#include "AudioBuffer.h"

#include <Arduino.h>

#include "ProjectConfig.h"

namespace {

static StereoFrame pcmRing[ProjectConfig::PCM_RING_FRAMES];
static volatile uint32_t pcmHead = 0;
static volatile uint32_t pcmTail = 0;

static volatile uint32_t adcDropped = 0;
static volatile uint32_t usbStarved = 0;

static portMUX_TYPE pcmMux = portMUX_INITIALIZER_UNLOCKED;

} // namespace

namespace AudioBuffer {

uint32_t available()
{
  uint32_t head;
  uint32_t tail;

  portENTER_CRITICAL(&pcmMux);
  head = pcmHead;
  tail = pcmTail;
  portEXIT_CRITICAL(&pcmMux);

  return (head - tail) & ProjectConfig::PCM_RING_MASK;
}

void clear()
{
  portENTER_CRITICAL(&pcmMux);
  pcmTail = pcmHead;
  portEXIT_CRITICAL(&pcmMux);
}

bool push(const StereoFrame &frame)
{
  bool ok = false;

  portENTER_CRITICAL(&pcmMux);

  const uint32_t next =
      (pcmHead + 1) & ProjectConfig::PCM_RING_MASK;

  if (next != pcmTail) {
    pcmRing[pcmHead] = frame;
    pcmHead = next;
    ok = true;
  } else {
    adcDropped++;
  }

  portEXIT_CRITICAL(&pcmMux);

  return ok;
}

bool pop(StereoFrame &frame)
{
  bool ok = false;

  portENTER_CRITICAL(&pcmMux);

  if (pcmTail != pcmHead) {
    frame = pcmRing[pcmTail];
    pcmTail = (pcmTail + 1) & ProjectConfig::PCM_RING_MASK;
    ok = true;
  }

  portEXIT_CRITICAL(&pcmMux);

  return ok;
}

uint32_t droppedFrames()
{
  uint32_t value;

  portENTER_CRITICAL(&pcmMux);
  value = adcDropped;
  portEXIT_CRITICAL(&pcmMux);

  return value;
}

uint32_t starvedFrames()
{
  uint32_t value;

  portENTER_CRITICAL(&pcmMux);
  value = usbStarved;
  portEXIT_CRITICAL(&pcmMux);

  return value;
}

void incrementStarved()
{
  portENTER_CRITICAL(&pcmMux);
  usbStarved++;
  portEXIT_CRITICAL(&pcmMux);
}

} // namespace AudioBuffer
