/*
  Esp32s3DACBT - APB8202 automatic passive boot capture
  Core target: Arduino-ESP32 2.0.17

  GPIO18 observes APB physical pin 5.
  GPIO17 observes APB physical pin 6.
  GPIO8 controls the APB power N-MOSFET.

  Serial1 is never started and GPIO17/18 are never driven.
*/

#include <Arduino.h>
#include "APB8202Monitor.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "Esp32s3DACBT requires ESP32-S3"
#endif

void setup()
{
  Serial0.begin(115200);
  delay(1500);

  Serial0.println();
  Serial0.println("============================================");
  Serial0.println(" APB8202 AUTO POWER + PASSIVE SIGNAL SCAN");
  Serial0.println(" Arduino-ESP32 Core 2.0.17");
  Serial0.println("============================================");
  Serial0.println(" GPIO18 <- APB physical pin 5 (INPUT only)");
  Serial0.println(" GPIO17 <- APB physical pin 6 (INPUT only)");
  Serial0.println(" GPIO8  -> IRLZ44N Pin 1 (Gate)");
  Serial0.println(" IRLZ44N Pin 1 -> 10k -> common GND");
  Serial0.println(" Serial1: NOT started");
  Serial0.println();

  APB8202Monitor::begin();
}

void loop()
{
  APB8202Monitor::update();
  yield();
}
