/// @module Hub75Canvas

#include "doctest.h"
#include "light/drivers/Hub75Canvas.h"

#include <vector>

using namespace mm;

// The expected numbers come from docs/reference/hardware/hub75-refresh.md's formula and tables, worked by hand, and from the placement rule in Hub75Canvas.h; none is read back from the code under test.

namespace {

// A 128 by 64 panel's worth of settings: the rows, columns, row addresses, bit planes and, past the default, the LSB time.
PanelSpec panel(uint16_t rows, uint16_t cols, uint16_t scan, uint8_t bits, uint16_t lsb = 130, uint8_t first = 0, uint8_t dither = 0) {
    PanelSpec p;
    p.rows = rows; p.cols = cols; p.scanRows = scan; p.bits = bits; p.lsbNs = lsb; p.firstPlane = first; p.ditherBits = dither;
    return p;
}

void checkPos(uint32_t i, uint16_t rows, uint16_t cols, uint16_t chain, uint16_t x, uint16_t y) {
    const CanvasPos p = chainCanvasPos(i, rows, cols, chain);
    INFO("light " << i << " rows " << rows << " cols " << cols << " chain " << chain);
    CHECK(p.x == x);
    CHECK(p.y == y);
}

} // namespace

TEST_CASE("Hub75Canvas: one panel's lights run along its rows") {
    checkPos(0, 64, 128, 1, 0, 0);
    checkPos(1, 64, 128, 1, 1, 0);
    checkPos(127, 64, 128, 1, 127, 0);
    checkPos(128, 64, 128, 1, 0, 1);
    checkPos(8191, 64, 128, 1, 127, 63);
}

TEST_CASE("Hub75Canvas: the second panel of a chain starts one panel to the right") {
    checkPos(8191, 64, 128, 2, 127, 63);
    checkPos(8192, 64, 128, 2, 128, 0);
    checkPos(8321, 64, 128, 2, 129, 1);
    checkPos(16383, 64, 128, 2, 255, 63);
}

TEST_CASE("Hub75Canvas: six panels as two to a chain, three chains, fill a 256 by 192 canvas") {
    checkPos(0, 64, 128, 2, 0, 0);
    checkPos(8192, 64, 128, 2, 128, 0);
    checkPos(16384, 64, 128, 2, 0, 64);
    checkPos(40960, 64, 128, 2, 128, 128);
    checkPos(49151, 64, 128, 2, 255, 191);
}

TEST_CASE("Hub75Canvas: the same six panels as three to a chain, two chains, make a 384 by 128 canvas") {
    checkPos(0, 64, 128, 3, 0, 0);
    checkPos(16384, 64, 128, 3, 256, 0);
    checkPos(24576, 64, 128, 3, 0, 64);
    checkPos(49151, 64, 128, 3, 383, 127);
}

TEST_CASE("Hub75Canvas: every light lands on its own cell and the cells fill the canvas exactly") {
    struct Config { uint16_t rows, cols; uint32_t panels; uint16_t chain; };
    const Config configs[] = {{64, 128, 6, 2}, {64, 128, 6, 3}, {32, 64, 4, 2}, {16, 32, 3, 3}, {8, 16, 5, 1}, {64, 64, 6, 6}};
    for (const Config& c : configs) {
        const uint32_t width = static_cast<uint32_t>(c.cols) * c.chain;
        const uint32_t height = static_cast<uint32_t>(c.rows) * (c.panels / c.chain);
        std::vector<bool> seen(static_cast<size_t>(width) * height, false);
        bool inside = true, unique = true;
        const uint32_t lights = c.panels * c.rows * c.cols;
        for (uint32_t i = 0; i < lights; i++) {
            const CanvasPos p = chainCanvasPos(i, c.rows, c.cols, c.chain);
            if (p.x >= width || p.y >= height) { inside = false; continue; }
            const size_t cell = static_cast<size_t>(p.y) * width + p.x;
            if (seen[cell]) unique = false;
            seen[cell] = true;
        }
        INFO("rows " << c.rows << " cols " << c.cols << " panels " << c.panels << " chain " << c.chain);
        CHECK(inside);
        CHECK(unique);
        CHECK(lights == width * height);   // as many lights as cells, so unique and inside mean every cell is filled
    }
}

