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
#include "BLEDebugConsole.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "Esp32s3DACBT requires ESP32-S3"
#endif

void setup()
{
  Serial0.begin(115200);
  DebugConsole.begin("Esp32s3DACBT Serial");
  delay(1000);

  DebugConsole.println();
  DebugConsole.println("============================================");
  DebugConsole.println(" APB8202 / CW6638M UART DISCOVERY MODE");
  DebugConsole.println("============================================");
  DebugConsole.println(" PC console: Serial0 @ 115200");
  DebugConsole.println(" BT TX pin 5 -> GPIO18 RX");
  DebugConsole.println(" BT RX pin 6 -> GPIO17 TX");
  DebugConsole.println(" ADC audio: DISABLED");
  DebugConsole.println(" USB host:  DISABLED");
  DebugConsole.println();

  if (!APB8202Monitor::begin()) {
    DebugConsole.println("[FATAL] APB8202 UART monitor setup failed");
    while (true) yield();
  }
}

void loop()
{
  APB8202Monitor::update();
  DebugConsole.update();
  yield();
}
