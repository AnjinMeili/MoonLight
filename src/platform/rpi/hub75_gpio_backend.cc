// hub75_gpio_backend.cc -- the GpioBackend factory.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/rpi/hub75_gpio_backend.h"

namespace mm::platform {

GpioBackend* createGpioBackend(bool preferStub) {
    if (preferStub) return createGpioStubBackend();
    return createGpioHwBackend();
}

}  // namespace mm::platform
