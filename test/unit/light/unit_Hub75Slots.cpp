/// @module Hub75Driver
/// @also Hub75Slots

#include "doctest.h"
#include "light/drivers/Correction.h"
#include "light/drivers/Hub75Slots.h"

#include <cstring>
#include <vector>

// The encoder's contract on plain buffers, with no ESP32 and no panel: each case pins one thing a wall shows wrong without saying why.

namespace {

// A frame of `w * h` lights, every channel zero, so a test sets only what it means.
std::vector<uint8_t> blackFrame(uint16_t w, uint16_t h) {
    return std::vector<uint8_t>(static_cast<size_t>(w) * h * 3, 0);
}

void setPixel(std::vector<uint8_t>& f, uint16_t w, uint16_t x, uint16_t y,
              uint8_t r, uint8_t g, uint8_t b) {
    const size_t i = (static_cast<size_t>(y) * w + x) * 3;
    f[i + 0] = r; f[i + 1] = g; f[i + 2] = b;
}

/// Slot `n` as the 16-bit little-endian bus word, so no check tests one byte and misses the lines above bit 7.
uint16_t slot(const std::vector<uint8_t>& out, size_t n) {
    return static_cast<uint16_t>(out[n * 2] | (out[n * 2 + 1] << 8));
}

/// The first slot of the row that lights plane p's first row: the one after it latches, since a row is lit while the next one clocks in, and the buffer wraps. The stride is the pass, hold words included.
size_t litRowBase(const mm::Hub75Geometry& geo, uint8_t p) {
    const size_t rowsInFrame = static_cast<size_t>(geo.bitDepth) * geo.scanRows();
    return (static_cast<size_t>(p) * geo.scanRows() + 1) % rowsInFrame * geo.passWords();
}

}  // namespace

TEST_CASE("HUB75 encode: the frame is bit-plane major, one word per column") {
    mm::Hub75Geometry geo;
    geo.width = 8; geo.height = 4; geo.scanRate = 2; geo.bitDepth = 2;

    // 2 planes x 2 scan rows x 8 columns x 1 pair = 32 slots, and 2 bytes each because the bus is 16 bits wide.
    CHECK(geo.frameSlots() == 32);
    CHECK(geo.frameBytes() == 32 * 2);

    auto rgb = blackFrame(geo.width, geo.height);
    std::vector<uint8_t> out(geo.frameBytes(), 0xAA);
    CHECK(mm::hub75Encode(rgb.data(), out.data(), geo) == geo.frameBytes());
}

TEST_CASE("HUB75 encode: the address on a data word is the row in the LATCH, one behind") {
    // Seen on a wall: the picture displaced one scan row, the last row drawn over the first. The latch holds the row last strobed, so a data word addressed to its own row lights the previous row's data on it.
    mm::Hub75Geometry geo;
    geo.width = 4; geo.height = 4; geo.scanRate = 2; geo.bitDepth = 2;
    mm::Hub75Layout lay;   // the DEFAULT layout: a=8, b=9, above the low byte

    auto rgb = blackFrame(geo.width, geo.height);
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());

    // Row 0's lit words name the LAST row and its latch word names row 0; row 1's name row 0, then row 1.
    const size_t row1 = geo.passWords();           // row 1 starts right after row 0's pass
    const uint16_t latch = geo.passWords() - 1;    // the last word of the pass is the latch
    for (uint16_t x = 0; x < latch; x++) CHECK((slot(out, x) & (1u << lay.a)) != 0);
    CHECK((slot(out, latch) & (1u << lay.a)) == 0);
    for (uint16_t x = 0; x < latch; x++) CHECK((slot(out, row1 + x) & (1u << lay.a)) == 0);
    CHECK((slot(out, row1 + latch) & (1u << lay.a)) != 0);

    // And on EVERY lit word, not only the first: an address changing mid-row ghosts the row.
    for (uint16_t x = 1; x < latch; x++) {
        CHECK((slot(out, x) & (1u << lay.a)) == (slot(out, 0) & (1u << lay.a)));
    }

    // A board is free to re-map the lines into the low byte, and that must keep working: Hub75Layout exists so a different wiring costs a struct rather than an encoder.
    mm::Hub75Layout low;
    low.r1 = 0; low.g1 = 1; low.b1 = 2; low.r2 = 3; low.g2 = 4; low.b2 = 5;
    low.a = 6; low.b = 7; low.c = 7; low.d = 7; low.e = 7;
    low.lat = 6; low.oe = 7;
    std::vector<uint8_t> out2(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out2.data(), geo, low) == geo.frameBytes());
    for (uint16_t x = 0; x < latch; x++) CHECK((slot(out2, x) & (1u << low.a)) != 0);
    for (uint16_t x = 0; x < latch; x++) CHECK((slot(out2, row1 + x) & (1u << low.a)) == 0);
}

