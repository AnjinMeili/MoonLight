// hub75_dma_pins.h -- active-3 board pinout for the Pi 3/4 DMA backend.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// DESIGN NOTE -- read before wondering where the DMA is: despite the
// backend's name, this engine drives the panel columns from the CPU and
// times OE with the hardware PWM pulser. Measured DMA GPIO rates on Pi 4
// (~1.5-1.8 MHz of GPIO operations, hzeller's rpi-gpio-dma-demo,
// oscilloscope-verified) put a DMA-streamed wall at 15-18 Hz computed --
// far below the 80-100 Hz flicker threshold -- while the CPU loop
// sustains ~78 ns per column (~250 Hz computed on the wall). See the
// design decision in MOONLIGHT-INTEGRATION.md ("Pi 3/4 (DMA)" section);
// the DMA control-block design stays in the research report
// (research/hzeller-dma-reference.md section 1) for a future revisit if
// hardware measurement ever overturns the DMA rate.
//
// Pin facts: the active-3 adapter's "regular" mapping -- the same board
// facts the Pi 5 PIO engine's hub75_pio_pins.h carries. The numbers are
// deliberately repeated here rather than shared: the Pi 5 side keeps its
// clean-room rule (MoonLight + Pimoroni Pico HUB75 only), so the Pi 4
// files carry their own pinout and no Pi 5 file is touched by this
// backend. If the board wiring ever changes, change both headers.
//
// Unlike the PIO engine, which drives GPIO 2..27 as one wide word, this
// engine writes the BCM GPIO set/clear registers directly, so the masks
// below are raw GPIO bit positions (1u << gpio), not wide-word bits.

#pragma once

#include <cstdint>

namespace mm::platform {

// ---------------------------------------------------------------------------
// Shared signals (fanned out to all three chains; buffered per connector
// on the active board).
// ---------------------------------------------------------------------------
namespace dma_shared {
//          GPIO   40-pin header pin
constexpr uint8_t kOe  = 18;  // pin 12, active low (panel OE-); PWM0-capable (ALT5), which the OE pulser needs
constexpr uint8_t kClk = 17;  // pin 11, pixel clock
constexpr uint8_t kStb = 4;   // pin  7, strobe / latch
constexpr uint8_t kA   = 22;  // pin 15, row address bit 0
constexpr uint8_t kB   = 23;  // pin 16, row address bit 1
constexpr uint8_t kC   = 24;  // pin 18, row address bit 2
constexpr uint8_t kD   = 25;  // pin 22, row address bit 3 (1/16 and up)
constexpr uint8_t kE   = 15;  // pin 10, row address bit 4 (1/32 only).
                              // E sits on HUB75 pin 4 or 8 depending on the
                              // panel; the board has a jumper for it.
}  // namespace dma_shared

// ---------------------------------------------------------------------------
// Per-chain colour signals. R1/G1/B1 feed the upper half of the panel,
// R2/G2/B2 the lower half.
// ---------------------------------------------------------------------------
struct DmaChainPins {
    uint8_t r1, g1, b1;  // upper half
    uint8_t r2, g2, b2;  // lower half
};

//          R1       G1       B1       R2       G2       B2
//          GPIO/hdr GPIO/hdr GPIO/hdr GPIO/hdr GPIO/hdr GPIO/hdr
constexpr DmaChainPins kDmaChainPins[3] = {
    {11, 27, 7, 8, 9, 10},    // chain 1: hdr 23, 13, 26, 24, 21, 19
    {12, 5, 6, 19, 13, 20},   // chain 2: hdr 32, 29, 31, 35, 33, 38
    {14, 2, 3, 26, 16, 21},   // chain 3: hdr  8,  3,  5, 37, 36, 40
};

// ---------------------------------------------------------------------------
// GPIO bit masks (raw GPIO positions). The dump loop writes these to
// GPSET0/GPCLR0; the setup writes the dark levels through them before
// any pin becomes an output (NOTE.md rule 1: values, then directions,
// then the mux).
// ---------------------------------------------------------------------------
constexpr uint32_t kDmaClkBit = (1u << dma_shared::kClk);
constexpr uint32_t kDmaStbBit = (1u << dma_shared::kStb);
constexpr uint32_t kDmaOeBit  = (1u << dma_shared::kOe);

// The five row-address lines as one mask.
constexpr uint32_t kDmaAddressMask = (1u << dma_shared::kA) | (1u << dma_shared::kB) |
                                     (1u << dma_shared::kC) | (1u << dma_shared::kD) |
                                     (1u << dma_shared::kE);

// All eighteen colour lines (three chains x six) as one mask.
constexpr uint32_t kDmaColourMask =
    (1u << kDmaChainPins[0].r1) | (1u << kDmaChainPins[0].g1) | (1u << kDmaChainPins[0].b1) |
    (1u << kDmaChainPins[0].r2) | (1u << kDmaChainPins[0].g2) | (1u << kDmaChainPins[0].b2) |
    (1u << kDmaChainPins[1].r1) | (1u << kDmaChainPins[1].g1) | (1u << kDmaChainPins[1].b1) |
    (1u << kDmaChainPins[1].r2) | (1u << kDmaChainPins[1].g2) | (1u << kDmaChainPins[1].b2) |
    (1u << kDmaChainPins[2].r1) | (1u << kDmaChainPins[2].g1) | (1u << kDmaChainPins[2].b1) |
    (1u << kDmaChainPins[2].r2) | (1u << kDmaChainPins[2].g2) | (1u << kDmaChainPins[2].b2);

// Every GPIO the board drives: colour + address + clock + latch + OE.
constexpr uint32_t kDmaAllPinsMask = kDmaColourMask | kDmaAddressMask | kDmaClkBit |
                                     kDmaStbBit | kDmaOeBit;

// The row-address lines for address `row` (0..31) as a GPIO mask: A is
// bit 0 of the row, E is bit 4.
constexpr uint32_t dmaAddressMaskForRow(uint8_t row) {
    uint32_t m = 0;
    if (row & 0x01) m |= (1u << dma_shared::kA);
    if (row & 0x02) m |= (1u << dma_shared::kB);
    if (row & 0x04) m |= (1u << dma_shared::kC);
    if (row & 0x08) m |= (1u << dma_shared::kD);
    if (row & 0x10) m |= (1u << dma_shared::kE);
    return m;
}

// GPFSEL register index (0..5) and bit shift for a GPIO pin: three bits
// per pin, ten pins per register (BCM2835 ARM Peripherals section 6).
constexpr int dmaGpfselReg(uint8_t gpio) { return gpio / 10; }
constexpr int dmaGpfselShift(uint8_t gpio) { return (gpio % 10) * 3; }

// GPFSEL function values (BCM2835 section 6.1).
constexpr uint32_t kDmaGpioInput = 0;   // 000: input
constexpr uint32_t kDmaGpioOutput = 1;  // 001: output
constexpr uint32_t kDmaGpioAlt5 = 2;    // 010: alternate function 5 (PWM0 on GPIO 18)

static_assert((kDmaColourMask & kDmaAddressMask) == 0, "colour and address pins are distinct");
static_assert((kDmaAllPinsMask & kDmaClkBit) != 0 && (kDmaAllPinsMask & kDmaStbBit) != 0 &&
                  (kDmaAllPinsMask & kDmaOeBit) != 0,
              "the shared lines are in the board mask");

}  // namespace mm::platform
