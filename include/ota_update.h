#pragma once

#ifdef BLE_PHASE1_OTA_DIAGNOSTIC
constexpr char FIRMWARE_VERSION[] = "ble-radio-test-1";
#else
constexpr char FIRMWARE_VERSION[] = "temperature-7";
#endif

bool checkForSignedFirmwareUpdate();
