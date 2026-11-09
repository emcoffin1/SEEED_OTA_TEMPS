#pragma once

#include <Arduino.h>

// Advertise one sensor sample and expose the signed BLE OTA service used by the
// Raspberry Pi gateway.
void startBleOta(const char *firmwareVersion, uint8_t deviceLocation,
                 uint16_t sequence, float temperatureF, bool readingValid);

// Keep the radio available for discovery. A connected or interrupted OTA
// transfer extends the window up to maxTransferMs.
void waitForBleWindow(uint32_t discoveryWindowMs, uint32_t maxTransferMs);
