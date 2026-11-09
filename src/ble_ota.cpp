#include "ble_ota.h"

#include <NimBLEDevice.h>
#include <Update.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "ota_public_key.h"

namespace {
constexpr char SERVICE_UUID[] = "7d5a0001-7b21-4f6a-8f2b-4b68f64a9c01";
constexpr char CONTROL_UUID[] = "7d5a0002-7b21-4f6a-8f2b-4b68f64a9c01";
constexpr char DATA_UUID[] = "7d5a0003-7b21-4f6a-8f2b-4b68f64a9c01";
constexpr char STATUS_UUID[] = "7d5a0004-7b21-4f6a-8f2b-4b68f64a9c01";

constexpr uint8_t PROTOCOL_VERSION = 1;
constexpr uint8_t CONTROL_BEGIN = 0x01;
constexpr uint8_t CONTROL_COMMIT = 0x02;
constexpr uint8_t CONTROL_ABORT = 0x03;
constexpr uint8_t CONTROL_QUERY = 0x04;
constexpr uint8_t STREAM_SIGNATURE = 0x01;
constexpr uint8_t STREAM_FIRMWARE = 0x02;
constexpr uint8_t STATUS_READY = 0x10;
constexpr uint8_t STATUS_SUCCESS = 0x12;
constexpr uint8_t STATUS_ERROR = 0x7f;
constexpr size_t BEGIN_FIXED_BYTES = 40;
constexpr size_t DATA_HEADER_BYTES = 5;
constexpr size_t MAX_SIGNATURE_BYTES = 1024;
constexpr size_t MAX_FIRMWARE_BYTES = 2U * 1024U * 1024U;

NimBLEServer *bleServer = nullptr;
NimBLECharacteristic *statusCharacteristic = nullptr;

struct OtaSession {
  bool active = false;
  uint32_t firmwareSize = 0;
  uint32_t firmwareOffset = 0;
  uint16_t signatureSize = 0;
  uint16_t signatureOffset = 0;
  uint8_t expectedDigest[32]{};
  String version;
  std::vector<uint8_t> signature;
  mbedtls_sha256_context sha;
  bool shaInitialized = false;
} session;

uint16_t readLe16(const uint8_t *data) {
  return static_cast<uint16_t>(data[0]) |
         (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t readLe32(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

void writeLe16(uint8_t *data, uint16_t value) {
  data[0] = value & 0xff;
  data[1] = value >> 8;
}

void writeLe32(uint8_t *data, uint32_t value) {
  data[0] = value & 0xff;
  data[1] = (value >> 8) & 0xff;
  data[2] = (value >> 16) & 0xff;
  data[3] = value >> 24;
}

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

void notifyStatus(uint8_t code, const char *message = nullptr) {
  if (!statusCharacteristic) return;
  const size_t messageLength = message ? strlen(message) : 0;
  std::vector<uint8_t> value(9 + messageLength);
  value[0] = code;
  writeLe32(value.data() + 1, session.signatureOffset);
  writeLe32(value.data() + 5, session.firmwareOffset);
  if (messageLength) memcpy(value.data() + 9, message, messageLength);
  statusCharacteristic->setValue(value);
  statusCharacteristic->notify();
}

void clearSession(bool abortUpdate) {
  if (abortUpdate && session.active) Update.abort();
  if (session.shaInitialized) mbedtls_sha256_free(&session.sha);
  session = OtaSession{};
}

bool signatureValid(const uint8_t digest[32]) {
  mbedtls_pk_context key;
  mbedtls_pk_init(&key);
  const int parsed = mbedtls_pk_parse_public_key(
      &key, reinterpret_cast<const uint8_t *>(OTA_PUBLIC_KEY),
      strlen(OTA_PUBLIC_KEY) + 1);
  const int verified = parsed == 0
      ? mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, digest, 32,
                          session.signature.data(), session.signature.size())
      : parsed;
  mbedtls_pk_free(&key);
  return verified == 0;
}

bool sameTransfer(const uint8_t *data, uint32_t firmwareSize,
                  uint16_t signatureSize, const String &version) {
  return session.active && session.firmwareSize == firmwareSize &&
         session.signatureSize == signatureSize && session.version == version &&
         memcmp(session.expectedDigest, data + 5, 32) == 0;
}

void beginTransfer(const uint8_t *data, size_t length) {
  if (length < BEGIN_FIXED_BYTES) {
    notifyStatus(STATUS_ERROR, "BEGIN packet too short");
    return;
  }
  const uint32_t firmwareSize = readLe32(data + 1);
  const uint16_t signatureSize = readLe16(data + 37);
  const uint8_t versionLength = data[39];
  if (versionLength == 0 || versionLength > 40 ||
      length != BEGIN_FIXED_BYTES + versionLength || firmwareSize == 0 ||
      firmwareSize > MAX_FIRMWARE_BYTES || signatureSize == 0 ||
      signatureSize > MAX_SIGNATURE_BYTES) {
    notifyStatus(STATUS_ERROR, "Invalid BEGIN fields");
    return;
  }
  const String version(reinterpret_cast<const char *>(data + BEGIN_FIXED_BYTES),
                       versionLength);
  if (sameTransfer(data, firmwareSize, signatureSize, version)) {
    notifyStatus(STATUS_READY);
    return;
  }

  clearSession(true);
  if (!Update.begin(firmwareSize)) {
    notifyStatus(STATUS_ERROR, "Could not open OTA partition");
    return;
  }
  session.active = true;
  session.firmwareSize = firmwareSize;
  session.signatureSize = signatureSize;
  session.version = version;
  memcpy(session.expectedDigest, data + 5, sizeof(session.expectedDigest));
  session.signature.resize(signatureSize);
  mbedtls_sha256_init(&session.sha);
  session.shaInitialized = true;
  if (mbedtls_sha256_starts_ret(&session.sha, 0) != 0) {
    clearSession(true);
    notifyStatus(STATUS_ERROR, "Could not start SHA-256");
    return;
  }
  Serial.printf("BLE OTA ready for %s (%lu bytes).\n", version.c_str(),
                static_cast<unsigned long>(firmwareSize));
  notifyStatus(STATUS_READY);
}

void receiveData(const uint8_t *data, size_t length) {
  if (!session.active || length <= DATA_HEADER_BYTES) {
    notifyStatus(STATUS_ERROR, "No active OTA transfer");
    return;
  }
  const uint8_t stream = data[0];
  const uint32_t offset = readLe32(data + 1);
  const uint8_t *payload = data + DATA_HEADER_BYTES;
  const size_t payloadLength = length - DATA_HEADER_BYTES;

  if (stream == STREAM_SIGNATURE) {
    if (offset != session.signatureOffset ||
        payloadLength > session.signatureSize - session.signatureOffset) {
      notifyStatus(STATUS_ERROR, "Unexpected signature offset");
      return;
    }
    memcpy(session.signature.data() + session.signatureOffset, payload,
           payloadLength);
    session.signatureOffset += payloadLength;
    return;
  }
  if (stream != STREAM_FIRMWARE || offset != session.firmwareOffset ||
      payloadLength > session.firmwareSize - session.firmwareOffset) {
    notifyStatus(STATUS_ERROR, "Unexpected firmware offset");
    return;
  }
  if (Update.write(const_cast<uint8_t *>(payload), payloadLength) != payloadLength ||
      mbedtls_sha256_update_ret(&session.sha, payload, payloadLength) != 0) {
    notifyStatus(STATUS_ERROR, "Could not write firmware");
    clearSession(true);
    return;
  }
  session.firmwareOffset += payloadLength;
}

void commitTransfer() {
  if (!session.active || session.signatureOffset != session.signatureSize ||
      session.firmwareOffset != session.firmwareSize) {
    notifyStatus(STATUS_ERROR, "OTA transfer is incomplete");
    return;
  }
  uint8_t digest[32];
  if (mbedtls_sha256_finish_ret(&session.sha, digest) != 0) {
    notifyStatus(STATUS_ERROR, "Could not finish SHA-256");
    clearSession(true);
    return;
  }
  mbedtls_sha256_free(&session.sha);
  session.shaInitialized = false;
  if (memcmp(digest, session.expectedDigest, sizeof(digest)) != 0 ||
      !signatureValid(digest)) {
    notifyStatus(STATUS_ERROR, "Digest or signature rejected");
    clearSession(true);
    return;
  }
  if (!Update.end(true)) {
    notifyStatus(STATUS_ERROR, "Could not finalize OTA image");
    clearSession(true);
    return;
  }
  Serial.printf("Signed BLE firmware %s installed; rebooting.\n",
                session.version.c_str());
  notifyStatus(STATUS_SUCCESS);
  Serial.flush();
  delay(500);
  ESP.restart();
}

class ControlCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic,
               NimBLEConnInfo & /* connection */) override {
    const NimBLEAttValue value = characteristic->getValue();
    if (value.size() == 0) return;
    const uint8_t *data = value.data();
    switch (data[0]) {
      case CONTROL_BEGIN:
        beginTransfer(data, value.size());
        break;
      case CONTROL_COMMIT:
        if (value.size() == 1) commitTransfer();
        break;
      case CONTROL_ABORT:
        if (value.size() == 1) clearSession(true);
        break;
      case CONTROL_QUERY:
        if (value.size() == 1) notifyStatus(STATUS_READY);
        break;
      default:
        notifyStatus(STATUS_ERROR, "Unknown control command");
    }
  }
};

class DataCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic,
               NimBLEConnInfo & /* connection */) override {
    const NimBLEAttValue value = characteristic->getValue();
    receiveData(value.data(), value.size());
  }
};