// @step driver-setup/add-hub75
TEST_CASE("HUB75 encode: a color bit lands on its own line, for both half-panels") {
    // A walking-one per color line: the test that catches a swapped pair (red where green should be) and an upper/lower mix-up. A panel shows both as "wrong colors", which is the least diagnostic symptom there is.
    mm::Hub75Geometry geo;
    geo.width = 2; geo.height = 4; geo.scanRate = 2; geo.bitDepth = 4;
    mm::Hub75Layout lay;

    struct Case { const char* name; uint8_t r, g, b; uint8_t bit; bool lower; };
    const Case cases[] = {
        {"upper red",   0xFF, 0, 0,    lay.r1, false},
        {"upper green", 0, 0xFF, 0,    lay.g1, false},
        {"upper blue",  0, 0, 0xFF,    lay.b1, false},
        {"lower red",   0xFF, 0, 0,    lay.r2, true},
        {"lower green", 0, 0xFF, 0,    lay.g2, true},
        {"lower blue",  0, 0, 0xFF,    lay.b2, true},
    };

    for (const auto& c : cases) {
        CAPTURE(c.name);
        auto rgb = blackFrame(geo.width, geo.height);
        // Scan row 0 drives panel row 0 (upper) and row 0 + height/2 = 2 (lower).
        setPixel(rgb, geo.width, 0, c.lower ? 2 : 0, c.r, c.g, c.b);

        std::vector<uint8_t> out(geo.frameBytes(), 0);
        REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());

        // At full value every plane carries the bit; column 0 of scan row 0 of plane 0. A 2-column row at 4 bits holds 7 words before its shift words, so the colour sits at the hold, not at slot 0.
        const size_t col0 = geo.holdWords();
        CHECK((slot(out, col0) & (1u << c.bit)) != 0);
        // and no OTHER color line is set by it.
        const uint8_t colorBits = static_cast<uint8_t>(
            (1u << lay.r1) | (1u << lay.g1) | (1u << lay.b1) |
            (1u << lay.r2) | (1u << lay.g2) | (1u << lay.b2));
        CHECK((slot(out, col0) & colorBits) == (1u << c.bit));
    }
}

TEST_CASE("HUB75 encode: depth below 8 keeps the HIGH bits") {
    // Dropping the low bits costs precision; dropping the high ones would cost RANGE, so a bright pixel would come out dim. The difference is invisible in a buffer dump and obvious on a wall, which is the wrong way round.
    mm::Hub75Geometry geo;
    geo.width = 1; geo.height = 2; geo.scanRate = 1; geo.bitDepth = 2;
    mm::Hub75Layout lay;

    auto rgb = blackFrame(geo.width, geo.height);
    setPixel(rgb, geo.width, 0, 0, 0xC0, 0, 0);   // 1100 0000: both top bits set

    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());

    // Both planes of a 2-bit depth read bits 6 and 7, so both carry red. The 1-column row holds 2 words, so the colour sits at the hold.
    const size_t plane0 = geo.holdWords();
    // Plane 1 starts after plane 0's single pass (2^0 = 1), in SLOTS: a 3-word pass, and the colour at its hold.
    const size_t plane1 = static_cast<size_t>(geo.scanRows()) * geo.passWords() + geo.holdWords();
    CHECK((slot(out, plane0) & (1u << lay.r1)) != 0);
    CHECK((slot(out, plane1) & (1u << lay.r1)) != 0);

    // 0x30 (0011 0000) is in the LOW half: at 2-bit depth it reads as black, which is the precision loss the depth control trades for refresh.
    auto dim = blackFrame(geo.width, geo.height);
    setPixel(dim, geo.width, 0, 0, 0x30, 0, 0);
    std::vector<uint8_t> out2(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(dim.data(), out2.data(), geo, lay) == geo.frameBytes());
    CHECK((slot(out2, plane0) & (1u << lay.r1)) == 0);
    CHECK((slot(out2, plane1) & (1u << lay.r1)) == 0);
}

TEST_CASE("HUB75 encode: a panel driving four rows per address step encodes every pair") {
    // A 64-row 1/16 panel steps 16 addresses and drives FOUR rows each: two pairs, the second offset by 2 x scanRate. An encoder assuming two rows would leave three quarters of the panel dark, and the geometry is common enough to matter.
    mm::Hub75Geometry geo;
    geo.width = 2; geo.height = 8; geo.scanRate = 2; geo.bitDepth = 4;
    mm::Hub75Layout lay;

    CHECK(geo.rowsPerScan() == 4);
    // 4 planes x 2 scan rows x a 9-word pass (4 shift words + 5 hold words) = 72 slots, 144 bytes.
    CHECK(geo.holdWords() == 5);   // 2^(4-1)+1 - 4: the 4-word row stretches to 9
    CHECK(geo.frameSlots() == 72);
    CHECK(geo.frameBytes() == 72 * 2);

    auto rgb = blackFrame(geo.width, geo.height);
    setPixel(rgb, geo.width, 0, 2, 0xFF, 0, 0);   // row 2 = address step 0, pair 1, upper

    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());

    // Pair 0 (rows 0 and 4) is black; pair 1 (rows 2 and 6) carries the red. Both sit past the hold words.
    CHECK((slot(out, geo.holdWords()) & (1u << lay.r1)) == 0);              // pair 0, column 0
    CHECK((slot(out, geo.holdWords() + geo.width) & (1u << lay.r1)) != 0);  // pair 1, column 0
    // One latch per address step, on the last pair's last word, not at the end of each pair.
    CHECK((slot(out, geo.holdWords() + geo.width - 1) & (1u << lay.lat)) == 0);
    CHECK((slot(out, geo.passWords() - 1) & (1u << lay.lat)) != 0);
}

