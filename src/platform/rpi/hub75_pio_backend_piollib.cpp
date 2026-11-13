// hub75_pio_backend_piollib.cpp -- PioBackend on Raspberry Pi's piolib.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Built only when piolib is available (see the root CMakeLists.txt); the
// engine links against the SYSTEM piolib, it is not vendored here. piolib
// is BSD-3-Clause, Copyright (c) 2023 Raspberry Pi Ltd.
// (raspberrypi/utils, piolib/).
//
// The piolib calls go through hub75_pio_c_api.* (compiled as C) because
// piolib.h is C-only and does not compile as C++.
//
// API notes (verified against raspberrypi/utils master, piolib.h and
// pio_rp1.c):
//   - `pio0` is pio_open_helper(0) and calls exit(1) if /dev/pio0 cannot
//     be opened, so open() checks the device node first and fails cleanly.
//   - pio_enable_fatal_errors(pio, false) turns piolib's error paths
//     (e.g. pio_add_program with no program space) into return values
//     plus pio_get_error(), instead of aborting the process.
//   - There is deliberately no use of pio_sm_put() on the hot path: each
//     put is a mailbox RPC (>= 10 us), far too slow to stream pixels.
//     Frames go through the synchronous DMA transfer (pio_sm_xfer_data),
//     which blocks until the whole frame has been delivered to the FIFO;
//     see hub75_pio_backend.h for why the engine relies on that.

#include "platform/rpi/hub75_pio_backend.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>

#include "platform/rpi/hub75_pio_c_api.h"
#include "platform/rpi/hub75_pio_pins.h"

namespace mm::platform {

namespace {

constexpr const char* kPioDevice = "/dev/pio0";

// How long blankAndStop() waits for the TX FIFO to drain after the blank
// frame before giving up and disabling the SM anyway. The FIFO holds at
// most 8 words; at any sane pixel clock it drains in microseconds.
constexpr int kFifoDrainTimeoutMs = 100;

class PiolibBackend : public PioBackend {
  public:
    bool open(std::string& error) override {
        if (access(kPioDevice, R_OK | W_OK) != 0) {
            error = std::string("cannot open ") + kPioDevice + ": " +
                    std::strerror(errno) +
                    " (is this a Pi 5 with a recent kernel/firmware, and "
                    "are you in the gpio group?)";
            return false;
        }
        pio_ = hub75_pio_open();  // cannot fail after the access() check
        if (pio_ == nullptr) {
            error = "pio_open failed";
            return false;
        }
        sm_ = hub75_pio_claim_sm(pio_);
        if (sm_ < 0) {
            error = "no free RP1 PIO state machine (all 4 claimed)";
            hub75_pio_release(pio_, -1);
            pio_ = nullptr;
            return false;
        }
        opened_ = true;
        return true;
    }

    bool start(const uint16_t* programInstructions, int programLength,
               int wrapTarget, int wrap, uint16_t clkdivInt,
               uint8_t clkdivFrac, uint32_t xferBufSize,
               uint32_t xferBufCount, std::string& error) override {
        if (!opened_) {
            error = "piolib backend not opened";
            return false;
        }
        // DMA ring first (piotest.c configures the transfer before
        // loading the program).
        if (hub75_pio_config_xfer(pio_, sm_, xferBufSize, xferBufCount) != 0) {
            error = "pio_sm_config_xfer failed (buf_size=" +
                    std::to_string(xferBufSize) +
                    " buf_count=" + std::to_string(xferBufCount) + ")";
            return false;
        }
        offset_ = hub75_pio_add_program(pio_, programInstructions, programLength);
        if (offset_ < 0) {
            error = "pio_add_program failed: no instruction memory free "
                    "(the program needs 3 of 32 words)";
            return false;
        }
        programInstructions_ = programInstructions;
        programLength_ = programLength;
        programLoaded_ = true;

        if (hub75_pio_init_sm(pio_, sm_, offset_, wrapTarget, wrap, clkdivInt,
                              clkdivFrac, kBlankWideWord) != 0) {
            error = "PIO SM configuration failed";
            return false;
        }
        hub75_pio_clear_fifos(pio_, sm_);
        hub75_pio_set_enabled(pio_, sm_, 1);
        started_ = true;
        return true;
    }

    int submitFrame(const void* data, uint32_t bytes) override {
        if (!started_) return -ENODEV;
        // The kernel copies from the buffer; the transfer does not modify it.
        return hub75_pio_xfer(pio_, sm_, const_cast<void*>(data), bytes);
    }

    bool blankAndStop(const void* blankData, uint32_t blankBytes,
                      std::string& error) override {
        bool ok = true;
        if (started_) {
            // 1. Drive the panel dark: submit the blank frame and wait
            //    for it to be delivered (xfer is synchronous).
            const int rc = submitFrame(blankData, blankBytes);
            if (rc != 0) {
                error = "blank frame submit failed (" + std::to_string(rc) +
                        "); continuing with shutdown";
                ok = false;
            }
            // 2. Wait for the TX FIFO to drain, so the SM has shifted
            //    out the last blank word and is stalled on `pull` with
            //    the pins holding the dark word.
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(kFifoDrainTimeoutMs);
            while (!hub75_pio_tx_fifo_empty(pio_, sm_)) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    if (error.empty())
                        error = "TX FIFO did not drain within timeout; "
                                "disabling SM anyway";
                    ok = false;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            // 3. Blank is established; now it is safe to stop the SM.
            //    The pins hold the dark word (PIO output registers keep
            //    their values when the SM is disabled).
            hub75_pio_set_enabled(pio_, sm_, 0);
            hub75_pio_clear_fifos(pio_, sm_);
            started_ = false;
        }
        if (programLoaded_) {
            hub75_pio_remove_program(pio_, programInstructions_, programLength_,
                                     offset_);
            programLoaded_ = false;
        }
        return ok;
    }

    void close() override {
        if (started_) {
            // No blank frame available here; disable directly. The
            // engine always calls blankAndStop() before close() on the
            // clean path, so this is only the error path.
            hub75_pio_set_enabled(pio_, sm_, 0);
            started_ = false;
        }
        if (programLoaded_) {
            hub75_pio_remove_program(pio_, programInstructions_, programLength_,
                                     offset_);
            programLoaded_ = false;
        }
        if (opened_) {
            hub75_pio_release(pio_, sm_);
            pio_ = nullptr;
            sm_ = -1;
            opened_ = false;
        }
    }

    const char* name() const override { return "piolib (/dev/pio0)"; }

  private:
    void* pio_ = nullptr;
    int sm_ = -1;
    int offset_ = -1;
    const uint16_t* programInstructions_ = nullptr;
    int programLength_ = 0;
    bool opened_ = false;
    bool started_ = false;
    bool programLoaded_ = false;
};

}  // namespace

PioBackend* createPiolibBackend() { return new PiolibBackend(); }

}  // namespace mm::platform
