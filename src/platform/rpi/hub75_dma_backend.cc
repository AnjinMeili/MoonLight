// hub75_dma_backend.cc -- the DmaBackend factory.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/rpi/hub75_dma_backend.h"

namespace mm::platform {

DmaBackend* createDmaBackend(bool preferStub) {
    if (preferStub) return createDmaStubBackend();
    return createDmaHwBackend();
}

}  // namespace mm::platform