TEST_CASE("HUB75 encode: the last column's word latches, dark") {
    // Dark while the shift register hands its row to the output drivers, or the addressed row briefly shows the previous row's data.
    mm::Hub75Geometry geo;
    geo.width = 2; geo.height = 2; geo.scanRate = 1; geo.bitDepth = 2;
    mm::Hub75Layout lay;
    lay.lat = 6; lay.oe = 7;   // both in the low byte, so the encoded byte shows them

    auto rgb = blackFrame(geo.width, geo.height);
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());

    const size_t latch = geo.width - 1;   // the last column's word
    CHECK((slot(out, latch) & (1u << lay.oe)) == 0);    // dark
    CHECK((slot(out, latch) & (1u << lay.lat)) != 0);   // and latching
    CHECK((slot(out, 0) & (1u << lay.lat)) == 0);       // and only there
}

TEST_CASE("HUB75 encode: the DMA wrap lights the last row once, from row 0's data words") {
    // The peripheral LOOPS this buffer, so the last row is strobed dark by its blank and what lights it is row 0's data words at the top, addressed to it. A dark tail word here fixed a window that never existed.
    mm::Hub75Geometry geo;
    geo.width = 2; geo.height = 4; geo.scanRate = 2; geo.bitDepth = 2;
    mm::Hub75Layout lay;
    lay.lat = 6; lay.oe = 7;
    lay.a = 8; lay.b = 9; lay.c = 10; lay.d = 11; lay.e = 12;

    std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, 255);
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());

    // The frame's last word is the last row's latch word: dark, latching, addressed to that row.
    const size_t last = geo.frameSlots() - 1;
    const uint16_t lastRow = geo.scanRows() - 1;
    CHECK((slot(out, last) & (1u << lay.oe)) == 0);
    CHECK((slot(out, last) & (1u << lay.lat)) != 0);
    CHECK((slot(out, last) & (1u << lay.a)) == (lastRow & 1 ? (1u << lay.a) : 0u));

    // And the first word lights, addressed to that same last row, whose data the latch now holds.
    CHECK((slot(out, 0) & (1u << lay.oe)) != 0);
    CHECK((slot(out, 0) & (1u << lay.a)) == (lastRow & 1 ? (1u << lay.a) : 0u));
}

// Seen on two chained 64x64 panels: a line at rows 31 and 63, the last row lit with the next plane's window while the latch held the plane before. White sums the same, so the wrap test above passes either way.
TEST_CASE("HUB75 encode: every row of a value is lit as long as every other, the last row of each half included") {
    mm::Hub75Geometry geo;
    geo.width = 64; geo.height = 64; geo.scanRate = 32; geo.bitDepth = 6; geo.brightness = 255;
    mm::Hub75Layout lay;
    std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, 0x7F);   // below half: every plane but the top one
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());

    // As the panel sees it: a row lights what the row before it latched, so a row's brightness sums the next row's lit words over the planes where its bit is set.
    const uint16_t rows = geo.scanRows();
    const size_t rowsInFrame = static_cast<size_t>(geo.bitDepth) * rows;
    std::vector<uint32_t> upper(rows, 0), lower(rows, 0);
    for (size_t b = 0; b < rowsInFrame; b++) {
        const size_t latchedFrom = (b + rowsInFrame - 1) % rowsInFrame;
        const uint16_t data = slot(out, latchedFrom * geo.passWords() + geo.holdWords());
        uint32_t lit = 0;
        for (size_t x = 0; x < geo.passWords(); x++) lit += (slot(out, b * geo.passWords() + x) & (1u << lay.oe)) != 0;
        const uint16_t row = static_cast<uint16_t>(latchedFrom % rows);
        if (data & (1u << lay.r1)) upper[row] += lit;
        if (data & (1u << lay.r2)) lower[row] += lit;
    }
    REQUIRE(upper[0] > 0);
    for (uint16_t r = 0; r < rows; r++) {
        CAPTURE(r);
        CHECK(upper[r] == upper[0]);   // row r of the upper half
        CHECK(lower[r] == upper[0]);   // row r + 32: the lower half, row 63 the bottom edge
    }
}

// Seen on a 64x64 panel: row 63 ghosting onto row 32, rows 0 and 32 brighter, flashing. The peripheral idles its lines LOW between scans, and with OE encoded as the panel reads it (active low) that idle bus lit address 0 with the last latched row.
TEST_CASE("HUB75 encode: the OE bit is set where the panel is lit, so an all-zero idle bus is dark") {
    mm::Hub75Geometry geo;
    geo.width = 4; geo.height = 2; geo.scanRate = 1; geo.bitDepth = 2;
    mm::Hub75Layout lay;
    std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, 0);   // black: color says nothing about OE
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
    // The top plane fills its row, so every word of the pass lights but the latch word.
    const size_t top = litRowBase(geo, geo.bitDepth - 1);
    for (size_t x = 0; x + 1 < geo.passWords(); x++) CHECK((slot(out, top + x) & (1u << lay.oe)) != 0);
    CHECK((slot(out, top + geo.passWords() - 1) & (1u << lay.oe)) == 0);
}

