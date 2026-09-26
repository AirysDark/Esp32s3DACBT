/*
  Esp32s3DACBT - passive APB8202 boot signal capture
  Core target: Arduino-ESP32 2.0.17

  Serial Monitor:
    Serial0 / UART0 at 115200 baud

  APB observation:
    APB pin 5 -> ESP32-S3 GPIO18
    APB pin 6 / ESP32 GPIO17 is NOT driven

  IMPORTANT: this build never starts Serial1 and never transmits to the APB.
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
  Serial0.println(" APB8202 PASSIVE BOOT CAPTURE");
  Serial0.println(" Arduino-ESP32 Core 2.0.17");
  Serial0.println("============================================");
  Serial0.println(" GPIO18: passive input from APB pin 5");
  Serial0.println(" GPIO17: input/high-Z; NEVER transmitted");
  Serial0.println(" Serial1: NOT started");
  Serial0.println();
  Serial0.println("Leave ESP32 powered. Switch APB power OFF,");
  Serial0.println("then ON. Boot transitions print below.");
  Serial0.println();

  APB8202Monitor::begin();
}

void loop()
{
  APB8202Monitor::update();
  yield();
}
