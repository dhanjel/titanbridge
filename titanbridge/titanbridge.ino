#ifndef ZIGBEE_MODE_ED
#error "Select Tools -> Zigbee Mode -> Zigbee ED (end device)"
#endif

#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "Zigbee.h"
#include "ep/ZigbeeWindSpeedSensor.h"
#include "ep/ZigbeeOccupancySensor.h"

// ── CONFIG ────────────────────────────────────────────────────────────────
// Update TREADMILL_ADDR to match your treadmill's BLE MAC address.
// Use the bundled explorer/ or scanner/ sketch to discover it.
static const char* TREADMILL_ADDR = "c8:1f:c2:2a:90:40";
static NimBLEUUID  ftmsServiceUUID("1826");      // Fitness Machine Service
static NimBLEUUID  treadmillDataUUID("2ad3");    // Treadmill Data characteristic
static NimBLEUUID  ftmsStatusUUID("2ada");       // Fitness Machine Status

#define RSC_SERVICE_UUID      "1814"
#define RSC_MEASUREMENT_UUID  "2a53"
#define RSC_FEATURE_UUID      "2a54"
#define RSC_LOCATION_UUID     "2a5d"
#define RSC_CTRL_POINT_UUID   "2a55"  // SC Control Point — required by Garmin

#define TREADMILL_RETRY_MS   5000   // ms between treadmill reconnect attempts
#define KEEPALIVE_MS         1000   // ms between RSC keep-alive notifications

// XIAO ESP32-C6: built-in LED is on pin 15, active LOW (no RGB)

// ── ZIGBEE ENDPOINTS ──────────────────────────────────────────────────────
// EP 10: speed   → Wind Speed cluster (0x040b, 0.01 m/s)      — ZHA: sensor
// EP 11: running → Occupancy cluster  (0x0406, bool)           — ZHA: binary_sensor
// EP 12: garmin  → Occupancy cluster  (0x0406, bool)           — ZHA: binary_sensor
ZigbeeWindSpeedSensor  zbSpeed(10);
ZigbeeOccupancySensor  zbRunning(11);
ZigbeeOccupancySensor  zbGarmin(12);

// ── GLOBAL STATE ──────────────────────────────────────────────────────────
NimBLEServer*         pServer          = nullptr;
NimBLECharacteristic* pRSCMeasurement  = nullptr;
NimBLECharacteristic* pSCCtrl          = nullptr;
NimBLEClient*         pTreadmillClient = nullptr;

volatile bool deviceConnected    = false;
volatile bool treadmillConnected = false;

SemaphoreHandle_t dataMutex;
float    currentSpeed_mps  = 0.0f;
uint8_t  currentCadence    = 0;
float    totalDistance_m   = 0.0f;
unsigned long lastDataMs   = 0;  // millis() of last treadmill data packet

unsigned long lastNotifyTime = 0;
bool zbInitialReported = false;  // true once initial Zigbee state has been pushed

// ── SC CONTROL POINT CALLBACK ─────────────────────────────────────────────
class SCControlPointCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
        if (!pSCCtrl) return;
        const uint8_t* data = pChar->getValue().data();
        size_t         len  = pChar->getValue().length();
        if (len < 1) return;

        uint8_t opcode = data[0];

        if (opcode == 0x04) {
            // Request Supported Sensor Locations → Success + Right Shoe
            uint8_t locResp[4] = {0x10, 0x04, 0x01, 0x06};
            pSCCtrl->setValue(locResp, sizeof(locResp));
        } else {
            // All other opcodes → Op Code Not Supported
            uint8_t resp[3] = {0x10, opcode, 0x02};
            pSCCtrl->setValue(resp, sizeof(resp));
        }
        pSCCtrl->indicate();
        Serial.printf("SC opcode=0x%02x\n", opcode);
    }
};

