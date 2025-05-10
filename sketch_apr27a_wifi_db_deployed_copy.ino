/*************************************************************
   ESP32-WROOM-32E  ·  BLE Beacon + Wi-Fi & LAN-DB Auth
   -----------------------------------------------------
   • Advertises tag “40ab22c0” (Mfr-ID 0x04B5) every 300 ms
   • Scans for phone beacons   (Mfr-ID 0x007B, UID ECEE1234)
   • Queries your PC’s SQLite-backed REST API at
       http://172.20.10.14:5000/api/door/40ab22c0/uid/ECEE1234
   • If {"access":true} → Relay K1 (IO14 LOW) + LED D2 (IO12 HIGH) for 10 s
*************************************************************/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEAdvertising.h>
#include <BLEScan.h>

// ─── CONFIGURATION ───
const bool USE_WIFI_AUTH = true;    // enable REST-API lookup
const bool USE_LOCAL_DB  = false;   // no onboard DB in this build

// Wi-Fi & REST server (your laptop)
#define WIFI_SSID   "Alsiary"
#define WIFI_PASS   "Saleh0543470378"
#define AUTH_IP     "172.20.10.14"   // your PC’s LAN IP
#define AUTH_PORT   "5000"
#define DOOR_ID     "40ab22c0"

// Hardware pins
const int RELAY_PIN = 14;    // active-LOW relay coil (K1)
const int LED_PIN   = 12;    // active-HIGH green LED (D2)

// BLE manufacturer IDs & constants
#define MANUF_PHONE   0x007B
#define MANUF_ESP     0x04B5
const uint8_t  BEACON_VER    = 0x01;
const char     TAG_NAME[]    = "40ab22c0";
const uint16_t ADV_PERIOD_MS = 300;

// Globals
BLEAdvertising* pAdv;
BLEScan*        pScan;
unsigned long   lastAdv    = 0;
unsigned long   latchStart = 0;
bool            latched    = false;

// ─── HELPERS ───

// Build a String containing [id_lo, id_hi, payload…]
String makeMfr(uint16_t id, const uint8_t* payload, uint8_t len) {
  static uint8_t buf[31];
  buf[0] = id & 0xFF;
  buf[1] = id >> 8;
  memcpy(buf + 2, payload, len);
  return String((char*)buf, len + 2);
}

// Broadcast our door-ID beacon
void loadTagBeacon() {
  const uint8_t tagLen = sizeof(TAG_NAME) - 1;
  uint8_t pay[2 + tagLen];
  pay[0] = BEACON_VER;
  pay[1] = tagLen;
  memcpy(pay + 2, TAG_NAME, tagLen);

  BLEAdvertisementData ad;
  ad.setFlags(0x04);
  ad.setManufacturerData(makeMfr(MANUF_ESP, pay, 2 + tagLen));
  ad.setName(TAG_NAME);
  pAdv->setAdvertisementData(ad);
}

// REST call to your PC server
bool askServer(const char* uid) {
  if (!USE_WIFI_AUTH || WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  String url = String("http://") + AUTH_IP + ":" + AUTH_PORT
             + "/api/door/" DOOR_ID "/uid/" + String(uid);
  http.begin(url);
  int rc = http.GET();
  if (rc != 200) { http.end(); return false; }
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, http.getStream())) { http.end(); return false; }
  http.end();
  return doc["access"].as<bool>();
}

// BLE scan callback – looks only for UID ECEE1234
class ScanCB : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    auto md = d.getManufacturerData();
    // Debug: dump every MFR packet
    Serial.printf("%s RSSI %d  MFR ", d.getAddress().toString().c_str(), d.getRSSI());
    for (size_t i = 0; i < md.length(); i++) Serial.printf("%02X ", md[i]);
    Serial.println();

    if (md.length() < 8) return;
    const uint8_t* b = (const uint8_t*)md.c_str();
    uint16_t id = b[0] | (b[1] << 8);
    if (id != MANUF_PHONE || b[2] != BEACON_VER || b[3] != 4) return;

    // Extract UID bytes
    char uid[9];
    sprintf(uid, "%02X%02X%02X%02X", b[4], b[5], b[6], b[7]);
    Serial.printf("UID=%s\n", uid);

    // Local filter: only ECEE1234
    if (strcmp(uid, "ECEE1234") != 0) return;

    // Query server
    Serial.println("→ Found ECEE1234, querying server...");
    if (askServer(uid)) {
      Serial.println("✓ ACCESS GRANTED");
      digitalWrite(RELAY_PIN, LOW);   // energize relay (active-LOW)
      digitalWrite(LED_PIN,   HIGH);  // turn on LED
      latched = true;
      latchStart = millis();
    } else {
      Serial.println("⨯ ACCESS DENIED");
    }
  }
};

void setup() {
  Serial.begin(115200);
  // Init pins
  pinMode(RELAY_PIN, OUTPUT); digitalWrite(RELAY_PIN, HIGH); // relay OFF
  pinMode(LED_PIN,   OUTPUT); digitalWrite(LED_PIN,   LOW);

  // 1) Connect Wi-Fi (if enabled)
  if (USE_WIFI_AUTH) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("Wi-Fi");
    while (WiFi.status() != WL_CONNECTED) {
      Serial.print('.');
      delay(300);
    }
    Serial.println(" ✓");
  }

  // 2) BLE advertise + scan (after Wi-Fi)
  BLEDevice::init("");
  pAdv = BLEDevice::getAdvertising();
  pAdv->setAdvertisementType(ADV_TYPE_NONCONN_IND);
  pAdv->setScanResponse(false);
  pAdv->setMinPreferred(0x06);
  loadTagBeacon();
  pAdv->start();
  lastAdv = millis();

  pScan = BLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new ScanCB(), /*dups=*/true);
  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(99);
  pScan->start(0, nullptr);

  Serial.println("Beacon + scanner running");
}

void loop() {
  if (millis() - lastAdv > ADV_PERIOD_MS) {
    lastAdv = millis();
    loadTagBeacon();
    pAdv->start();
  }
  if (latched && millis() - latchStart > 10000) {
    latched = false;
    digitalWrite(RELAY_PIN, HIGH);
    digitalWrite(LED_PIN,   LOW);
  }
  delay(20);
}
