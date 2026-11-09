#include <Arduino.h>
#include <BMP280.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_sleep.h>

#include "ble_ota.h"
#include "device_config.h"
#include "ota_update.h"
#include "secrets.h"

namespace {
constexpr uint32_t DEFAULT_SEND_INTERVAL_SECONDS = 30;

// Enable only after reserving this address for the XIAO in the router.
constexpr bool USE_STATIC_IP = false;
const IPAddress STATIC_IP(192, 168, 68, 201);
const IPAddress GATEWAY(192, 168, 68, 1);
const IPAddress SUBNET(255, 255, 255, 0);
const IPAddress DNS_SERVER(192, 168, 68, 1);

constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20UL * 1000UL;
constexpr uint32_t WIFI_FAST_CONNECT_TIMEOUT_MS = 5UL * 1000UL;

// Seeed Studio XIAO ESP32-C3: D4 is GPIO6/SDA and D5 is GPIO7/SCL.
constexpr uint8_t I2C_SDA_PIN = 6;
constexpr uint8_t I2C_SCL_PIN = 7;
constexpr uint8_t BMP280_I2C_ADDRESS = 0x76;
constexpr uint32_t WIFI_CACHE_MAGIC = 0x57494649;

BMP280 bmp;
bool sensorReady = false;

RTC_DATA_ATTR uint32_t wifiCacheMagic = 0;
RTC_DATA_ATTR int32_t cachedWifiChannel = 0;
RTC_DATA_ATTR uint8_t cachedWifiBssid[6] = {};
RTC_DATA_ATTR uint32_t nextSleepSeconds = DEFAULT_SEND_INTERVAL_SECONDS;
RTC_DATA_ATTR uint16_t readingSequence = 0;
float lastTemperatureF = 0.0F;
bool lastReadingValid = false;

bool connectBmp280() {
  if (bmp.begin(I2C_SDA_PIN, I2C_SCL_PIN)) {
    bmp.setOversampling(4);
    Serial.printf("BMP280 connected at I2C address 0x%02X.\n",
                  BMP280_I2C_ADDRESS);
    return true;
  }

  Serial.printf("BMP280 not found (SDA GPIO %u, SCL GPIO %u). ",
                I2C_SDA_PIN, I2C_SCL_PIN);
  Serial.printf("Expected address 0x%02X; I2C error %d.\n",
                BMP280_I2C_ADDRESS, bmp.getError());

  Serial.print("Other I2C devices found:");
  bool foundDevice = false;
  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      Serial.printf(" 0x%02X", address);
      foundDevice = true;
    }
  }
  if (!foundDevice) {
    Serial.print(" none");
  }
  Serial.println();
  return false;
}

bool connectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  Serial.printf("Connecting to Wi-Fi \"%s\"", WIFI_SSID);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.setSleep(true);

  if (USE_STATIC_IP &&
      !WiFi.config(STATIC_IP, GATEWAY, SUBNET, DNS_SERVER)) {
    Serial.println("\nCould not configure static IP.");
    return false;
  }

  bool usingCachedAccessPoint =
      wifiCacheMagic == WIFI_CACHE_MAGIC && cachedWifiChannel > 0;
  if (usingCachedAccessPoint) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, cachedWifiChannel,
               cachedWifiBssid, true);
  } else {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startedAt < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
    if (usingCachedAccessPoint &&
        millis() - startedAt >= WIFI_FAST_CONNECT_TIMEOUT_MS) {
      Serial.print(" cached AP unavailable; scanning");
      WiFi.disconnect(false);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      usingCachedAccessPoint = false;
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWi-Fi connection timed out.");
    return false;
  }

  Serial.printf("\nWi-Fi connected; XIAO address: %s\n",
                WiFi.localIP().toString().c_str());
  cachedWifiChannel = WiFi.channel();
  memcpy(cachedWifiBssid, WiFi.BSSID(), sizeof(cachedWifiBssid));
  wifiCacheMagic = WIFI_CACHE_MAGIC;
  return true;
}

void sampleTemperature() {
  if (!sensorReady) {
    sensorReady = connectBmp280();
    if (!sensorReady) {
      return;
    }
  }

  const char waitMs = bmp.startMeasurment();
  if (waitMs <= 0) {
    Serial.printf("Could not start BMP280 measurement (I2C error %d).\n",
                  bmp.getError());
    sensorReady = false;
    return;
  }

  delay(waitMs);
  double temperatureC;
  double pressureHpa;
  if (!bmp.getTemperatureAndPressure(temperatureC, pressureHpa)) {
    Serial.printf("Could not read BMP280 measurement (I2C error %d).\n",
                  bmp.getError());
    sensorReady = false;
    return;
  }

  Serial.printf("BMP280 temperature: %.2f C | Pressure: %.2f hPa\n",
                temperatureC, pressureHpa);
  lastTemperatureF = static_cast<float>(temperatureC * 9.0 / 5.0 + 32.0);
  lastReadingValid = true;
  ++readingSequence;
}

[[noreturn]] void enterDeepSleep() {
  Serial.printf("Entering deep sleep for %lu seconds.\n",
                static_cast<unsigned long>(nextSleepSeconds));

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Wire.end();

  esp_sleep_enable_timer_wakeup(
      static_cast<uint64_t>(nextSleepSeconds) * 1000000ULL);
  Serial.flush();
  delay(20);
  esp_deep_sleep_start();

  // esp_deep_sleep_start() does not return.
  while (true) {
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);

  const esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  if (wakeCause == ESP_SLEEP_WAKEUP_TIMER) {
    Serial.println("Woke from timer.");
  } else {
    // Native USB can take several seconds to enumerate. Keep cold boots awake
    // long enough for a serial monitor to attach without delaying timer wakes.
    delay(5000);
    Serial.println("Cold boot or external reset.");
  }

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Serial.printf("I2C started: SDA D4/GPIO%u, SCL D5/GPIO%u.\n",
                I2C_SDA_PIN, I2C_SCL_PIN);
  sensorReady = connectBmp280();

  sampleTemperature();
  startBleOta(FIRMWARE_VERSION, ACTIVE_DEVICE_LOCATION, readingSequence,
              lastTemperatureF, lastReadingValid);
  waitForBleWindow(5000, 3UL * 60UL * 1000UL);
  // Sensor readings travel only over BLE. Wi-Fi remains available afterward as
  // a signed OTA recovery path and never posts the temperature sample.
  if (WiFi.status() != WL_CONNECTED) {
    connectWifi();
  }
  if (WiFi.status() == WL_CONNECTED) {
    checkForSignedFirmwareUpdate();
  }
  enterDeepSleep();
}

void loop() {
  // The device always enters deep sleep before setup() finishes.
}
