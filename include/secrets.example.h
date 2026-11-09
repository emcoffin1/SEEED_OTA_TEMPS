#pragma once

constexpr char WIFI_SSID[] = "replace-with-wifi-name";
constexpr char WIFI_PASSWORD[] = "replace-with-wifi-password";

constexpr char FIRMWARE_MANIFEST_URL[] =
    "http://replace-with-pi-address:8080/api/firmware/xiao/manifest";

// Used only when no PlatformIO room environment overrides it.
// 0 = Living Room, 1 = Kitchen
constexpr uint8_t DEVICE_LOCATION = 0;
static_assert(DEVICE_LOCATION <= 1, "DEVICE_LOCATION must be 0 or 1");
constexpr const char *DEVICE_NAME =
    DEVICE_LOCATION == 1 ? "Kitchen" : "Living Room";
