// hub75_pio_pins.h -- hzeller active-3 ("regular" mapping) pinout.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pin facts from hzeller rpi-rgb-led-matrix, lib/hardware-mapping.c:30-66
// (the `regular` mapping table) and wiring.md:96-175 (40-pin header table).
// The active-3 adapter uses the `regular` mapping: active vs passive is
// electrical (74HCT245 buffering), not a pinout difference
// (adapter/active-3/README.md:52-55). The Adafruit HAT mapping is a
// different single-chain pinout and is NOT used here.
//
// The PIO program drives GPIO 2..27 as one contiguous 26-pin OUT range
// (see hub75.pio). Every GPIO in 2..27 is used by the board, with no gaps,
// which is what makes the single-SM wide-word design possible.

#pragma once

#include <cstdint>

namespace mm::platform {

// ---------------------------------------------------------------------------
// Shared signals (fanned out to all three chains; buffered per connector
// on the active board).
// ---------------------------------------------------------------------------
namespace shared {
//          GPIO   40-pin header pin
constexpr uint8_t kOe  = 18;  // pin 12, active low (panel OE-)
constexpr uint8_t kClk = 17;  // pin 11, pixel clock (side-set)
constexpr uint8_t kStb = 4;   // pin  7, strobe / latch
constexpr uint8_t kA   = 22;  // pin 15, row address bit 0
constexpr uint8_t kB   = 23;  // pin 16, row address bit 1
constexpr uint8_t kC   = 24;  // pin 18, row address bit 2
constexpr uint8_t kD   = 25;  // pin 22, row address bit 3 (1/16 and up)
constexpr uint8_t kE   = 15;  // pin 10, row address bit 4 (1/32 only).
                              // E sits on HUB75 pin 4 or 8 depending on the
                              // panel; the board has a jumper for it.
}  // namespace shared

// ---------------------------------------------------------------------------
// Per-chain colour signals. Chain 1 = [1], chain 2 = [2], chain 3 = [3] in
// hzeller's wiring.md. R1/G1/B1 feed the upper half of the panel,
// R2/G2/B2 the lower half.
// ---------------------------------------------------------------------------
struct ChainPins {
    uint8_t r1, g1, b1;  // upper half
    uint8_t r2, g2, b2;  // lower half
};

//          R1       G1       B1       R2       G2       B2
//          GPIO/hdr GPIO/hdr GPIO/hdr GPIO/hdr GPIO/hdr GPIO/hdr
constexpr ChainPins kChainPins[3] = {
    {11, 27, 7, 8, 9, 10},    // chain 1: hdr 23, 13, 26, 24, 21, 19
    {12, 5, 6, 19, 13, 20},   // chain 2: hdr 32, 29, 31, 35, 33, 38
    {14, 2, 3, 26, 16, 21},   // chain 3: hdr  8,  3,  5, 37, 36, 40
};

// ---------------------------------------------------------------------------
// Wide-word layout. The PIO OUT range starts at GPIO 2 and covers 26 pins,
// so wide-word bit (gpio - 2) drives that GPIO (OUT shift right: word bit 0
// -> OUT base pin). The CLK bit is a don't-care -- side-set overrides OUT
// on GPIO 17 -- and the host writes 0 there.
// ---------------------------------------------------------------------------
constexpr uint8_t kOutBaseGpio = 2;
constexpr uint8_t kOutPinCount = 26;  // GPIO 2..27 inclusive
constexpr uint8_t kClkGpio = shared::kClk;  // side-set pin (overlaps OUT)

// Wide-word bit for a GPIO, or -1 if the GPIO is outside the OUT range.
constexpr int wideBitForGpio(uint8_t gpio) {
    return (gpio >= kOutBaseGpio && gpio < kOutBaseGpio + kOutPinCount)
               ? static_cast<int>(gpio - kOutBaseGpio)
               : -1;
}

// Wide-word bit for the shared lines (handy in tests and the repack).
constexpr int kWideBitOe  = 16;  // GPIO 18; INVERTED: set = blanked
constexpr int kWideBitStb = 2;   // GPIO 4
constexpr int kWideBitA   = 20;  // GPIO 22
constexpr int kWideBitB   = 21;  // GPIO 23
constexpr int kWideBitC   = 22;  // GPIO 24
constexpr int kWideBitD   = 23;  // GPIO 25
constexpr int kWideBitE   = 13;  // GPIO 15
constexpr int kWideBitClk = 15;  // GPIO 17; don't-care (side-set wins)

// A word that drives the panel dark: OE pin high (blanked), no colour,
// address 0, no latch, CLK bit 0. Used for the blank frame on shutdown.
constexpr uint32_t kBlankWideWord = (1u << kWideBitOe);

// Sanity: the OUT range must cover every pin the board uses.
static_assert(wideBitForGpio(shared::kOe) == kWideBitOe, "OE bit");
static_assert(wideBitForGpio(shared::kStb) == kWideBitStb, "STB bit");
static_assert(wideBitForGpio(shared::kA) == kWideBitA, "A bit");
static_assert(wideBitForGpio(shared::kB) == kWideBitB, "B bit");
static_assert(wideBitForGpio(shared::kC) == kWideBitC, "C bit");
static_assert(wideBitForGpio(shared::kD) == kWideBitD, "D bit");
static_assert(wideBitForGpio(shared::kE) == kWideBitE, "E bit");
static_assert(wideBitForGpio(shared::kClk) == kWideBitClk, "CLK bit");

}  // namespace mm::platform
