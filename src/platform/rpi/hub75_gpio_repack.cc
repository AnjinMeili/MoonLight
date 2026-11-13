// hub75_gpio_repack.cc -- see hub75_gpio_repack.h.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/rpi/hub75_gpio_repack.h"

#include <cassert>

#include "platform/Hub75Bus.h"   // Hub75Layout: the default the repack pins
#include "platform/rpi/hub75_gpio_pins.h"

// hub75Encode writes little-endian slot words; the repack reads them back
// as uint16_t. Both ARM (Pi) and x86 Linux are little-endian; fail loudly
// anywhere else rather than silently byte-swapping the bus.
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "hub75 GPIO repack assumes a little-endian host");

// The repack hard-codes the default Hub75Layout; if the default ever moves,
// the tables below would silently scramble the bus, so pin it here.
namespace {
constexpr mm::Hub75Layout kDefaultLayout{};
static_assert(kDefaultLayout.r1 == 0 && kDefaultLayout.g1 == 1 && kDefaultLayout.b1 == 2 &&
                  kDefaultLayout.r2 == 3 && kDefaultLayout.g2 == 4 && kDefaultLayout.b2 == 5 &&
                  kDefaultLayout.a == 8 && kDefaultLayout.b == 9 && kDefaultLayout.c == 10 &&
                  kDefaultLayout.d == 11 && kDefaultLayout.e == 12 && kDefaultLayout.lat == 13 &&
                  kDefaultLayout.oe == 14,
              "hub75 GPIO repack assumes the default Hub75Layout");
}  // namespace

namespace mm::platform {

namespace {

// MoonLight default Hub75Layout bit positions in the 16-bit slot word
// (platform/Hub75Bus.h). The repack hard-codes the default layout; see the
// header.
constexpr int kSlotR1 = 0, kSlotG1 = 1, kSlotB1 = 2;
constexpr int kSlotR2 = 3, kSlotG2 = 4, kSlotB2 = 5;

// The six colour slot bits, in the order they index the colour table.
constexpr int kColourSlotBits[6] = {kSlotR1, kSlotG1, kSlotB1,
                                    kSlotR2, kSlotG2, kSlotB2};

}  // namespace

GpioRepackTables buildGpioRepackTables() {
    GpioRepackTables t{};
    // Colour tables: for each chain, each combination of its six colour
    // bits maps to the corresponding GPIO bits in the data word.
    for (int c = 0; c < kGpioMaxChains; c++) {
        const uint8_t gpios[6] = {kGpioChainPins[c].r1, kGpioChainPins[c].g1,
                                  kGpioChainPins[c].b1, kGpioChainPins[c].r2,
                                  kGpioChainPins[c].g2, kGpioChainPins[c].b2};
        for (int bits = 0; bits < 64; bits++) {
            uint32_t mask = 0;
            for (int i = 0; i < 6; i++)
                if (bits & (1 << kColourSlotBits[i]))
                    mask |= (1u << gpios[i]);
            t.colour[c][bits] = mask;
        }
    }
    return t;
}

void repackSlotsToGpio(const uint16_t* const chainSlots[kGpioMaxChains], int chains,
                       size_t slots, const GpioRepackTables& tables, uint32_t* gpioOut) {
    assert(chains >= 1 && chains <= kGpioMaxChains);
    assert(chainSlots[0] != nullptr && gpioOut != nullptr);
    for (size_t i = 0; i < slots; i++) {
        uint32_t w = tables.colour[0][chainSlots[0][i] & 0x3F];
        if (chains > 1 && chainSlots[1] != nullptr)
            w |= tables.colour[1][chainSlots[1][i] & 0x3F];
        if (chains > 2 && chainSlots[2] != nullptr)
            w |= tables.colour[2][chainSlots[2][i] & 0x3F];
        gpioOut[i] = w;
    }
}

uint32_t packGpioWordReference(uint16_t slot0, uint16_t slot1, uint16_t slot2,
                               int chains) {
    const uint16_t slotByChain[3] = {slot0, slot1, slot2};
    uint32_t w = 0;
    for (int c = 0; c < chains && c < kGpioMaxChains; c++) {
        const uint16_t s = slotByChain[c];
        const uint8_t gpios[6] = {kGpioChainPins[c].r1, kGpioChainPins[c].g1,
                                  kGpioChainPins[c].b1, kGpioChainPins[c].r2,
                                  kGpioChainPins[c].g2, kGpioChainPins[c].b2};
        for (int i = 0; i < 6; i++)
            if (s & (1u << kColourSlotBits[i]))
                w |= (1u << gpios[i]);
    }
    return w;
}

}  // namespace mm::platform