// ── RSC NOTIFICATION ─────────────────────────────────────────────────────
void sendRSCUpdate() {
    if (!deviceConnected) return;

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    float    spd     = currentSpeed_mps;
    uint8_t  cad     = currentCadence;
    uint32_t dist_dm = (uint32_t)(totalDistance_m * 10.0f);  // units: 1/10 m
    xSemaphoreGive(dataMutex);

    // Bit 1 (0x02) = Total Distance Present, Bit 2 (0x04) = Running (vs Walking)
    uint8_t  flags     = 0x02 | ((spd > 0) ? 0x04 : 0x00);
    uint16_t speed_rsc = (uint16_t)(spd * 256.0f);  // units: 1/256 m/s

    // RSC Measurement: flags(1) + speed(2) + cadence(1) + total_distance(4) = 8 bytes
    uint8_t rscData[8] = {
        flags,
        (uint8_t)(speed_rsc & 0xFF),
        (uint8_t)((speed_rsc >> 8) & 0xFF),
        cad,
        (uint8_t)(dist_dm & 0xFF),
        (uint8_t)((dist_dm >> 8) & 0xFF),
        (uint8_t)((dist_dm >> 16) & 0xFF),
        (uint8_t)((dist_dm >> 24) & 0xFF),
    };

    pRSCMeasurement->setValue(rscData, sizeof(rscData));
    pRSCMeasurement->notify();
    lastNotifyTime = millis();
}

// ── ZIGBEE PUSH ───────────────────────────────────────────────────────────
void pushZigbee(float spd_mps, uint8_t cadence, float dist_m, bool running) {
    if (!Zigbee.connected()) return;

    zbSpeed.setWindSpeed(spd_mps);   // setWindSpeed() takes m/s, converts internally
    zbSpeed.reportWindSpeed();

    zbRunning.setOccupancy(running);
    zbRunning.report();
}

// ── SPEED PACKET HANDLER ─────────────────────────────────────────────────
void handleSpeedRaw(uint16_t speed_raw) {
    float   speed_kmh = speed_raw / 100.0f;
    float   spd_mps   = speed_kmh / 3.6f;
    uint8_t cadence   = (speed_raw > 0) ? (uint8_t)(140 + speed_kmh * 3.0f) : 0;

    unsigned long now = millis();
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (lastDataMs > 0 && currentSpeed_mps > 0)
        totalDistance_m += currentSpeed_mps * ((now - lastDataMs) / 1000.0f);
    currentSpeed_mps = spd_mps;
    currentCadence   = cadence;
    lastDataMs       = now;
    float dist_snap  = totalDistance_m;
    xSemaphoreGive(dataMutex);

    sendRSCUpdate();
    pushZigbee(spd_mps, cadence, dist_snap, speed_raw > 0);
}

// ── TREADMILL DATA CALLBACK ───────────────────────────────────────────────
void notifyCallback(NimBLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
    if (length < 2) return;

    uint16_t flags = (uint16_t)pData[0] | ((uint16_t)pData[1] << 8);

    // Log every packet for diagnostics
    Serial.printf("T data len=%d flags=%04x:", length, flags);
    for (size_t i = 0; i < length && i < 8; i++) Serial.printf(" %02x", pData[i]);
    Serial.println();

    // Bit 0 (More Data): when set, instantaneous speed is absent from this packet.
    // The treadmill sends multi-packet updates; speed is in the final packet (bit 0 = 0).
    if (flags & 0x0001) return;

    if (length < 4) return;

    // Speed is at bytes 2-3 when More Data bit is clear (0.01 km/h units)
    uint16_t speed_raw = (uint16_t)pData[2] | ((uint16_t)pData[3] << 8);
    Serial.printf("spd=%.2fkmh\n", speed_raw / 100.0f);
    handleSpeedRaw(speed_raw);
}

// ── TREADMILL CLIENT CALLBACKS ────────────────────────────────────────────
class TreadmillClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* pClient) override {
        Serial.println("T+");
    }
    void onDisconnect(NimBLEClient* pClient, int reason) override {
        treadmillConnected = false;
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        currentSpeed_mps = 0.0f;
        currentCadence   = 0;
        lastDataMs       = 0;
        xSemaphoreGive(dataMutex);
        // Mark client for recreation — stale client causes reconnect failures
        pTreadmillClient = nullptr;
        Serial.printf("T- reason=%d\n", reason);
    }
};

// ── GARMIN SERVER CALLBACKS ───────────────────────────────────────────────
class GarminServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pSrv, NimBLEConnInfo& connInfo) override {
        deviceConnected = true;
        if (Zigbee.connected()) { zbGarmin.setOccupancy(true); zbGarmin.report(); }
        Serial.println("C");
    }
    void onDisconnect(NimBLEServer* pSrv, NimBLEConnInfo& connInfo, int reason) override {
        deviceConnected = false;
        // Keep treadmill connected so Zigbee keeps reporting while Garmin is away
        if (Zigbee.connected()) { zbGarmin.setOccupancy(false); zbGarmin.report(); }
        Serial.printf("D reason=%d\n", reason);
        NimBLEDevice::startAdvertising();
    }
    void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
        Serial.printf("auth encrypted=%d\n", connInfo.isEncrypted());
    }
};

