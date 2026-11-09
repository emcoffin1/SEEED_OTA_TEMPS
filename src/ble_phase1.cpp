#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>

#include "device_config.h"
#include "ota_update.h"
#include "secrets.h"

namespace {
constexpr uint32_t ADVERTISING_DURATION_MS = 60000;
constexpr uint32_t WIFI_TIMEOUT_MS = 20000;
constexpr uint32_t OTA_CHECK_INTERVAL_MS = 30000;

NimBLEAdvertising *advertising = nullptr;
uint32_t lastOtaCheckMs = 0;

uint32_t versionCrc32(const char *value) {
  uint32_t crc = 0xffffffffU;
  while (*value) {
    crc ^= static_cast<uint8_t>(*value++);
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
  }
  return crc ^ 0xffffffffU;
}

bool connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - started < WIFI_TIMEOUT_MS) {
    delay(100);
  }
  Serial.printf("BLE diagnostic Wi-Fi connected=%d\n",
                WiFi.status() == WL_CONNECTED);
  return WiFi.status() == WL_CONNECTED;
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);

  NimBLEDevice::init("");
  Serial.printf("BLE address: %s\n",
                NimBLEDevice::getAddress().toString().c_str());

  NimBLEAdvertisementData advertisement;
  const bool flagsOkay = advertisement.setFlags(
      BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  const bool dataOkay = advertisement.setName("XIAO-BLE-TEST");

  advertising = NimBLEDevice::getAdvertising();
  advertising->enableScanResponse(false);
  const bool advertisementOkay =
      advertising->setAdvertisementData(advertisement);
  const bool started = NimBLEDevice::startAdvertising(
      ADVERTISING_DURATION_MS);

  Serial.printf(
      "BLE flags=%d data=%d payload=%u set=%d started=%d active=%d\n",
      flagsOkay, dataOkay,
      static_cast<unsigned>(advertisement.getPayload().size()),
      advertisementOkay, started, advertising->isAdvertising());
  Serial.printf("Advertisement: %s\n",
                advertisement.toString().c_str());
  Serial.printf("BLE device=%u version=%s crc32=%08lx sleep=disabled\n",
                ACTIVE_DEVICE_LOCATION == 1 ? 1 : 2, FIRMWARE_VERSION,
                static_cast<unsigned long>(versionCrc32(FIRMWARE_VERSION)));

  delay(60000);
  if (connectWifi()) checkForSignedFirmwareUpdate();
  lastOtaCheckMs = millis();
}

void loop() {
  if (!advertising->isAdvertising()) {
    const bool restarted = NimBLEDevice::startAdvertising(
        ADVERTISING_DURATION_MS);
    Serial.printf("BLE advertising restarted=%d active=%d\n", restarted,
                  advertising->isAdvertising());
  }
  if (millis() - lastOtaCheckMs >= OTA_CHECK_INTERVAL_MS) {
    lastOtaCheckMs = millis();
    if (connectWifi()) checkForSignedFirmwareUpdate();
  }
  delay(100);
}
