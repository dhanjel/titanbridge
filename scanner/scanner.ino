#include <NimBLEDevice.h>

void setup() {
  Serial.begin(115200);
  uint32_t start = millis();
  while (!Serial && (millis() - start) < 5000); 
  
  delay(1000);
  Serial.println("--- TitanBridge Aggressive Scanner Starting ---");

  NimBLEDevice::init("");
  NimBLEScan* pScan = NimBLEDevice::getScan();
  
  // Try passive scan and 100% duty cycle
  pScan->setActiveScan(false); 
  pScan->setInterval(100);
  pScan->setWindow(100);
}

void loop() {
  NimBLEScan* pScan = NimBLEDevice::getScan();
  Serial.println("Scanning 5s...");
  
  pScan->start(5, false);
  NimBLEScanResults results = pScan->getResults();

  Serial.printf("Found %d devices:\n", results.getCount());
  for (int i = 0; i < results.getCount(); i++) {
    const NimBLEAdvertisedDevice* device = results.getDevice(i);
    Serial.printf("[%d] Name: %s, RSSI: %d, UUID: %s, Addr: %s\n", 
      i,
      device->haveName() ? device->getName().c_str() : "<no name>",
      device->getRSSI(),
      device->haveServiceUUID() ? device->getServiceUUID().toString().c_str() : "<none>",
      device->getAddress().toString().c_str()
    );
  }
  
  pScan->clearResults();
  delay(500);
}