// Seen on the 64x64 panel: the last column dark whatever the effect, and the picture one column left. Every word is a clock, so a separate latch word shifted a 65th value into a 64-pixel row.
TEST_CASE("HUB75 encode: a row clocks exactly its width, so the last column reaches the panel's last pixel") {
    mm::Hub75Geometry geo;
    geo.width = 4; geo.height = 2; geo.scanRate = 1; geo.bitDepth = 2;
    mm::Hub75Layout lay;
    auto rgb = blackFrame(geo.width, geo.height);
    setPixel(rgb, geo.width, 3, 0, 0xFF, 0, 0);   // the LAST column, red
    setPixel(rgb, geo.width, 0, 0, 0, 0xFF, 0);   // the FIRST column, green
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
    CHECK(geo.frameSlots() == static_cast<size_t>(geo.bitDepth) * geo.width);   // one scan row: width words a plane, nothing more
    CHECK((slot(out, 0) & (1u << lay.g1)) != 0);   // first word: column 0, shifted in first, lands on the far pixel
    CHECK((slot(out, 3) & (1u << lay.r1)) != 0);   // the latch word still carries column 3's color
    CHECK((slot(out, 3) & (1u << lay.lat)) != 0);
}

namespace {
// How many words light plane p's first row: the words of the row after it that carry the OE (lit) bit. The count runs over the whole pass, hold words included: a hold word inside the window lights the panel.
size_t litWords(const std::vector<uint8_t>& out, const mm::Hub75Geometry& geo, const mm::Hub75Layout& lay, uint8_t p) {
    const size_t base = litRowBase(geo, p);
    size_t n = 0;
    for (size_t x = 0; x < geo.passWords(); x++) n += (slot(out, base + x) & (1u << lay.oe)) != 0;
    return n;
}
}  // namespace

// Seen on the 64x64 panel: brightness coarse and out of order. Equal-time planes made a pixel's brightness count its set bits, so 7 (0111) outshone 8 (1000); binary time makes it follow the value.
TEST_CASE("HUB75 encode: each plane is lit twice as long as the one below, the top plane for the whole row") {
    mm::Hub75Geometry geo;
    geo.width = 9; geo.height = 2; geo.scanRate = 1; geo.bitDepth = 4;   // 8 words that can light, plus the latch
    mm::Hub75Layout lay;
    auto rgb = blackFrame(geo.width, geo.height);
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
    CHECK(litWords(out, geo, lay, 0) == 1);
    CHECK(litWords(out, geo, lay, 1) == 2);
    CHECK(litWords(out, geo, lay, 2) == 4);
    CHECK(litWords(out, geo, lay, 3) == 8);
}

// The slider used to scale values, so at about 75 every channel fell below 4 bits and the panel went black. As time, every plane keeps its share and only the shortest windows round away.
TEST_CASE("HUB75 encode: brightness shortens every plane's window, keeping the values") {
    mm::Hub75Geometry geo;
    geo.width = 9; geo.height = 2; geo.scanRate = 1; geo.bitDepth = 4;
    mm::Hub75Layout lay;
    auto rgb = blackFrame(geo.width, geo.height);
    setPixel(rgb, geo.width, 0, 0, 0xFF, 0, 0);
    geo.brightness = 128;
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
    CHECK(litWords(out, geo, lay, 3) == 4);   // half of 8
    CHECK(litWords(out, geo, lay, 2) == 2);
    CHECK(litWords(out, geo, lay, 1) == 1);
    CHECK((slot(out, 0) & (1u << lay.r1)) != 0);   // the pixel's value is untouched: full red still shifts in
    geo.brightness = 0;
    std::vector<uint8_t> dark(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), dark.data(), geo, lay) == geo.frameBytes());
    for (uint8_t p = 0; p < geo.bitDepth; p++) CHECK(litWords(dark, geo, lay, p) == 0);
}

// Plane 0's window is 1/32 of the top plane's at 6-bit, so near a quarter brightness it rounds to nothing: the limit the encoder's docs state.
TEST_CASE("HUB75 encode: below a quarter brightness the lowest 6-bit plane of a 64-column row is dark") {
    mm::Hub75Geometry geo;
    geo.width = 64; geo.height = 2; geo.scanRate = 1; geo.bitDepth = 6; geo.brightness = 64;
    mm::Hub75Layout lay;
    auto rgb = blackFrame(geo.width, geo.height);
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
    CHECK(litWords(out, geo, lay, 0) == 0);
    CHECK(litWords(out, geo, lay, 5) == 16);   // the top plane keeps its quarter of 63
}

// A one-column panel: its only shift word is the latch, so the pass holds two words before it and the row encodes lit, not dark. The hold words carry no colour and no latch; the latch word still carries the column's colour, dark.
TEST_CASE("HUB75 encode: a one-column row holds two words, then latches") {
    mm::Hub75Geometry geo;
    geo.width = 1; geo.height = 2; geo.scanRate = 1; geo.bitDepth = 2;
    mm::Hub75Layout lay;
    auto rgb = blackFrame(geo.width, geo.height);
    setPixel(rgb, geo.width, 0, 0, 0xFF, 0, 0);
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
    CHECK(geo.holdWords() == 2);
    CHECK(geo.passWords() == 3);
    CHECK(geo.frameSlots() == 6);   // 2 planes x 1 scan row x a 3-word pass
    // Plane 0's group: the top plane's window is 2 words, so both hold words light. Plane 1's: the lowest plane's window is 1 word, so only the first hold word lights.
    CHECK((slot(out, 0) & (1u << lay.oe)) != 0);
    CHECK((slot(out, 1) & (1u << lay.oe)) != 0);
    CHECK((slot(out, 3) & (1u << lay.oe)) != 0);
    CHECK((slot(out, 4) & (1u << lay.oe)) == 0);
    for (size_t s : {0u, 1u, 3u, 4u}) {
        CHECK((slot(out, s) & (1u << lay.lat)) == 0);   // no latch on a hold word
        CHECK((slot(out, s) & (1u << lay.r1)) == 0);    // and no colour
    }
    for (size_t s : {2u, 5u}) {
        CHECK((slot(out, s) & (1u << lay.lat)) != 0);   // the latch word latches
        CHECK((slot(out, s) & (1u << lay.oe)) == 0);    // dark
        CHECK((slot(out, s) & (1u << lay.r1)) != 0);    // and the colour still shifts in
    }
}

