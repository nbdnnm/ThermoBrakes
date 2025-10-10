#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "PacketIdInfo.h"
#include <max6675.h>
#include "Arduino.h"
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);

int SCK1 = 13;
int CS1 = 14;
int SO1 = 26;
MAX6675 module1(SCK1, CS1, SO1);

int SCK2 = 13;
int CS2 = 27;
int SO2 = 26;
MAX6675 module2(SCK2, CS2, SO2);

int BRAKES_TEMP_THRESHOLD = 650;

#define SERVICE_UUID "01217d39-9bd7-48bf-982d-a74922484e91"
#define CANBUS_MAIN_UUID "0001"
#define CANBUS_FILTER_UUID "0002"

BLEServer *pServer;
BLEAdvertising *pAdvertising;

BLECharacteristic *canBusMainCharacteristic;
BLECharacteristic *canBusFilterCharacteristic;

PacketIdInfo canBusPacketIdInfo;
bool canBusAllowUnknownPackets = true;
uint32_t canBusLastNotifyMs = 0;
boolean isCanBusConnected = false;

uint8_t tempData[20];
unsigned long lastSendTime = 0;
unsigned long lastNotifyTime = 0;
const long sendInterval = 300; // 3 Hz, the MAX6675 fails to update more frequently

class FilterCallback : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pCharacteristic)
  {
    String value = String(pCharacteristic->getValue());
    if (value.length() < 1)
      return;
    uint8_t command = value[0];
    switch (command)
    {
    case 0x00: // DENY_ALL
      if (value.length() == 1)
      {
        canBusPacketIdInfo.reset();
        canBusAllowUnknownPackets = false;
      }
      break;
    case 0x01: // ALLOW_ALL
      if (value.length() == 3)
      {
        canBusPacketIdInfo.reset();
        canBusPacketIdInfo.setDefaultNotifyInterval(sendInterval);
        canBusAllowUnknownPackets = true;
      }
      break;
    case 0x02: // ADD_PID
      if (value.length() == 7)
      {
        uint16_t notifyIntervalMs = value[1] << 8 | value[2];
        uint32_t pid = value[3] << 24 | value[4] << 16 | value[5] << 8 | value[6];
        canBusPacketIdInfo.setNotifyInterval(pid, sendInterval);
      }
      break;
    }
  }
};

void startAdvertising()
{
  pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinInterval(0x20); // 20 ms
  pAdvertising->setMaxInterval(0x40); // 40 ms
  pAdvertising->start();
}

void startRC()
{
  BLEDevice::init("ThermoBrakes");
  pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(SERVICE_UUID);
  canBusMainCharacteristic = pService->createCharacteristic(
      CANBUS_MAIN_UUID,
      BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ);
  canBusMainCharacteristic->addDescriptor(new BLE2902());
  canBusFilterCharacteristic = pService->createCharacteristic(
      CANBUS_FILTER_UUID,
      BLECharacteristic::PROPERTY_WRITE);
  canBusFilterCharacteristic->setCallbacks(new FilterCallback());
  pService->start();
  startAdvertising();
}

void setup()
{
  Serial.begin(115200);
  while (!Serial);
  startRC();
  Serial.println("Started");
  lcd.init();
  lcd.clear();
  lcd.backlight();
}

void sendTemperatureForModule(uint32_t packetId, MAX6675 module)
{
  PacketIdInfoItem *infoItem;
  int temperature = (int)module.readCelsius();
  uint8_t temperatureHigh = (temperature >> 8) & 0xFF;
  uint8_t temperatureLow = temperature & 0xFF;
  tempData[4] = temperatureHigh;
  tempData[5] = temperatureLow;
  ((uint32_t *)tempData)[0] = packetId;
  infoItem = canBusPacketIdInfo.findItem(packetId, canBusAllowUnknownPackets);
  if (infoItem && infoItem->shouldNotify())
  {
    Serial.println("Notifying");
    canBusMainCharacteristic->setValue(tempData, 6);
    canBusMainCharacteristic->notify();
    infoItem->markNotified();
  }
}

void sendTempData()
{
  unsigned long currentTime = millis();

  if (currentTime - lastNotifyTime < sendInterval)
    return;
  lastNotifyTime = currentTime;

  sendTemperatureForModule(1, module1);
  sendTemperatureForModule(2, module2);
}

void displayTemperature()
{
  lcd.clear();

  int temperature1 = (int)module1.readCelsius();
  int temperature2 = (int)module2.readCelsius();

  lcd.setCursor(0, 0);
  lcd.print("FL: ");
  lcd.print(temperature1);

  lcd.setCursor(8, 0);
  lcd.print("FR: ");
  lcd.print(temperature2);

  if ((temperature1 >= BRAKES_TEMP_THRESHOLD && temperature1 < INT_MAX) || (temperature2 >= BRAKES_TEMP_THRESHOLD && temperature2 < INT_MAX))
  {
    lcd.noBacklight();
    delay(200);
    lcd.backlight();
  }
}

void loop()
{
  unsigned long currentTime = millis();
  if (currentTime - lastSendTime >= sendInterval)
  {
    lastSendTime = currentTime;
    if (isCanBusConnected)
    {
      sendTempData();
    }
    displayTemperature();
  }

  if (!isCanBusConnected && pServer->getConnectedCount() > 0)
  {
    isCanBusConnected = true;
    Serial.println("BLE connected");
    canBusPacketIdInfo.reset();
  }
  else if (isCanBusConnected && pServer->getConnectedCount() == 0)
  {
    isCanBusConnected = false;
    Serial.println("BLE disconnected");
    pAdvertising->stop();
    delay(100);
    startAdvertising();
  }
}
