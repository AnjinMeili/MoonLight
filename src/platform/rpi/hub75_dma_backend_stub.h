// hub75_dma_backend_stub.h -- test capture for the stub DMA backend.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The stub backend keeps a thread-safe copy of the last submitted GPIO
// frame, a submit count, and the setup-step log, so an engine test can
// read back golden words and pin the dark-first setup order (NOTE.md
// rule 1) without hardware. All accessors take the capture lock; the
// refresh thread submits while the test reads.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace mm::platform {

// Frames the stub has submitted since the last reset.
uint64_t dmaStubSubmittedFrames();

// Copy the last submitted frame's GPIO words into `out` (at most
// `capWords`); returns the words copied, 0 when nothing was submitted.
size_t dmaStubCopyLastFrame(uint32_t* out, size_t capWords);

// The setup steps the stub's open()/start() ran, in order, comma
// separated: "dark-values,directions,pwm-config,oe-mux" for a full
// open + start. The hardware backend runs the same four steps in the
// same order; the engine test pins this string.
std::string dmaStubSetupLog();

// Drop the capture, the count and the setup log.
void dmaStubResetCapture();

}  // namespace mm::platform
