#pragma once

#include "secrets.h"

#ifdef DEVICE_LOCATION_OVERRIDE
constexpr uint8_t ACTIVE_DEVICE_LOCATION = DEVICE_LOCATION_OVERRIDE;
#else
constexpr uint8_t ACTIVE_DEVICE_LOCATION = DEVICE_LOCATION;
#endif

static_assert(ACTIVE_DEVICE_LOCATION <= 1,
              "DEVICE_LOCATION_OVERRIDE must be 0 or 1");
constexpr const char *ACTIVE_DEVICE_NAME =
    ACTIVE_DEVICE_LOCATION == 1 ? "Kitchen" : "Living Room";
