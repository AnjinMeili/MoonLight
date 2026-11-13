// hub75_dma_pwm.cc -- see hub75_dma_pwm.h.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/rpi/hub75_dma_pwm.h"

#include <algorithm>
#include <thread>

namespace mm::platform {

namespace {

// Bounded waits on hardware bits: poll `tries` times, sleeping `us`
// microseconds between polls. No unbounded loop touches hardware -- a
// wedged clock must fail the open, not hang the process.
constexpr int kClockPollTries = 1000;
constexpr int kClockPollUs = 1;   // ~1 ms total

bool waitClockBit(volatile uint32_t* clk, uint32_t bit, bool set) {
    for (int i = 0; i < kClockPollTries; i++) {
        const bool isSet = (clk[kDmaClkPwmCtlOffset / 4] & bit) != 0;
        if (isSet == set) return true;
        std::this_thread::sleep_for(std::chrono::microseconds(kClockPollUs));
    }
    return false;
}

}  // namespace

DmaPwmTiming hub75DmaPwmTiming(uint16_t lsbNs) {
    // Divider = lsbNs / 4, truncated, clamped to the 12-bit field.
    // The tick is divider x 2 ns (500 MHz source); the LSB pulse is 2
    // ticks, so the actual LSB is divider x 4 ns.
    uint32_t divider = lsbNs / 4;
    if (divider < 1) divider = 1;
    if (divider > 4095) divider = 4095;
    DmaPwmTiming t;
    t.divider = static_cast<uint16_t>(divider);
    t.tickNs = static_cast<uint16_t>(divider * 2);
    t.lsbActualNs = static_cast<uint16_t>(divider * 4);
    return t;
}

DmaPulseWords hub75DmaPulseWords(uint8_t plane) {
    const uint16_t ticks = hub75DmaPulseTicks(plane);
    DmaPulseWords w;
    if (ticks < 16) {
        // One FIFO word holding the full range, then the two sentinels.
        w.range = ticks;
        w.words[0] = ticks;
        w.words[1] = 0;
        w.words[2] = 0;
        w.count = 3;
    } else {
        // Eight words of range/8 (ticks is a power of two >= 16, so the
        // split is exact), then the two sentinels.
        w.range = static_cast<uint16_t>(ticks / 8);
        for (int i = 0; i < 8; i++) w.words[i] = w.range;
        w.words[8] = 0;
        w.words[9] = 0;
        w.count = 10;
    }
    return w;
}

bool DmaPwmPulser::configure(volatile uint32_t* pwm, volatile uint32_t* clk,
                             uint16_t lsbNs, std::string& error) {
    if (pwm == nullptr || clk == nullptr) {
        error = "PWM pulser has no register mapping";
        return false;
    }
    timing_ = hub75DmaPwmTiming(lsbNs);

    // Stop the PWM clock: KILL, then wait for BUSY to clear. Every
    // clock-manager write carries the password in the top byte.
    clk[kDmaClkPwmCtlOffset / 4] = kDmaClkPassword | kDmaClkCtlKill;
    if (!waitClockBit(clk, kDmaClkCtlBusy, false)) {
        error = "the PWM clock did not stop (BUSY stayed set)";
        return false;
    }
    // Divider, then source + enable. The divider write carries the
    // password too; DIVI is bits 12..23, DIVF is 0 (integer divider).
    clk[kDmaClkPwmDivOffset / 4] =
        kDmaClkPassword | (static_cast<uint32_t>(timing_.divider) << 12);
    clk[kDmaClkPwmCtlOffset / 4] =
        kDmaClkPassword | kDmaClkSrcPlld | kDmaClkCtlEnab;
    if (!waitClockBit(clk, kDmaClkCtlBusy, true)) {
        error = "the PWM clock did not start (BUSY stayed clear)";
        return false;
    }

    pwm_ = pwm;
    clk_ = clk;

    // Channel setup, FIFO mode, inverted polarity, FIFO cleared. The
    // channel is disabled while the control register is rewritten, so
    // no partial configuration reaches the pin.
    pwm_[kDmaPwmCtlOffset / 4] = 0;
    pwm_[kDmaPwmCtlOffset / 4] = kDmaPwmCtlClrf1;   // clear the FIFO
    pwm_[kDmaPwmCtlOffset / 4] =
        kDmaPwmCtlUsef1 | kDmaPwmCtlPwen1 | kDmaPwmCtlPola1;
    pulseEnd_ = std::chrono::steady_clock::now();
    return true;
}

void DmaPwmPulser::sendPulse(uint8_t plane) {
    if (pwm_ == nullptr) return;
    const DmaPulseWords w = hub75DmaPulseWords(plane);
    // Range first: the FIFO words are consumed one period at a time
    // against the current range, and the FIFO is empty (the caller
    // waited for the previous pulse), so the new range is in place
    // before the first word is consumed.
    pwm_[kDmaPwmRng1Offset / 4] = w.range;
    for (int i = 0; i < w.count; i++)
        pwm_[kDmaPwmFif1Offset / 4] = w.words[i];
    // The pulse ends ticks x tickNs from now; the wait sleeps until
    // near it and then polls.
    const uint64_t ns = static_cast<uint64_t>(hub75DmaPulseTicks(plane)) * timing_.tickNs;
    pulseEnd_ = std::chrono::steady_clock::now() + std::chrono::nanoseconds(ns);
}

bool DmaPwmPulser::waitPulseFinished() {
    if (pwm_ == nullptr) return true;
    // Sleep until near the pulse's computed end, so a long pulse (a
    // large LSB time on a top plane) does not spin the refresh thread;
    // short pulses are already over, and the sleep is skipped.
    const auto now = std::chrono::steady_clock::now();
    const auto sleepUntil = pulseEnd_ - std::chrono::microseconds(30);
    if (now < sleepUntil)
        std::this_thread::sleep_until(sleepUntil);
    // Then poll the FIFO-empty bit until it sets, with a 2 ms grace
    // past the computed end. The FIFO drains only after the two zero
    // sentinels, so an empty FIFO means the output is dark again.
    const auto deadline = pulseEnd_ + std::chrono::milliseconds(2);
    while ((pwm_[kDmaPwmStaOffset / 4] & kDmaPwmStaEmpt1) == 0) {
        if (std::chrono::steady_clock::now() > deadline) return false;
    }
    return true;
}

void DmaPwmPulser::stop() {
    if (pwm_ == nullptr) return;
    pwm_[kDmaPwmCtlOffset / 4] = 0;
    pwm_[kDmaPwmCtlOffset / 4] = kDmaPwmCtlClrf1;
    pwm_ = nullptr;
    clk_ = nullptr;
}

}  // namespace mm::platform
