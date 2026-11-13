// hub75_gpio_pwm.h -- the hardware PWM OE pulser for the Pi 3/4 GPIO backend.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The panel's OE (output enable) line is timed by the BCM PWM peripheral,
// not by the CPU and not by DMA: the CPU fires one pulse per bitplane and
// clocks the next row's data in while the pulse runs, so clock-in time
// hides inside OE on-time. This is the architecture hzeller's
// rpi-rgb-led-matrix uses for its classic Pi 1-4 backend (its
// HardwarePinPulser); the design here is written from scratch from the
// BCM2835/BCM2711 datasheets and the published timing numbers -- no
// hzeller code is adapted (every file in its lib/ carries a GPL-2.0-only
// header, which cannot combine into this GPL-3.0-or-later tree; see
// MOONLIGHT-INTEGRATION.md for the license check).
//
// How it works (BCM2835 ARM Peripherals section 9):
//
//   - Clock: PLLD at 500 MHz, divided by (lsbNs / 4). One PWM tick is
//     divider x 2 ns; the LSB pulse is always exactly 2 ticks, so the
//     actual LSB time is divider x 4 ns (130 ns asks divider 32, tick
//     64 ns, LSB 128 ns -- a 1.5% quantisation error, the same as the
//     reference design's).
//   - Channel 1 (PWM0) in FIFO mode with inverted polarity: CTL =
//     USEF1 | PWEN1 | POLA1. Inverted because OE is active-low: a FIFO
//     word equal to the range drives the pin low (OE active, LEDs on)
//     for one full PWM period. The silence bit stays 0, so when the
//     FIFO empties the pin returns high (OE inactive, LEDs off).
//   - A pulse for bitplane p lasts 2^(p+1) ticks. Ranges below 16 go
//     out as one FIFO word holding the full range; ranges of 16 or
//     more are split into 8 FIFO words of range/8 each, keeping the
//     range register small so the trailing zero phase is short. Two
//     zero-valued sentinel words follow: one returns the output to its
//     default state, and the second makes the "FIFO empty" detection
//     reliable (there appears to be a buffering register behind the
//     FIFO -- the same observation the reference design's comment
//     records).
//   - The PWM output reaches the outside world only on GPIO 18 (ALT5)
//     or GPIO 12 (ALT0). The board puts OE on GPIO 18, so the pulser
//     needs no fallback; a board that moved OE elsewhere could not use
//     this pulser at all.
//
// Conflict: the onboard analogue audio driver (snd_bcm2835) uses the
// same PWM hardware. The backend refuses to open while it is loaded --
// see hub75_gpio_backend_hw.cpp -- since two drivers programming one PWM
// block would fight over the clock and the FIFO.

#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace mm::platform {

// ---------------------------------------------------------------------------
// PWM block registers (offsets within the PWM block; BCM2835 section 9.6).
// ---------------------------------------------------------------------------
constexpr uint32_t kGpioPwmCtlOffset = 0x00;   // control
constexpr uint32_t kGpioPwmStaOffset = 0x04;   // status
constexpr uint32_t kGpioPwmRng1Offset = 0x10;  // channel 1 range
constexpr uint32_t kGpioPwmFif1Offset = 0x18;  // FIFO input

// PWM_CTL bits, channel 1.
constexpr uint32_t kGpioPwmCtlPwen1 = (1u << 0);  // channel enable
constexpr uint32_t kGpioPwmCtlPola1 = (1u << 4);  // invert output polarity
constexpr uint32_t kGpioPwmCtlUsef1 = (1u << 5);  // use FIFO for data
constexpr uint32_t kGpioPwmCtlClrf1 = (1u << 6);  // clear FIFO (self-clearing)

// PWM_STA bits, channel 1.
constexpr uint32_t kGpioPwmStaEmpt1 = (1u << 1);  // FIFO empty

// ---------------------------------------------------------------------------
// Clock manager registers for the PWM clock (BCM2835 section 6.3; the
// clock manager's registers are 4 bytes each, so register N sits at
// offset N*4).
// ---------------------------------------------------------------------------
constexpr uint32_t kGpioClkPwmCtlOffset = 40 * 4;  // CM_PWMCTL
constexpr uint32_t kGpioClkPwmDivOffset = 41 * 4;  // CM_PWMDIV
constexpr uint32_t kGpioClkPassword = (0x5Au << 24);  // every clock write carries this
constexpr uint32_t kGpioClkSrcPlld = 6;               // PLLD, 500 MHz
constexpr uint32_t kGpioClkCtlEnab = (1u << 4);
constexpr uint32_t kGpioClkCtlKill = (1u << 5);
constexpr uint32_t kGpioClkCtlBusy = (1u << 7);
constexpr uint32_t kGpioPwmClockHz = 500'000'000;  // PLLD

