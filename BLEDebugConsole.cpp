#include "BLEDebugConsole.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

namespace {
BLECharacteristic *txCharacteristic = nullptr;
volatile bool bleConnected = false;
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;
static const size_t RX_SIZE = 512;
uint8_t rxBuffer[RX_SIZE];
volatile size_t rxHead = 0, rxTail = 0;

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*) override { bleConnected = true; }
  void onDisconnect(BLEServer *server) override {
    bleConnected = false;
    server->getAdvertising()->start();
  }
};

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *ch) override {
    std::string value = ch->getValue();
    portENTER_CRITICAL(&rxMux);
    for (size_t i=0; i<value.size(); ++i) {
      size_t next = (rxHead + 1) % RX_SIZE;
      if (next == rxTail) break;
      rxBuffer[rxHead] = (uint8_t)value[i];
      rxHead = next;
    }
    portEXIT_CRITICAL(&rxMux);
  }
};
}

BLEDebugConsole DebugConsole;

bool BLEDebugConsole::begin(const char *deviceName)
{
  BLEDevice::init(deviceName);
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService *service = server->createService("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
  txCharacteristic = service->createCharacteristic(
      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E",
      BLECharacteristic::PROPERTY_NOTIFY);
  txCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic *rx = service->createCharacteristic(
      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E",
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new RxCallbacks());

  service->start();
  BLEAdvertising *advertising = server->getAdvertising();
  advertising->addServiceUUID("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
  advertising->start();
  return true;
}

void BLEDebugConsole::update() {}

bool BLEDebugConsole::connected() const { return bleConnected; }

size_t BLEDebugConsole::write(uint8_t c) { return write(&c, 1); }

size_t BLEDebugConsole::write(const uint8_t *buffer, size_t size)
{
  if (!bleConnected || !txCharacteristic || !size) return size;

  size_t offset = 0;
  while (offset < size) {
    size_t n = size - offset;
    if (n > 20) n = 20;
    txCharacteristic->setValue((uint8_t *)(buffer + offset), n);
    txCharacteristic->notify();
    offset += n;
    delay(1);
  }
  return size;
}

int BLEDebugConsole::available()
{
  portENTER_CRITICAL(&rxMux);
  size_t n = (rxHead + RX_SIZE - rxTail) % RX_SIZE;
  portEXIT_CRITICAL(&rxMux);
  return (int)n;
}

int BLEDebugConsole::read()
{
  portENTER_CRITICAL(&rxMux);
  if (rxTail == rxHead) {
    portEXIT_CRITICAL(&rxMux);
    return -1;
  }
  uint8_t c = rxBuffer[rxTail];
  rxTail = (rxTail + 1) % RX_SIZE;
  portEXIT_CRITICAL(&rxMux);
  return c;
}

int BLEDebugConsole::peek()
{
  portENTER_CRITICAL(&rxMux);
  int v = (rxTail == rxHead) ? -1 : rxBuffer[rxTail];
  portEXIT_CRITICAL(&rxMux);
  return v;
}

void BLEDebugConsole::flush() {}