TEST_CASE("HUB75 encode: an unusable geometry writes nothing") {
    // Zero rather than a partial write. A half-encoded frame on a panel is a worse failure than a dark one, and the driver turns this into a status the user reads.
    auto rgb = blackFrame(8, 8);
    std::vector<uint8_t> out(4096, 0xAA);

    mm::Hub75Geometry odd;      // 3 rows per address step: no pair for the third
    odd.width = 8; odd.height = 6; odd.scanRate = 2; odd.bitDepth = 4;
    CHECK(odd.valid() == false);
    CHECK(mm::hub75Encode(rgb.data(), out.data(), odd) == 0);

    mm::Hub75Geometry indivisible;
    indivisible.width = 8; indivisible.height = 7; indivisible.scanRate = 2;
    CHECK(mm::hub75Encode(rgb.data(), out.data(), indivisible) == 0);

    mm::Hub75Geometry deep;   // above the 8-bit ceiling, which is what valid() enforces; 7 and 8 encode, the hold words stretching their narrow rows
    deep.width = 8; deep.height = 8; deep.scanRate = 4; deep.bitDepth = 9;
    CHECK(mm::hub75Encode(rgb.data(), out.data(), deep) == 0);

    mm::Hub75Geometry ok;
    ok.width = 8; ok.height = 8; ok.scanRate = 4; ok.bitDepth = 4;
    CHECK(mm::hub75Encode(nullptr, out.data(), ok) == 0);
    CHECK(mm::hub75Encode(rgb.data(), nullptr, ok) == 0);

    // Nothing was written by any of the refusals.
    CHECK(out[0] == 0xAA);
}

TEST_CASE("HUB75 frame size is what decides the peripheral") {
    // The sizes that decide Parlio (65,535 bytes a transfer) against LCD_CAM, two bytes a slot, pinned at 4-bit so a wire-format change cannot move the cliff unseen.
    mm::Hub75Geometry one;    // one 64x64 panel, 1/32 scan
    one.width = 64; one.height = 64; one.scanRate = 32; one.bitDepth = 4;
    CHECK(one.frameBytes() == (4 * 32 * 64 * 1) * 2);            // 16,384
    CHECK(one.frameBytes() < 65535u);                          // Parlio carries one panel

    mm::Hub75Geometry four;   // 128x128, 1/32 scan
    four.width = 128; four.height = 128; four.scanRate = 32; four.bitDepth = 4;
    CHECK(four.frameBytes() == (4 * 32 * 128 * 2) * 2);          // 65,536
    CHECK(four.frameBytes() > 65535u);                         // four panels miss the cap by one byte

    mm::Hub75Geometry sixteen;   // 256x256, 1/32 scan
    sixteen.width = 256; sixteen.height = 256; sixteen.scanRate = 32; sixteen.bitDepth = 4;
    CHECK(sixteen.frameBytes() == (4 * 32 * 256 * 4) * 2);        // 262,144
    CHECK(sixteen.frameBytes() > 65535u);                       // Parlio cannot carry it
}

namespace {
// The lit-word vector of a geometry, plane by plane, so a test pins the whole vector in one loop.
void checkVector(const std::vector<uint8_t>& out, const mm::Hub75Geometry& geo, const mm::Hub75Layout& lay, const size_t* want, uint8_t bits) {
    for (uint8_t p = 0; p < bits; p++) CHECK(litWords(out, geo, lay, p) == want[p]);
}
}  // namespace

TEST_CASE("HUB75 encode: at 8 bits every input bit has its own plane, LSB first") {
    // The golden vector: three red pixels whose values pick out the bottom bit, the top bit and every bit, so a plane that read the wrong bit shows at once. An 8-column row at 8 bits holds 121 words, so the colour sits past the hold.
    mm::Hub75Geometry geo;
    geo.width = 8; geo.height = 4; geo.scanRate = 2; geo.bitDepth = 8;
    CHECK(geo.valid());
    CHECK(geo.holdWords() == 121);   // 2^(8-1)+1 - 8: the 8-word row stretches to 129
    CHECK(geo.frameSlots() == 8 * 2 * 129);   // 8 planes x 2 scan rows x a 129-word pass

    auto rgb = blackFrame(geo.width, geo.height);
    setPixel(rgb, geo.width, 0, 0, 0x01, 0, 0);   // bit 0 only
    setPixel(rgb, geo.width, 1, 0, 0x80, 0, 0);   // bit 7 only
    setPixel(rgb, geo.width, 2, 0, 0xFF, 0, 0);   // every bit
    mm::Hub75Layout lay;
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());

    // Plane p's row 0 starts at slot p * scanRows * passWords, past the hold words; the pixel's red bit rides its own plane's word.
    for (uint8_t p = 0; p < 8; p++) {
        const size_t base = static_cast<size_t>(p) * geo.scanRows() * geo.passWords() + geo.holdWords();
        CHECK(((slot(out, base + 0) >> lay.r1) & 1) == (p == 0 ? 1 : 0));   // 0x01: plane 0 alone
        CHECK(((slot(out, base + 1) >> lay.r1) & 1) == (p == 7 ? 1 : 0));   // 0x80: plane 7 alone
        CHECK(((slot(out, base + 2) >> lay.r1) & 1) == 1);                  // 0xFF: every plane
    }
    // The frame ends on the last row's latch word, dark, as at every depth.
    CHECK((slot(out, geo.frameSlots() - 1) & (1u << lay.lat)) != 0);
    CHECK((slot(out, geo.frameSlots() - 1) & (1u << lay.oe)) == 0);
}

