#pragma once

#include "secrets.h"

#ifdef DEVICE_LOCATION_OVERRIDE
constexpr uint8_t ACTIVE_DEVICE_LOCATION = DEVICE_LOCATION_OVERRIDE;
#if DEVICE_LOCATION_OVERRIDE == 1
constexpr char ACTIVE_DEVICE_NAME[] = "Kitchen";
#else
constexpr char ACTIVE_DEVICE_NAME[] = "Living Room";
#endif
#else
constexpr uint8_t ACTIVE_DEVICE_LOCATION = DEVICE_LOCATION;
constexpr const char *ACTIVE_DEVICE_NAME =
    DEVICE_LOCATION == 1 ? "Kitchen" : "Living Room";
#endif

static_assert(ACTIVE_DEVICE_LOCATION <= 1,
              "DEVICE_LOCATION_OVERRIDE must be 0 or 1");
