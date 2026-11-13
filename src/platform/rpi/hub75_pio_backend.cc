// hub75_pio_backend.cc -- see hub75_pio_backend.h.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/rpi/hub75_pio_backend.h"

namespace mm::platform {

PioBackend* createPioBackend(bool preferStub) {
    if (preferStub) return createStubBackend();
#ifdef MM_HUB75_PIO_HAVE_PIOLIB
    return createPiolibBackend();
#else
    return nullptr;   // no piolib in this build: the engine says so
#endif
}

}  // namespace mm::platform