TEST_CASE("Hub75Canvas: refresh falls with chain length, and 8 bits beats 11") {
    // 128 by 64 panels at 1/32 scan, the 130 ns default LSB: the page's table.
    const uint16_t eight[] = {420, 243, 165, 124, 100, 83};
    const uint16_t eleven[] = {102, 86, 74, 65, 57, 50};
    for (uint32_t n = 1; n <= 6; n++) {
        INFO("panels in the chain " << n);
        CHECK(hub75PredictedHz(128 * n, 32, 8) == eight[n - 1]);
        CHECK(hub75PredictedHz(128 * n, 32, 11) == eleven[n - 1]);
    }
    CHECK(hub75PredictedHz(256, 32, 11, 50) == 142);   // the page's 50 ns figure for two panels
}

TEST_CASE("Hub75Canvas: an old small panel keeps a long chain fast") {
    CHECK(hub75PredictedHz(32 * 15, 8, 8) == 530);     // a 32 by 16 panel at 1/8 scan, fifteen on a chain
    CHECK(hub75PredictedHz(32 * 30, 8, 8) == 267);
}

namespace {

void checkArr(const Arrangement& a, uint16_t chain, uint16_t parallel, uint32_t w, uint32_t h, uint16_t hz) {
    INFO("arrangement " << a.chain << " x " << a.parallel);
    CHECK(a.chain == chain);
    CHECK(a.parallel == parallel);
    CHECK(a.width == w);
    CHECK(a.height == h);
    CHECK(a.hz == hz);
}

} // namespace

TEST_CASE("Hub75Canvas: six panels are offered wide before deep") {
    Arrangement a[4];
    REQUIRE(panelArrangements(6, panel(64, 128, 32, 8), 3, a, 4) == 3);
    checkArr(a[0], 2, 3, 256, 192, 243);
    checkArr(a[1], 3, 2, 384, 128, 165);
    checkArr(a[2], 6, 1, 768, 64, 83);
}

TEST_CASE("Hub75Canvas: two, four and five panels have the arrangements that use them all") {
    Arrangement a[4];
    REQUIRE(panelArrangements(2, panel(64, 128, 32, 8), 3, a, 4) == 2);
    checkArr(a[0], 1, 2, 128, 128, 420);
    checkArr(a[1], 2, 1, 256, 64, 243);
    REQUIRE(panelArrangements(4, panel(64, 128, 32, 8), 3, a, 4) == 2);
    checkArr(a[0], 2, 2, 256, 128, 243);
    checkArr(a[1], 4, 1, 512, 64, 124);
    REQUIRE(panelArrangements(5, panel(64, 128, 32, 8), 3, a, 4) == 1);
    checkArr(a[0], 5, 1, 640, 64, 100);
}

TEST_CASE("Hub75Canvas: the mapping's chain limit bounds how wide the offer spreads") {
    Arrangement a[4];
    REQUIRE(panelArrangements(12, panel(64, 128, 32, 8), 3, a, 4) == 3);
    checkArr(a[0], 4, 3, 512, 192, 124);
    checkArr(a[1], 6, 2, 768, 128, 83);
    checkArr(a[2], 12, 1, 1536, 64, 42);
    REQUIRE(panelArrangements(6, panel(64, 128, 32, 8), 1, a, 4) == 1);   // one chain only, as the adafruit-hat mapping
    checkArr(a[0], 6, 1, 768, 64, 83);
}

TEST_CASE("Hub75Canvas: no arrangement chains more panels than the card's chain control takes") {
    Arrangement a[4];
    REQUIRE(panelArrangements(40, panel(32, 64, 16, 8), 3, a, 4) == 1);   // 1 chain of 40 is past kChainMax
    CHECK(a[0].chain == 20);
    CHECK(a[0].parallel == 2);
    CHECK(panelArrangements(41, panel(32, 64, 16, 8), 3, a, 4) == 0);    // 41 divides only into a chain of 41
}

