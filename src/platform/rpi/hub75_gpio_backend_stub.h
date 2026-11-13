// hub75_gpio_backend_stub.h -- test capture for the stub GPIO backend.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The stub backend keeps a thread-safe copy of the last submitted GPIO
// frame, the dump stream for that frame (every GPIO write and pulser
// step the shared dump loop made, in order), a submit count, and the
// setup-step log, so an engine test can read back golden words, pin the
// dump sequence the hardware backend would drive, and pin the dark-first
// setup order (NOTE.md rule 1) without hardware. All accessors take the
// capture lock; the refresh thread submits while the test reads.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "platform/rpi/hub75_gpio_dump.h"   // GpioDumpEvent

namespace mm::platform {

// Frames the stub has submitted since the last reset.
uint64_t gpioStubSubmittedFrames();

// Copy the last submitted frame's GPIO words into `out` (at most
// `capWords`); returns the words copied, 0 when nothing was submitted.
size_t gpioStubCopyLastFrame(uint32_t* out, size_t capWords);

// Copy the last submitted frame's dump stream -- the GPIO writes and
// pulser steps the dump loop made, in order -- into `out` (at most
// `capEvents`); returns the events copied, 0 when nothing was submitted.
// The hardware backend runs the same loop against the registers, so the
// stream is the sequence the panel would have seen: the test decodes it
// into clock edges, strobes, address changes and OE pulses.
size_t gpioStubCopyLastDumpStream(GpioDumpEvent* out, size_t capEvents);

// The setup steps the stub's open()/start() ran, in order, comma
// separated: "dark-values,directions,pwm-config,oe-mux" for a full
// open + start. The hardware backend runs the same four steps in the
// same order; the engine test pins this string.
std::string gpioStubSetupLog();

// Drop the capture, the count and the setup log.
void gpioStubResetCapture();

}  // namespace mm::platform
