#pragma once

#include <cstddef>   // size_t
#include <cstdint>

namespace mm {

/// The HUB75 bus contract between the light driver and a port: which bit of the bus word carries each panel line, and the geometry that sizes one frame.
///
/// @technical
///
/// ## The bus word
///
/// Address, latch and OE share the color bits' word, since the panel wants them at once; `Hub75Layout` names each line's bit. OE is active low and a peripheral idles its lines low, so the OE bit means lit and the platform inverts the pin.

/// Which bus bit each HUB75 line occupies in @xref{the-bus-word|the 16-bit bus word} per slot; the platform layer maps bit to GPIO. Defaults are the conventional order, and a board that wires the panel differently overrides them.
struct Hub75Layout {
    /// Color bits for the upper half-panel.
    uint8_t r1 = 0, g1 = 1, b1 = 2;
    /// Color bits for the lower half-panel.
    uint8_t r2 = 3, g2 = 4, b2 = 5;
    /// Row address bits; a 1/8 panel uses only a, b and c.
    uint8_t a = 8, b = 9, c = 10, d = 11, e = 12;
    /// Latch: moves the shift register to the output drivers.
    uint8_t lat = 13;
    /// Output enable, SET where the panel is lit: the platform inverts the pin for the panel's active-low OE.
    uint8_t oe = 14;
};

/// The geometry one encode needs. `scanRate` is the panel's own and is NOT derivable from the height. Two panels of identical dimensions can scan differently, which is why it is a control rather than a calculation.
struct Hub75Geometry {
    /// Bytes on the wire per pixel clock: one 16-bit bus word, sixteen rather than eight since the control lines live at bits 8-14.
    static constexpr size_t kBytesPerSlot = 2;

    /// Panel width in pixels.
    uint16_t width = 64;
    /// Panel height in pixels.
    uint16_t height = 64;
    /// The panel's own scan rate: 8, 16 or 32, meaning 1/8, 1/16 or 1/32.
    uint8_t  scanRate = 16;
    /// Bit planes per frame, 2 to 8. Every plane costs a full scan pass.
    uint8_t  bitDepth = 4;
    /// How long each plane is lit, 0 to 255 of its full window: the global brightness, as time.
    uint8_t  brightness = 255;

    /// Address steps one plane walks: the panel's scanRate, so a 1/16 panel steps 16 addresses whatever its height.
    uint16_t scanRows() const { return scanRate; }

    /// Rows driven per address step: four on a 64-row 1/16 panel, since encoding only the first pair leaves three quarters dark.
    uint16_t rowsPerScan() const { return scanRate ? height / scanRate : 0; }

    /// Is this geometry encodable? Each step drives an upper and a lower row together, so an odd count leaves one unpaired.
    bool valid() const {
        if (width == 0 || height == 0) return false;
        // 8 is the encoder's hard ceiling: above it the plane shift in hub75Encode goes negative and the cast wraps. The old 6-bit cap guarded the narrow-row LSB, which the hold words in hub75Encode now guarantee instead, so the cap no longer needs to; which outputs may open above 6 is their own range's decision, not this gate's.
        if (bitDepth < 2 || bitDepth > 8) return false;
        if (scanRate == 0 || height % scanRate != 0) return false;
        return rowsPerScan() % 2 == 0;
    }

    /// Shift words one (plane, row) group clocks into the panel: the chain width times the colour passes per address step.
    size_t rowWords() const {
        return static_cast<size_t>(width) * (rowsPerScan() / 2);
    }

    /// Hold words the encoder leads each group with, so a narrow row's lowest plane still lights a whole word: 0 where the plane already lights one by rounding, else the stretch to a pass of `2^(bitDepth-1) + 1` words, where the lowest plane is exactly one word and every plane doubles exactly (Hub75Encode.h). A pure function of the row width and the depth — never of brightness, so the frame size does not move when the level changes.
    size_t holdWords() const {
        if (bitDepth < 2 || bitDepth > 8) return 0;
        const size_t row = rowWords();
        // The lowest plane's window rounds to nothing below 2^(bitDepth-2) + 1 row words; above that the rounding stands, as at 6 bits today.
        if (row > (static_cast<size_t>(1) << (bitDepth - 2))) return 0;
        const size_t pass = (static_cast<size_t>(1) << (bitDepth - 1)) + 1;
        return pass > row ? pass - row : 0;
    }

    /// Words one (plane, row) group holds on the wire: the shift words plus the hold words. Wide rows hold nothing, so the pass is the row.
    size_t passWords() const { return rowWords() + holdWords(); }

    /// Slots one encoded frame occupies, each plane stored once since storing plane p 2^p times is a megabyte for one 64x64 panel; each group is `passWords()` wide, hold words included.
    size_t frameSlots() const {
        return static_cast<size_t>(bitDepth) * scanRows() * passWords();
    }

    /// Bytes one encoded frame occupies: the one home for the size, which the platform asks for rather than recomputing.
    size_t frameBytes() const { return frameSlots() * kBytesPerSlot; }
};

}  // namespace mm