TEST_CASE("Hub75Canvas: a full buffer stops the list without writing past it") {
    Arrangement a[2];
    CHECK(panelArrangements(6, panel(64, 128, 32, 8), 3, a, 2) == 2);
}

TEST_CASE("Hub75Canvas: keeping the chains already built means fewer panels or more panels, never a recable") {
    KeepChains k = keepChains(7, panel(64, 128, 32, 8), 3, 3);
    checkArr(k.use, 3, 2, 384, 128, 165);
    CHECK(k.spare == 1);
    checkArr(k.add, 3, 3, 384, 192, 165);
    CHECK(k.added == 2);

    k = keepChains(5, panel(64, 128, 32, 8), 2, 3);
    checkArr(k.use, 2, 2, 256, 128, 243);
    CHECK(k.spare == 1);
    checkArr(k.add, 2, 3, 256, 192, 243);
    CHECK(k.added == 1);

    k = keepChains(6, panel(64, 128, 32, 8), 4, 3);
    checkArr(k.use, 4, 1, 512, 64, 124);
    CHECK(k.spare == 2);
    checkArr(k.add, 4, 2, 512, 128, 124);
    CHECK(k.added == 2);
}

TEST_CASE("Hub75Canvas: adding panels is not offered past the mapping's chain limit") {
    const KeepChains k = keepChains(10, panel(64, 128, 32, 8), 3, 3);
    checkArr(k.use, 3, 3, 384, 192, 165);
    CHECK(k.spare == 1);
    CHECK(k.add.chain == 0);   // a fourth chain exceeds the mapping
    const KeepChains fits = keepChains(6, panel(64, 128, 32, 8), 3, 3);
    checkArr(fits.use, 3, 2, 384, 128, 165);
    CHECK(fits.spare == 0);
    CHECK(fits.add.chain == 0);   // already a whole number of chains
}

TEST_CASE("Hub75Canvas: keeping the chains caps the panels at what the mapping's outputs can carry") {
    // Six panels in chains of 2 want three outputs; a mapping with one carries 2 of them, the other 4 spare.
    const KeepChains k = keepChains(6, panel(64, 128, 32, 8), 2, 1);
    checkArr(k.use, 2, 1, 256, 64, 243);
    CHECK(k.spare == 4);
    CHECK(k.add.chain == 0);
}

TEST_CASE("Hub75Canvas: the card's LSB time moves every rate the offers quote") {
    // Two 128 by 64 panels at 11 bits, the page's own figures: 86 Hz in one chain at the 130 ns default, 142 Hz at 50 ns.
    Arrangement a[4];
    REQUIRE(panelArrangements(2, panel(64, 128, 32, 11), 3, a, 4) == 2);
    checkArr(a[0], 1, 2, 128, 128, 102);
    checkArr(a[1], 2, 1, 256, 64, 86);
    REQUIRE(panelArrangements(2, panel(64, 128, 32, 11, 50), 3, a, 4) == 2);
    checkArr(a[0], 1, 2, 128, 128, 201);
    checkArr(a[1], 2, 1, 256, 64, 142);
    const KeepChains k = keepChains(3, panel(64, 128, 32, 11, 50), 2, 3);
    checkArr(k.use, 2, 1, 256, 64, 142);
}

// Below 11 bits the Pi library keeps each remaining plane's long lit time, so a chain is on-time bound; the rates are the formula by hand, planes from 11 minus the depth.

TEST_CASE("Hub75Canvas: the library at 8 bits clocks planes 3 to 10, so its rate falls less with the chain than a bits-from-zero driver's") {
    const uint16_t library[] = {111, 100, 89, 80, 72, 65};   // 128 by 64 panels, chain 1 to 6, 130 ns, no dither
    for (uint32_t n = 1; n <= 6; n++) {
        INFO("panels in the chain " << n);
        CHECK(hub75PredictedHz(128 * n, 32, 8, 130, 3) == library[n - 1]);
    }
    CHECK(hub75PredictedHz(256, 32, 8, 50, 3) == 181);   // a shorter LSB shortens every plane
}

