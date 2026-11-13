// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The HUB75 encode lives with the bus contract it builds, in platform/Hub75Encode.h, so the Pi 5 PIO engine (platform/) encodes with the same function as the ESP32 bus output (light/) without crossing the include boundary. This header stays, so the bus output's include is unchanged.
#include "platform/Hub75Encode.h"