ControlCallbacks controlCallbacks;
DataCallbacks dataCallbacks;
}  // namespace

void startBleOta(const char *firmwareVersion, uint8_t deviceLocation,
                 uint16_t sequence, float temperatureF, bool readingValid) {
  NimBLEDevice::init("");
  NimBLEDevice::setMTU(247);
  bleServer = NimBLEDevice::createServer();
  bleServer->advertiseOnDisconnect(true);
  NimBLEService *service = bleServer->createService(SERVICE_UUID);
  NimBLECharacteristic *control = service->createCharacteristic(
      CONTROL_UUID, NIMBLE_PROPERTY::WRITE, 96);
  NimBLECharacteristic *data = service->createCharacteristic(
      DATA_UUID, NIMBLE_PROPERTY::WRITE, 244);
  statusCharacteristic = service->createCharacteristic(
      STATUS_UUID, NIMBLE_PROPERTY::NOTIFY, 96);
  control->setCallbacks(&controlCallbacks);
  data->setCallbacks(&dataCallbacks);

  const long centiFahrenheit = lroundf(temperatureF * 100.0F);
  const int16_t encodedTemperature = static_cast<int16_t>(
      std::max(-32768L, std::min(32767L, centiFahrenheit)));
  uint8_t payload[10]{};
  payload[0] = PROTOCOL_VERSION;
  payload[1] = deviceLocation == 1 ? 1 : 2;
  writeLe16(payload + 2, sequence);
  writeLe16(payload + 4, static_cast<uint16_t>(
      readingValid ? encodedTemperature : INT16_MIN));
  writeLe32(payload + 6, versionCrc32(firmwareVersion));

  NimBLEAdvertisementData advertisement;
  advertisement.setServiceData(NimBLEUUID(SERVICE_UUID), payload,
                               sizeof(payload));
  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->setAdvertisementData(advertisement);
  advertising->enableScanResponse(false);
  advertising->setMinInterval(160);
  advertising->setMaxInterval(320);
  NimBLEDevice::startAdvertising();
  Serial.println("BLE temperature and signed OTA service advertising.");
}

void waitForBleWindow(uint32_t discoveryWindowMs, uint32_t maxTransferMs) {
  const uint32_t started = millis();
  while (millis() - started < maxTransferMs) {
    const bool connected = bleServer && bleServer->getConnectedCount() > 0;
    if (millis() - started >= discoveryWindowMs && !connected &&
        !session.active) {
      return;
    }
    delay(25);
  }
  if (session.active) {
    Serial.println("BLE OTA timed out; retaining current firmware.");
    clearSession(true);
  }
}