TEST_CASE("Hub75Canvas: the dither shortens every plane's lit time and lifts the rate") {
    const uint16_t dither2[] = {314, 214, 157, 123, 100, 83};
    for (uint32_t n = 1; n <= 6; n++) {
        INFO("panels in the chain " << n);
        CHECK(hub75PredictedHz(128 * n, 32, 8, 130, 3, 2) == dither2[n - 1]);
    }
}

TEST_CASE("Hub75Canvas: six panels on the library at 8 bits are offered with the library's rates") {
    Arrangement a[4];
    REQUIRE(panelArrangements(6, panel(64, 128, 32, 8, 130, 3), 3, a, 4) == 3);
    checkArr(a[0], 2, 3, 256, 192, 100);
    checkArr(a[1], 3, 2, 384, 128, 89);
    checkArr(a[2], 6, 1, 768, 64, 65);
}

namespace {

// Feed a layout's lights to the walk in index order, as `place(i)` puts them, and say which panel breaks plain rows.
template <typename Place>
uint32_t offPanel(uint16_t rows, uint16_t cols, uint32_t lights, Place place) {
    PanelWalk walk(rows, cols);
    for (uint32_t i = 0; i < lights; i++) {
        const CanvasPos p = place(i);
        walk.light(i, p.x, p.y);
    }
    return walk.firstOffPanel();
}

}  // namespace

TEST_CASE("Hub75Canvas: panels laid out in cabling order, each in plain rows, walk true") {
    // Six 8 by 16 panels, 2 across and 3 down, chained along each row of panels: the Pi 5 wall in miniature.
    CHECK(offPanel(8, 16, 6 * 128, [](uint32_t i) {
              const uint32_t k = i / 128, j = i % 128;
              return CanvasPos{static_cast<uint16_t>((k % 2) * 16 + j % 16), static_cast<uint16_t>((k / 2) * 8 + j / 16)};
          }) == PanelWalk::kNone);
    // The same panels chained down each column of panels (YX): another cabling, still plain rows inside each panel.
    CHECK(offPanel(8, 16, 6 * 128, [](uint32_t i) {
              const uint32_t k = i / 128, j = i % 128;
              return CanvasPos{static_cast<uint16_t>((k / 3) * 16 + j % 16), static_cast<uint16_t>((k % 3) * 8 + j / 16)};
          }) == PanelWalk::kNone);
}

TEST_CASE("Hub75Canvas: a panel mounted mirrored or turned a quarter still walks plain rows in its own orientation") {
    CHECK(offPanel(8, 16, 128, [](uint32_t j) {   // rows run right to left
              return CanvasPos{static_cast<uint16_t>(15 - j % 16), static_cast<uint16_t>(j / 16)};
          }) == PanelWalk::kNone);
    CHECK(offPanel(8, 16, 128, [](uint32_t j) {   // portrait: a panel row runs down the picture
              return CanvasPos{static_cast<uint16_t>(7 - j / 16), static_cast<uint16_t>(j % 16)};
          }) == PanelWalk::kNone);
}

TEST_CASE("Hub75Canvas: one raster across two chained panels breaks the first panel's rows") {
    // A 32 by 8 raster: light 16 sits at x 16 of row 0, where the panel's own second row should start.
    CHECK(offPanel(8, 16, 256, [](uint32_t i) { return CanvasPos{static_cast<uint16_t>(i % 32), static_cast<uint16_t>(i / 32)}; }) == 0);
}

TEST_CASE("Hub75Canvas: a snaked panel breaks plain rows, and the walk names the first such panel") {
    const auto snakeSecond = [](uint32_t i) {
        const uint32_t k = i / 128, j = i % 128, row = j / 16, col = j % 16;
        const uint32_t x = (k == 1 && row % 2 == 1) ? 15 - col : col;   // only panel 1 snakes
        return CanvasPos{static_cast<uint16_t>(k * 16 + x), static_cast<uint16_t>(row)};
    };
    CHECK(offPanel(8, 16, 2 * 128, snakeSecond) == 1);
}
