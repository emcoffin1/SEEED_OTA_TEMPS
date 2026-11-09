#include "ota_update.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

#include "ota_public_key.h"
#include "device_config.h"
#include "secrets.h"

namespace {
constexpr uint32_t HTTP_TIMEOUT_MS = 15UL * 1000UL;

String jsonString(const String &body, const char *key) {
  const String marker = String("\"") + key + "\":\"";
  const int start = body.indexOf(marker);
  if (start < 0) return {};
  const int valueStart = start + marker.length();
  const int end = body.indexOf('"', valueStart);
  return end < 0 ? String() : body.substring(valueStart, end);
}

bool downloadSignature(const String &url, uint8_t *signature, size_t &length) {
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, url) || http.GET() != HTTP_CODE_OK) return false;
  const int size = http.getSize();
  if (size <= 0 || size > 512) {
    http.end();
    return false;
  }
  length = http.getStreamPtr()->readBytes(signature, size);
  http.end();
  return length == static_cast<size_t>(size);
}

bool signatureValid(const uint8_t digest[32], const uint8_t *signature,
                    size_t signatureLength) {
  mbedtls_pk_context key;
  mbedtls_pk_init(&key);
  const int parsed = mbedtls_pk_parse_public_key(
      &key, reinterpret_cast<const uint8_t *>(OTA_PUBLIC_KEY),
      strlen(OTA_PUBLIC_KEY) + 1);
  const int verified = parsed == 0
      ? mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, digest, 32, signature,
                          signatureLength)
      : parsed;
  mbedtls_pk_free(&key);
  return verified == 0;
}

bool installFirmware(const String &firmwareUrl, const String &signatureUrl,
                     const String &expectedHash) {
  uint8_t signature[512];
  size_t signatureLength = 0;
  if (!downloadSignature(signatureUrl, signature, signatureLength)) {
    Serial.println("Could not download firmware signature.");
    return false;
  }

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, firmwareUrl)) return false;
  const int status = http.GET();
  const int total = http.getSize();
  if (status != HTTP_CODE_OK || total <= 0 || !Update.begin(total)) {
    Serial.printf("Could not start firmware update (HTTP %d, size %d).\n",
                  status, total);
    http.end();
    return false;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts_ret(&sha, 0);
  uint8_t buffer[1024];
  int remaining = total;
  WiFiClient *stream = http.getStreamPtr();
  while (remaining > 0) {
    const size_t wanted = min(sizeof(buffer), static_cast<size_t>(remaining));
    const size_t received = stream->readBytes(buffer, wanted);
    if (!received || Update.write(buffer, received) != received) break;
    mbedtls_sha256_update_ret(&sha, buffer, received);
    remaining -= received;
  }
  uint8_t digest[32];
  mbedtls_sha256_finish_ret(&sha, digest);
  mbedtls_sha256_free(&sha);
  http.end();

  String actualHash;
  actualHash.reserve(64);
  for (uint8_t value : digest) {
    char hex[3];
    snprintf(hex, sizeof(hex), "%02x", value);
    actualHash += hex;
  }
  if (remaining != 0 || !actualHash.equalsIgnoreCase(expectedHash) ||
      !signatureValid(digest, signature, signatureLength)) {
    Serial.println("Firmware integrity/signature verification failed.");
    Update.abort();
    return false;
  }
  if (!Update.end(true)) {
    Serial.printf("Could not finalize firmware update: %s\n",
                  Update.errorString());
    return false;
  }
  Serial.println("Signed firmware installed; rebooting.");
  Serial.flush();
  delay(500);
  ESP.restart();
  return true;
}
}  // namespace

bool checkForSignedFirmwareUpdate() {
  if (WiFi.status() != WL_CONNECTED) return false;

  String url = String(FIRMWARE_MANIFEST_URL) + "?device=" + ACTIVE_DEVICE_NAME +
               "&version=" + FIRMWARE_VERSION;
  url.replace(" ", "%20");
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) return false;
  const int status = http.GET();
  const String body = status == HTTP_CODE_OK ? http.getString() : String();
  http.end();
  if (status != HTTP_CODE_OK || body.indexOf("\"available\":true") < 0) {
    Serial.println("No signed firmware update pending.");
    return false;
  }

  const String version = jsonString(body, "version");
  const String firmwareUrl = jsonString(body, "firmware_url");
  const String signatureUrl = jsonString(body, "signature_url");
  const String hash = jsonString(body, "sha256");
  if (version.isEmpty() || firmwareUrl.isEmpty() || signatureUrl.isEmpty() ||
      hash.length() != 64) {
    Serial.println("Invalid firmware manifest.");
    return false;
  }
  Serial.printf("Installing signed firmware %s.\n", version.c_str());
  return installFirmware(firmwareUrl, signatureUrl, hash);
}
