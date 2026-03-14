#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

void setup() {
  Serial.begin(115200);
  uint32_t start = millis();
  while (!Serial && (millis() - start) < 5000); 
  
  Serial.println("--- Standard BLE Scanner Starting ---");

  BLEDevice::init("");
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
}

void loop() {
  Serial.println("Scanning...");
  BLEScan* pBLEScan = BLEDevice::getScan();
  BLEScanResults *foundDevices = pBLEScan->start(5, false);
  Serial.print("Devices found: ");
  Serial.println(foundDevices->getCount());
  for (int i = 0; i < foundDevices->getCount(); i++) {
    BLEAdvertisedDevice device = foundDevices->getDevice(i);
    Serial.print("["); Serial.print(i); Serial.print("] ");
    Serial.print("Name: "); Serial.print(device.getName().c_str());
    Serial.print(", RSSI: "); Serial.print(device.getRSSI());
    Serial.print(", Addr: "); Serial.println(device.getAddress().toString().c_str());
  }
  pBLEScan->clearResults();
  delay(1000);
}