// The LSB time's bounds, as the reference design validates them.
constexpr uint16_t kGpioPwmLsbMinNs = 50;
constexpr uint16_t kGpioPwmLsbMaxNs = 3000;

/// The PWM clocking a requested LSB time resolves to: the divider, the
/// tick it makes, and the LSB time the hardware actually produces.
struct GpioPwmTiming {
    uint16_t divider = 32;     ///< PLLD divider, 1..4095 (12-bit field)
    uint16_t tickNs = 64;      ///< one PWM tick in ns (divider x 2 ns)
    uint16_t lsbActualNs = 128;  ///< the LSB pulse in ns (2 ticks)
};

// The clocking for `lsbNs`: divider = lsbNs / 4, truncated, clamped to
// the 12-bit field. Pure function of the request; the unit test pins the
// 50 / 130 / 3000 ns cases.
GpioPwmTiming hub75GpioPwmTiming(uint16_t lsbNs);

/// PWM ticks in bitplane p's pulse: the LSB pulse is 2 ticks, doubling
/// per plane. Plane 0..10 (the encoder's depth caps at 6, so p <= 5 in
/// practice).
inline uint16_t hub75GpioPulseTicks(uint8_t plane) {
    return static_cast<uint16_t>(2u << plane);
}

/// One pulse as the FIFO takes it: the range register's value, the FIFO
/// words (the pulse split, then two zero sentinels), and their count.
struct GpioPulseWords {
    uint16_t range = 0;
    uint16_t words[10] = {};
    uint8_t count = 0;
};

// The FIFO words for bitplane p's pulse. Pure function of the plane;
// the unit test pins the split boundary (15 vs 16 ticks) and the top
// plane. Ranges below 16 go out as one word; 16 and up split into 8
// words of range/8, then two zero sentinels.
GpioPulseWords hub75GpioPulseWords(uint8_t plane);

/// The OE pulser over mapped PWM + clock registers. One instance per
/// backend; the backend owns the mappings and hands the pulser the
/// register bases. All register access is through volatile pointers, so
/// the compiler never elides or reorders a write the hardware must see.
class GpioPwmPulser {
  public:
    GpioPwmPulser() = default;
    GpioPwmPulser(const GpioPwmPulser&) = delete;
    GpioPwmPulser& operator=(const GpioPwmPulser&) = delete;

    // Program the PWM clock for `lsbNs` and set the channel up in FIFO
    // mode, inverted, FIFO cleared, output idle high (dark). Fills
    // `error` and returns false if the clock does not stop or start
    // within its timeout. `pwm`/`clk` are the mapped PWM and clock
    // manager blocks; they must stay mapped until stop().
    bool configure(volatile uint32_t* pwm, volatile uint32_t* clk, uint16_t lsbNs,
                   std::string& error);

    // Fire the OE pulse for bitplane p: set the range, write the FIFO
    // words. The pulse runs asynchronously; the caller clocks the next
    // row's data in while it runs, then waits for it (below) before
    // latching. Only call once the previous pulse has finished (the
    // FIFO must be empty, or the words queue behind the live pulse).
    void sendPulse(uint8_t plane);

    // Wait until the fired pulse has finished (the FIFO is empty, so
    // the output is back to dark). Sleeps until near the pulse's
    // computed end, then polls the FIFO-empty bit; returns false after
    // a 2 ms grace past the end, in which case the caller proceeds
    // anyway (a stuck PWM must not hang the refresh thread -- the
    // backend counts the timeout).
    bool waitPulseFinished();

    // Disable the channel and clear the FIFO. The clock keeps running;
    // the next configure() reprograms it. The OE pin's level after
    // stop() is the caller's business -- the backend muxes OE back to
    // a GPIO output driving high (dark) before calling this.
    void stop();

    bool configured() const { return pwm_ != nullptr; }
    GpioPwmTiming timing() const { return timing_; }

  private:
    volatile uint32_t* pwm_ = nullptr;
    volatile uint32_t* clk_ = nullptr;
    GpioPwmTiming timing_{};
    std::chrono::steady_clock::time_point pulseEnd_{};
};

}  // namespace mm::platform
