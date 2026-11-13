// hub75_pio_backend_stub.h -- test read-back for the stub PIO backend.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The stub backend keeps a copy of the last frame it was asked to submit,
// so an engine test can read back the wide words the refresh thread
// produced without any hardware. Production never calls these.

#pragma once

#include <cstddef>
#include <cstdint>

namespace mm::platform {

// Frames the stub backend has been asked to submit since the last reset.
uint64_t stubBackendSubmittedFrames();

// Copy the last submitted frame's wide words into `out`, at most
// `capWords` of them; the words copied, 0 when none has been submitted.
// The copy is one submitted frame whole: the refresh thread cannot
// replace it mid-copy.
size_t stubBackendCopyLastFrame(uint32_t* out, size_t capWords);

// Drop the captured frame and reset the submit count.
void stubBackendResetCapture();

}  // namespace mm::platform