TEST_CASE("HUB75 encode: at 8 bits the lit windows halve plane by plane on a wide row") {
    // The wall's row width, full brightness: the LSB lights 2 words, the top plane fills the row less its latch. No hold: 256 words is wide enough.
    mm::Hub75Geometry geo;
    geo.width = 256; geo.height = 32; geo.scanRate = 16; geo.bitDepth = 8;
    CHECK(geo.holdWords() == 0);
    mm::Hub75Layout lay;
    std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, 0xFF);
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
    const size_t want[8] = {2, 4, 8, 16, 32, 64, 128, 255};
    checkVector(out, geo, lay, want, 8);
}

TEST_CASE("HUB75 encode: at 8 bits every plane lights on every row width, narrowest included") {
    // The hold words: a narrow row stretches until its lowest plane is exactly one word, so the eighth bit shows on a 16-column panel too, at exact powers-of-two ratios. Row widths run from the narrowest the UI allows (16) past the wall (256) to 1024-wide chains, at 7 and 8 bits, full brightness: every plane lights at least one word.
    mm::Hub75Layout lay;
    for (uint16_t width : {16, 17, 32, 64, 65, 128, 192, 193, 256, 512, 1024}) {
        for (uint8_t bits : {7, 8}) {
            CAPTURE(width);
            CAPTURE(bits);
            mm::Hub75Geometry geo;
            geo.width = width; geo.height = 32; geo.scanRate = 16; geo.bitDepth = bits;
            std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, 0xFF);
            std::vector<uint8_t> out(geo.frameBytes(), 0);
            REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
            for (uint8_t p = 0; p < bits; p++) CHECK(litWords(out, geo, lay, p) >= 1);
        }
    }
    // The exact vectors the hold produces at full brightness: a stretched row doubles plane by plane, the top plane filling the pass less its latch.
    {
        mm::Hub75Geometry geo;
        geo.width = 64; geo.height = 32; geo.scanRate = 16; geo.bitDepth = 8;
        CHECK(geo.holdWords() == 65);   // 64 to 129 words a pass: the frame doubles
        std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, 0xFF);
        std::vector<uint8_t> out(geo.frameBytes(), 0);
        REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
        const size_t want[8] = {1, 2, 4, 8, 16, 32, 64, 128};
        checkVector(out, geo, lay, want, 8);
    }
    {
        mm::Hub75Geometry geo;
        geo.width = 16; geo.height = 32; geo.scanRate = 16; geo.bitDepth = 8;
        CHECK(geo.holdWords() == 113);   // 16 to 129 words a pass: the frame grows eightfold
        std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, 0xFF);
        std::vector<uint8_t> out(geo.frameBytes(), 0);
        REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
        const size_t want[8] = {1, 2, 4, 8, 16, 32, 64, 128};
        checkVector(out, geo, lay, want, 8);
    }
    {
        mm::Hub75Geometry geo;
        geo.width = 16; geo.height = 32; geo.scanRate = 16; geo.bitDepth = 7;
        CHECK(geo.holdWords() == 49);   // 16 to 65 words a pass
        std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, 0xFF);
        std::vector<uint8_t> out(geo.frameBytes(), 0);
        REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
        const size_t want[7] = {1, 2, 4, 8, 16, 32, 64};
        checkVector(out, geo, lay, want, 7);
    }
    // A 64-column row at 7 bits holds nothing -- its lowest plane is already a word -- and the vector is the formula's, top plane one short of the row.
    {
        mm::Hub75Geometry geo;
        geo.width = 64; geo.height = 32; geo.scanRate = 16; geo.bitDepth = 7;
        CHECK(geo.holdWords() == 0);
        std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, 0xFF);
        std::vector<uint8_t> out(geo.frameBytes(), 0);
        REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
        const size_t want[7] = {1, 2, 4, 8, 16, 32, 63};
        checkVector(out, geo, lay, want, 7);
    }
    // Where the lowest plane already lights by rounding, the hold does not engage and the rounding stands: 65 words at 8 bits, 33 at 7, each one word past the stretch. The same shape as 6-bit 17-wide, which the encoder has always produced.
    {
        mm::Hub75Geometry geo;
        geo.width = 65; geo.height = 32; geo.scanRate = 16; geo.bitDepth = 8;
        CHECK(geo.holdWords() == 0);
        std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, 0xFF);
        std::vector<uint8_t> out(geo.frameBytes(), 0);
        REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
        const size_t want[8] = {1, 1, 2, 4, 8, 16, 32, 64};
        checkVector(out, geo, lay, want, 8);
    }
    {
        mm::Hub75Geometry geo;
        geo.width = 33; geo.height = 32; geo.scanRate = 16; geo.bitDepth = 7;
        CHECK(geo.holdWords() == 0);
        std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, 0xFF);
        std::vector<uint8_t> out(geo.frameBytes(), 0);
        REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
        const size_t want[7] = {1, 1, 2, 4, 8, 16, 32};
        checkVector(out, geo, lay, want, 7);
    }
    // A 1024-wide chain at 8 bits: the vector doubles exactly, the top plane one short of the row.
    {
        mm::Hub75Geometry geo;
        geo.width = 1024; geo.height = 32; geo.scanRate = 16; geo.bitDepth = 8;
        CHECK(geo.holdWords() == 0);
        std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, 0xFF);
        std::vector<uint8_t> out(geo.frameBytes(), 0);
        REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
        const size_t want[8] = {8, 16, 32, 64, 128, 256, 512, 1023};
        checkVector(out, geo, lay, want, 8);
    }
}

