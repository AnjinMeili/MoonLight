// hub75_pio_repack.cc -- see hub75_pio_repack.h.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/rpi/hub75_pio_repack.h"

#include <cassert>

#include "platform/Hub75Bus.h"   // Hub75Layout: the default the repack pins
#include "platform/rpi/hub75_pio_pins.h"

// hub75Encode writes little-endian slot words; the repack reads them back
// as uint16_t. Both ARM (Pi) and x86 Linux are little-endian; fail loudly
// anywhere else rather than silently byte-swapping the bus.
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "hub75 repack assumes a little-endian host");

// The repack hard-codes the default Hub75Layout; if the default ever moves,
// the tables below would silently scramble the bus, so pin it here.
namespace {
constexpr mm::Hub75Layout kDefaultLayout{};
static_assert(kDefaultLayout.r1 == 0 && kDefaultLayout.g1 == 1 && kDefaultLayout.b1 == 2 &&
                  kDefaultLayout.r2 == 3 && kDefaultLayout.g2 == 4 && kDefaultLayout.b2 == 5 &&
                  kDefaultLayout.a == 8 && kDefaultLayout.b == 9 && kDefaultLayout.c == 10 &&
                  kDefaultLayout.d == 11 && kDefaultLayout.e == 12 && kDefaultLayout.lat == 13 &&
                  kDefaultLayout.oe == 14,
              "hub75 repack assumes the default Hub75Layout");
}  // namespace

namespace mm::platform {

namespace {

// MoonLight default Hub75Layout bit positions in the 16-bit slot word
// (platform/Hub75Bus.h). The repack hard-codes the default layout; see the
// header.
constexpr int kSlotR1 = 0, kSlotG1 = 1, kSlotB1 = 2;
constexpr int kSlotR2 = 3, kSlotG2 = 4, kSlotB2 = 5;
constexpr int kSlotA = 8, kSlotB = 9, kSlotC = 10, kSlotD = 11, kSlotE = 12;
constexpr int kSlotLat = 13, kSlotOe = 14;

// The six colour slot bits, in the order they index the colour table.
constexpr int kColourSlotBits[6] = {kSlotR1, kSlotG1, kSlotB1,
                                    kSlotR2, kSlotG2, kSlotB2};

}  // namespace

RepackTables buildRepackTables() {
    RepackTables t{};
    // Colour tables: for each chain, each combination of its six colour
    // bits maps to the corresponding GPIO bits in the wide word.
    for (int c = 0; c < kMaxChains; c++) {
        const uint8_t gpios[6] = {kChainPins[c].r1, kChainPins[c].g1,
                                  kChainPins[c].b1, kChainPins[c].r2,
                                  kChainPins[c].g2, kChainPins[c].b2};
        for (int bits = 0; bits < 64; bits++) {
            uint32_t mask = 0;
            for (int i = 0; i < 6; i++)
                if (bits & (1 << kColourSlotBits[i]))
                    mask |= (1u << wideBitForGpio(gpios[i]));
            t.colour[c][bits] = mask;
        }
    }
    // Control table: slot bits 8..14 (a, b, c, d, e, lat, oe) map to the
    // shared GPIO bits. OE is inverted: the slot bit means LIT, the pin
    // is active-low, so the pin bit is set when the slot bit is clear.
    const int slotBits[7] = {kSlotA, kSlotB, kSlotC, kSlotD,
                             kSlotE, kSlotLat, kSlotOe};
    const int wideBits[7] = {kWideBitA, kWideBitB, kWideBitC, kWideBitD,
                             kWideBitE, kWideBitStb, kWideBitOe};
    for (int bits = 0; bits < 128; bits++) {
        uint32_t mask = 0;
        for (int i = 0; i < 7; i++) {
            const bool slotSet = (bits & (1 << (slotBits[i] - 8))) != 0;
            // Index 6 is OE: invert it. The rest pass through.
            const bool pinSet = (i == 6) ? !slotSet : slotSet;
            if (pinSet) mask |= (1u << wideBits[i]);
        }
        t.control[bits] = mask;
    }
    return t;
}

void repackSlots(const uint16_t* const chainSlots[kMaxChains], int chains,
                 size_t slots, const RepackTables& tables, uint32_t* wideOut) {
    assert(chains >= 1 && chains <= kMaxChains);
    assert(chainSlots[0] != nullptr && wideOut != nullptr);
    for (size_t i = 0; i < slots; i++) {
        const uint16_t s0 = chainSlots[0][i];
        uint32_t w = tables.colour[0][s0 & 0x3F] | tables.control[(s0 >> 8) & 0x7F];
        if (chains > 1 && chainSlots[1] != nullptr)
            w |= tables.colour[1][chainSlots[1][i] & 0x3F];
        if (chains > 2 && chainSlots[2] != nullptr)
            w |= tables.colour[2][chainSlots[2][i] & 0x3F];
        wideOut[i] = w;
    }
}

void applyLatchBlanking(uint32_t* wide, size_t slots, size_t rowWords,
                        int blankWords) {
    if (blankWords <= 0 || rowWords == 0) return;
    assert(wide != nullptr);
    // Each row group is rowWords slots; its last slot is the latch word.
    // Blank the `blankWords` slots before it (set the OE pin bit).
    // k < rowWords keeps the index inside the row even for degenerate
    // one-column geometries (no size_t underflow).
    const uint32_t oeBit = (1u << kWideBitOe);
    for (size_t rowStart = 0; rowStart < slots; rowStart += rowWords) {
        const size_t rowEnd = rowStart + rowWords;  // one past the latch
        if (rowEnd > slots) break;
        for (int k = 1; k <= blankWords && static_cast<size_t>(k) < rowWords; k++)
            wide[rowEnd - 1 - static_cast<size_t>(k)] |= oeBit;
    }
}

uint32_t packWideWordReference(uint16_t slot0, uint16_t slot1, uint16_t slot2,
                               int chains) {
    const uint16_t slotByChain[3] = {slot0, slot1, slot2};
    uint32_t w = 0;
    for (int c = 0; c < chains && c < kMaxChains; c++) {
        const uint16_t s = slotByChain[c];
        const uint8_t gpios[6] = {kChainPins[c].r1, kChainPins[c].g1,
                                  kChainPins[c].b1, kChainPins[c].r2,
                                  kChainPins[c].g2, kChainPins[c].b2};
        for (int i = 0; i < 6; i++)
            if (s & (1u << kColourSlotBits[i]))
                w |= (1u << wideBitForGpio(gpios[i]));
    }
    // Control from chain 0.
    const int slotBits[7] = {kSlotA, kSlotB, kSlotC, kSlotD,
                             kSlotE, kSlotLat, kSlotOe};
    const int wideBits[7] = {kWideBitA, kWideBitB, kWideBitC, kWideBitD,
                             kWideBitE, kWideBitStb, kWideBitOe};
    for (int i = 0; i < 7; i++) {
        const bool slotSet = (slot0 & (1u << slotBits[i])) != 0;
        const bool pinSet = (i == 6) ? !slotSet : slotSet;  // OE inverted
        if (pinSet) w |= (1u << wideBits[i]);
    }
    return w;
}

}  // namespace mm::platform
