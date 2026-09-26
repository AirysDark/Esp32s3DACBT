#include "APB8202Monitor.h"

#include <Arduino.h>
#include "ProjectConfig.h"

namespace {

static const int kCapturePin = 18;
static const int kNoDrivePin = 17;

static const size_t kMaxEdges = 8192;
static volatile uint32_t edgeTimeUs[kMaxEdges];
static volatile uint8_t edgeLevel[kMaxEdges];
static volatile size_t edgeCount = 0;

static size_t printedEdges = 0;
static bool captureFullPrinted = false;
static uint32_t lastEdgeUs = 0;

void IRAM_ATTR captureISR()
{
  size_t i = edgeCount;
  if (i >= kMaxEdges) return;

  edgeTimeUs[i] = micros();
  edgeLevel[i] = (uint8_t)digitalRead(kCapturePin);
  edgeCount = i + 1;
}

void printNewEdges()
{
  size_t available;

  noInterrupts();
  available = edgeCount;
  interrupts();

  while (printedEdges < available) {
    const size_t i = printedEdges;

    uint32_t t;
    uint8_t level;
    noInterrupts();
    t = edgeTimeUs[i];
    level = edgeLevel[i];
    interrupts();

    uint32_t dt = (printedEdges == 0) ? 0 : (t - lastEdgeUs);
    lastEdgeUs = t;

    Serial0.printf("[BOOT] #%u t=%lu us dt=%lu us level=%s\n",
                   (unsigned)i,
                   (unsigned long)t,
                   (unsigned long)dt,
                   level ? "HIGH" : "LOW");
    ++printedEdges;
  }

  if (available >= kMaxEdges && !captureFullPrinted) {
    captureFullPrinted = true;
    Serial0.println("[BOOT] Capture buffer FULL (8192 transitions).");
    Serial0.println("[BOOT] Power-cycle the ESP32 to clear the capture.");
  }
}

} // namespace

namespace APB8202Monitor {

bool begin()
{
  // Critical isolation rule: neither APB-connected ESP32 pin is driven.
  // Do not call Serial1.begin(), pinMode OUTPUT, or enable pull resistors.
  pinMode(kCapturePin, INPUT);
  pinMode(kNoDrivePin, INPUT);

  edgeCount = 0;
  printedEdges = 0;
  captureFullPrinted = false;
  lastEdgeUs = 0;

  attachInterrupt(digitalPinToInterrupt(kCapturePin), captureISR, CHANGE);

  Serial0.println("[BOOT] Passive capture ARMED.");
  Serial0.printf("[BOOT] GPIO%d initial level: %s\n",
                 kCapturePin, digitalRead(kCapturePin) ? "HIGH" : "LOW");
  Serial0.println("[BOOT] Turn APB power ON now.");
  return true;
}

void update()
{
  printNewEdges();
}

} // namespace APB8202Monitor