// Without a floor the low-brightness behaviour is the analysis's original prediction, not a dark lift: the LSB drops out below brightness 64 on the wall (8-bit behaving as 7-bit there, smooth, not an artefact), and below half brightness on a stretched row, where it is exactly one word at full brightness.
TEST_CASE("HUB75 encode: at low brightness the lowest 8-bit plane drops out, on the wall and on a stretched row") {
    mm::Hub75Layout lay;
    {
        mm::Hub75Geometry geo;
        geo.width = 256; geo.height = 32; geo.scanRate = 16; geo.bitDepth = 8; geo.brightness = 64;
        std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, 0xFF);
        std::vector<uint8_t> out(geo.frameBytes(), 0);
        REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
        const size_t want[8] = {1, 1, 2, 4, 8, 16, 32, 64};
        checkVector(out, geo, lay, want, 8);
    }
    {
        mm::Hub75Geometry geo;
        geo.width = 256; geo.height = 32; geo.scanRate = 16; geo.bitDepth = 8; geo.brightness = 63;
        std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, 0xFF);
        std::vector<uint8_t> out(geo.frameBytes(), 0);
        REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
        const size_t want[8] = {0, 1, 2, 4, 8, 16, 32, 63};
        checkVector(out, geo, lay, want, 8);
    }
    {
        mm::Hub75Geometry geo;
        geo.width = 64; geo.height = 32; geo.scanRate = 16; geo.bitDepth = 8; geo.brightness = 128;
        std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, 0xFF);
        std::vector<uint8_t> out(geo.frameBytes(), 0);
        REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
        const size_t want[8] = {1, 1, 2, 4, 8, 16, 32, 64};
        checkVector(out, geo, lay, want, 8);
    }
    {
        mm::Hub75Geometry geo;
        geo.width = 64; geo.height = 32; geo.scanRate = 16; geo.bitDepth = 8; geo.brightness = 127;
        std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, 0xFF);
        std::vector<uint8_t> out(geo.frameBytes(), 0);
        REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
        const size_t want[8] = {0, 1, 2, 4, 8, 16, 32, 64};
        checkVector(out, geo, lay, want, 8);
    }
}

TEST_CASE("HUB75 encode: at 6 bits the hold is a no-op for rows of 17 words and up, and the 16-word row is the one exception") {
    // The hold is uniform across depths, so it cannot fix the 8-bit 64-word row without touching the 6-bit 16-word row: the 8-bit LSB's window (63/128 of a word) EXCEEDS the 6-bit one's (15/32), so any rule separating them would be a depth gate in disguise. The 16-word row at 6 bits is the ONLY in-UI geometry whose 6-bit frame changes: its lowest plane, which used to round to nothing, now lights exactly one word, and the frame stretches 16 to 33 words a pass.
    mm::Hub75Layout lay;
    {
        mm::Hub75Geometry geo;
        geo.width = 16; geo.height = 32; geo.scanRate = 16; geo.bitDepth = 6;
        CHECK(geo.holdWords() == 17);
        CHECK(geo.passWords() == 33);
        std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, 0xFF);
        std::vector<uint8_t> out(geo.frameBytes(), 0);
        REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
        const size_t want[6] = {1, 2, 4, 8, 16, 32};
        checkVector(out, geo, lay, want, 6);
    }
    // Every wider 6-bit row is byte-identical to the old encoder: the vectors below are the old rounding, unchanged.
    {
        mm::Hub75Geometry geo;
        geo.width = 17; geo.height = 32; geo.scanRate = 16; geo.bitDepth = 6;
        CHECK(geo.holdWords() == 0);
        std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, 0xFF);
        std::vector<uint8_t> out(geo.frameBytes(), 0);
        REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
        const size_t want[6] = {1, 1, 2, 4, 8, 16};
        checkVector(out, geo, lay, want, 6);
    }
    {
        mm::Hub75Geometry geo;
        geo.width = 64; geo.height = 32; geo.scanRate = 16; geo.bitDepth = 6;
        CHECK(geo.holdWords() == 0);
        std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, 0xFF);
        std::vector<uint8_t> out(geo.frameBytes(), 0);
        REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
        const size_t want[6] = {2, 4, 8, 16, 32, 63};
        checkVector(out, geo, lay, want, 6);
    }
    {
        mm::Hub75Geometry geo;
        geo.width = 256; geo.height = 32; geo.scanRate = 16; geo.bitDepth = 6;
        CHECK(geo.holdWords() == 0);
        std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, 0xFF);
        std::vector<uint8_t> out(geo.frameBytes(), 0);
        REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
        const size_t want[6] = {8, 16, 32, 64, 128, 255};
        checkVector(out, geo, lay, want, 6);
    }
}

