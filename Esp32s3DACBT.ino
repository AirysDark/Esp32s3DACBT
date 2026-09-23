/*
  Esp32s3DACBT - APB8202 / CW6638M UART discovery build
  Core target: Arduino-ESP32 2.0.17

  PC debug console:
    Serial0 / UART0 at 115200 baud

  Bluetooth UART:
    APB pin 5 TXD -> ESP32-S3 GPIO18 RX
    APB pin 6 RXD -> ESP32-S3 GPIO17 TX

  ADC audio and USB host are deliberately disabled in this discovery build.
*/

#include <Arduino.h>
#include "APB8202Monitor.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "Esp32s3DACBT requires ESP32-S3"
#endif

void setup()
{
  Serial0.begin(115200);
  delay(1000);
  delay(1000);

  Serial0.println();
  Serial0.println("============================================");
  Serial0.println(" APB8202 / CW6638M UART DISCOVERY MODE");
  Serial0.println("============================================");
  Serial0.println(" PC console: Serial0 @ 115200");
  Serial0.println(" BT TX pin 5 -> GPIO18 RX");
  Serial0.println(" BT RX pin 6 -> GPIO17 TX");
  Serial0.println(" ADC audio: DISABLED");
  Serial0.println(" USB host:  DISABLED");
  Serial0.println();

  if (!APB8202Monitor::begin()) {
    Serial0.println("[FATAL] APB8202 UART monitor setup failed");
    while (true) yield();
  }
}

void loop()
{
  APB8202Monitor::update();
  Serial0.update();
  yield();
}
