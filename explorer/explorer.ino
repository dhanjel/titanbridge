#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

static BLEAddress treadmillAddr("c8:1f:c2:2a:90:40");
static bool doConnect = true;
static BLERemoteCharacteristic* pRemoteCharacteristic;

void setup() {
  Serial.begin(115200);
  uint32_t start = millis();
  while (!Serial && (millis() - start) < 5000); 
  
  Serial.println("--- Connecting to Treadmill ---");
  BLEDevice::init("");
}

void loop() {
  if (doConnect) {
    BLEClient*  pClient  = BLEDevice::createClient();
    Serial.println("Connecting...");
    
    if (pClient->connect(treadmillAddr)) {
      Serial.println("Connected!");
      
      std::map<std::string, BLERemoteService*>* pRemoteServices = pClient->getServices();
      for (auto &myPair : *pRemoteServices) {
        Serial.print("Service: ");
        Serial.println(myPair.second->getUUID().toString().c_str());
        
        std::map<std::string, BLERemoteCharacteristic*>* pChars = myPair.second->getCharacteristics();
        for (auto &charPair : *pChars) {
          Serial.print("  Characteristic: ");
          Serial.print(charPair.second->getUUID().toString().c_str());
          
          if (charPair.second->canRead()) Serial.print(" [READ]");
          if (charPair.second->canWrite()) Serial.print(" [WRITE]");
          if (charPair.second->canNotify()) Serial.print(" [NOTIFY]");
          Serial.println();
        }
      }
      pClient->disconnect();
      Serial.println("Disconnected.");
      doConnect = false;
    } else {
      Serial.println("Failed to connect.");
      delay(5000);
    }
  }
}