// ── TREADMILL TASK ────────────────────────────────────────────────────────
void treadmillTask(void* param) {
    for (;;) {
        if (treadmillConnected) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // If Garmin is connected, give it time to finish GATT discovery first
        if (deviceConnected) vTaskDelay(pdMS_TO_TICKS(3000));

        // Pause advertising during connect() so Garmin doesn't collide with it
        NimBLEDevice::stopAdvertising();

        Serial.println("T?");

        if (!pTreadmillClient) {
            pTreadmillClient = NimBLEDevice::createClient();
            pTreadmillClient->setClientCallbacks(new TreadmillClientCallbacks(), true);
            pTreadmillClient->setConnectionParams(12, 12, 0, 51);
        }

        NimBLEAddress addr(TREADMILL_ADDR, BLE_ADDR_PUBLIC);
        if (!pTreadmillClient->connect(addr)) {
            Serial.println("T! connect failed");
            NimBLEDevice::startAdvertising();
            vTaskDelay(pdMS_TO_TICKS(TREADMILL_RETRY_MS));
            continue;
        }

        // Treadmill connected — resume advertising so Garmin can connect
        NimBLEDevice::startAdvertising();

        NimBLERemoteService* pSvc = pTreadmillClient->getService(ftmsServiceUUID);
        if (!pSvc) {
            Serial.println("T! no FTMS service");
            pTreadmillClient->disconnect();
            vTaskDelay(pdMS_TO_TICKS(TREADMILL_RETRY_MS));
            continue;
        }

        NimBLERemoteCharacteristic* pChar = pSvc->getCharacteristic(treadmillDataUUID);
        if (!pChar) {
            Serial.println("T! no TreadData char");
            pTreadmillClient->disconnect();
            vTaskDelay(pdMS_TO_TICKS(TREADMILL_RETRY_MS));
            continue;
        }

        // Subscribe with delays between each CCCD write — some treadmill BLE
        // stacks silently drop CCCDs written in rapid succession.

        // Subscribe only to Treadmill Data — test whether other subscriptions interfere
        bool subOk = false;
        if (pChar->canNotify()) {
            subOk = pChar->subscribe(true, notifyCallback);
        }
        Serial.printf("sub 2AD3: %d\n", subOk);
        vTaskDelay(pdMS_TO_TICKS(500));

        // Training Status (0x2ACD) — proprietary 19-byte packet; bytes 4-5 = set speed (0.1 km/h)
        NimBLERemoteCharacteristic* pTraining = pSvc->getCharacteristic(NimBLEUUID("2acd"));
        if (pTraining && pTraining->canNotify()) {
            bool ok = pTraining->subscribe(true, [](NimBLERemoteCharacteristic*, uint8_t* d, size_t l, bool) {
                Serial.printf("TRST len=%d:", l);
                for (size_t i = 0; i < l; i++) Serial.printf(" %02x", d[i]);
                Serial.println();

                if (l < 6) return;  // need at least bytes 0-5

                // Bytes 2-3: actual belt speed in 0.01 km/h units (FTMS standard)
                // Bytes 4-5: set/target speed in 0.1 km/h units — logged for reference
                uint16_t set_raw   = (uint16_t)d[4] | ((uint16_t)d[5] << 8);
                uint16_t speed_raw = (uint16_t)d[2] | ((uint16_t)d[3] << 8);
                Serial.printf("TRST actual=%.2fkmh set=%.1fkmh\n",
                              speed_raw / 100.0f, set_raw * 0.1f);
                handleSpeedRaw(speed_raw);
            });
            Serial.printf("sub TRST: %d\n", ok);
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        // Machine Status — opcode 0x08 = Target Speed Changed (uint16 in 0.01 km/h follows)
        NimBLERemoteCharacteristic* pStatus = pSvc->getCharacteristic(ftmsStatusUUID);
        if (pStatus && pStatus->canNotify()) {
            bool ok = pStatus->subscribe(true, [](NimBLERemoteCharacteristic*, uint8_t* d, size_t l, bool) {
                Serial.printf("FMST len=%d:", l);
                for (size_t i = 0; i < l; i++) Serial.printf(" %02x", d[i]);
                Serial.println();

                if (l >= 3 && d[0] == 0x08) {  // Target Speed Changed
                    uint16_t spd_raw = (uint16_t)d[1] | ((uint16_t)d[2] << 8);
                    Serial.printf("FMST spd=%.2fkmh\n", spd_raw / 100.0f);
                    handleSpeedRaw(spd_raw);
                }
            });
            Serial.printf("sub FMST: %d\n", ok);
        }

        // Read 0x2AD3 once at connect to get current speed (not just relying on notifications)
        {
            std::string val = pChar->readValue();
            Serial.printf("2AD3 read len=%d:", (int)val.length());
            for (size_t i = 0; i < val.length(); i++) Serial.printf(" %02x", (uint8_t)val[i]);
            Serial.println();
        }

        treadmillConnected = true;
        Serial.println("T");
    }
}

// ── SETUP ─────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);
    delay(200);
    Serial.println("Boot");

    dataMutex = xSemaphoreCreateMutex();

    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);  // off (active low)

    // ── Zigbee (must init before BLE — both share the 2.4 GHz radio) ─────
    zbSpeed.setManufacturerAndModel("TitanBridge", "TreadmillSpeed");
    zbSpeed.setMinMaxValue(0.0f, 55.56f);

    zbRunning.setManufacturerAndModel("TitanBridge", "TreadmillRunning");

    zbGarmin.setManufacturerAndModel("TitanBridge", "GarminConnected");

    Zigbee.addEndpoint(&zbSpeed);
    Zigbee.addEndpoint(&zbRunning);
    Zigbee.addEndpoint(&zbGarmin);

    // Start Zigbee as End Device (non-blocking — joins in background)
    if (!Zigbee.begin()) {
        Serial.println("Zigbee failed to start, rebooting");
        delay(1000);
        ESP.restart();
    }
    Serial.println("ZB start");

    // ── BLE ───────────────────────────────────────────────────────────────
    NimBLEDevice::init("TitanBridge");
    NimBLEDevice::setPower(9);
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC);

    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new GarminServerCallbacks());
    pServer->advertiseOnDisconnect(false);

    NimBLEService* pRSC = pServer->createService(RSC_SERVICE_UUID);

    pRSCMeasurement = pRSC->createCharacteristic(RSC_MEASUREMENT_UUID,
                                                   NIMBLE_PROPERTY::NOTIFY);

    // RSC Feature: bits 1+2 = Total Distance + Walking/Running Status
    uint16_t feat = 0x0006;
    pRSC->createCharacteristic(RSC_FEATURE_UUID, NIMBLE_PROPERTY::READ)
        ->setValue((uint8_t*)&feat, 2);

    // Sensor Location: 0x06 = Right Shoe
    uint8_t loc = 0x06;
    pRSC->createCharacteristic(RSC_LOCATION_UUID, NIMBLE_PROPERTY::READ)
        ->setValue(&loc, 1);

    // SC Control Point: WRITE + INDICATE (required by Garmin)
    pSCCtrl = pRSC->createCharacteristic(
        RSC_CTRL_POINT_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::INDICATE);
    pSCCtrl->setCallbacks(new SCControlPointCallbacks());

    pRSC->start();

    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    pAdv->addServiceUUID(RSC_SERVICE_UUID);
    pAdv->setAppearance(0x0440);  // Running Sensor / Foot Pod
    pAdv->setMinInterval(32);     // 20 ms
    pAdv->setMaxInterval(48);     // 30 ms
    pAdv->enableScanResponse(true);
    pAdv->start();
    Serial.println("A");

    xTaskCreate(treadmillTask, "treadmill", 4096, nullptr, 1, nullptr);
}

// ── LOOP ──────────────────────────────────────────────────────────────────
void loop() {
    unsigned long now = millis();

    // Push initial Zigbee state once after coordinator join
    if (!zbInitialReported && Zigbee.connected()) {
        zbGarmin.setOccupancy(false);
        zbGarmin.report();
        zbInitialReported = true;
    }

    // BLE keep-alive
    if (deviceConnected && (now - lastNotifyTime >= KEEPALIVE_MS)) {
        sendRSCUpdate();
    }

    // LED: solid = both connected, slow blink = Garmin only, fast blink = neither
    if (deviceConnected && treadmillConnected) {
        digitalWrite(LED_BUILTIN, LOW);
    } else if (deviceConnected) {
        digitalWrite(LED_BUILTIN, (now / 500) % 2 ? HIGH : LOW);
    } else {
        digitalWrite(LED_BUILTIN, (now / 200) % 2 ? HIGH : LOW);
    }

    delay(50);
}