TEST_CASE("HUB75 encode: brightness 0 stays dark at 8 bits") {
    mm::Hub75Geometry geo;
    geo.width = 64; geo.height = 32; geo.scanRate = 16; geo.bitDepth = 8; geo.brightness = 0;
    mm::Hub75Layout lay;
    std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, 0xFF);
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
    for (uint8_t p = 0; p < 8; p++) CHECK(litWords(out, geo, lay, p) == 0);
}

TEST_CASE("HUB75 encode: at 8 bits the planes carry the CIE-corrected byte, LSB included") {
    // The 8-bit path reads the full byte the DriverBase correction baked through its CIE 1931 curve (Correction::cieLuminance, the inverse CIE 1976 L* function): the corrected value's lowest bit is plane 0's, so the curve's bottom step shows. Spot values of the table first, then the encode of a corrected byte.
    mm::Correction correction;
    correction.curve = mm::Correction::Curve::Cie;
    correction.rebuildBrightness(255);
    CHECK(correction.briLut[0] == 0);
    CHECK(correction.briLut[1] == 1);     // the quantize floor keeps a lit input above zero
    CHECK(correction.briLut[64] == 11);
    CHECK(correction.briLut[128] == 47);
    CHECK(correction.briLut[255] == 255);

    mm::Hub75Geometry geo;
    geo.width = 8; geo.height = 4; geo.scanRate = 2; geo.bitDepth = 8;
    mm::Hub75Layout lay;
    auto rgb = blackFrame(geo.width, geo.height);
    setPixel(rgb, geo.width, 0, 0, correction.briLut[128], 0, 0);   // 47 = 0b00101111: bits 0,1,2,3,5
    setPixel(rgb, geo.width, 1, 0, correction.briLut[1], 0, 0);     // 1: bit 0 alone, the curve's bottom step
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());
    for (uint8_t p = 0; p < 8; p++) {
        const size_t base = static_cast<size_t>(p) * geo.scanRows() * geo.passWords() + geo.holdWords();
        const bool bit47 = (47u >> p) & 1;
        CHECK(((slot(out, base + 0) >> lay.r1) & 1) == (bit47 ? 1 : 0));
        CHECK(((slot(out, base + 1) >> lay.r1) & 1) == (p == 0 ? 1 : 0));
    }
}

TEST_CASE("HUB75 encode: the hold words carry no colour and no latch, are addressed to the latched row, and light inside the window") {
    // A 16-column row at 8 bits: 113 hold words lead each group. They push out of the shift register before the latch, so they carry no colour; they name the latched row, so the light they give falls on the row the latch holds.
    mm::Hub75Geometry geo;
    geo.width = 16; geo.height = 32; geo.scanRate = 16; geo.bitDepth = 8;
    mm::Hub75Layout lay;
    CHECK(geo.holdWords() == 113);
    CHECK(geo.passWords() == 129);
    std::vector<uint8_t> rgb(static_cast<size_t>(geo.width) * geo.height * 3, 0xFF);   // white: every colour bit set where a word carries data
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());

    const uint8_t colorBits = static_cast<uint8_t>(
        (1u << lay.r1) | (1u << lay.g1) | (1u << lay.b1) |
        (1u << lay.r2) | (1u << lay.g2) | (1u << lay.b2));
    // Row 0's words name the last row, 15: A-D set, E clear.
    const uint16_t lastRowAddr = static_cast<uint16_t>((1u << lay.a) | (1u << lay.b) | (1u << lay.c) | (1u << lay.d));
    const uint16_t addrBits = static_cast<uint16_t>((1u << lay.a) | (1u << lay.b) | (1u << lay.c) | (1u << lay.d) | (1u << lay.e));
    for (size_t i = 0; i < geo.holdWords(); i++) {
        CHECK((slot(out, i) & colorBits) == 0);              // no colour
        CHECK((slot(out, i) & (1u << lay.lat)) == 0);        // no latch
        CHECK((slot(out, i) & addrBits) == lastRowAddr);     // addressed to the latched row
    }
    // Plane 0's row 0 shows the top plane's window (128 words), so the hold words light; plane 1's shows the lowest plane's (1 word), so only its first hold word lights.
    CHECK((slot(out, geo.holdWords() - 1) & (1u << lay.oe)) != 0);
    const size_t plane1 = static_cast<size_t>(geo.scanRows()) * geo.passWords();
    CHECK((slot(out, plane1) & (1u << lay.oe)) != 0);
    CHECK((slot(out, plane1 + 1) & (1u << lay.oe)) == 0);
    // The shift words follow the hold words: the first carries column 0's colour, the last latches, dark, carrying the last column's.
    CHECK((slot(out, geo.holdWords()) & colorBits) != 0);
    CHECK((slot(out, geo.passWords() - 1) & (1u << lay.lat)) != 0);
    CHECK((slot(out, geo.passWords() - 1) & (1u << lay.oe)) == 0);
    CHECK((slot(out, geo.passWords() - 1) & (1u << lay.r1)) != 0);
}
