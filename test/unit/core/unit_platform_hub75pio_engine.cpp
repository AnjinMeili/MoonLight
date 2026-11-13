/// @module platform

/// Pins the PIO engine against the stub backend: the options it refuses, the clock divider it rounds, the wide words its refresh thread submits for a known frame, and the blank frame its shutdown leaves. No hardware is touched; the stub captures every submit.

#include "doctest.h"
#include "platform/platform.h"
#include "platform/rpi/hub75_pio_backend_stub.h"
#include "platform/rpi/hub75_pio_engine.h"
#include "platform/rpi/hub75_pio_pins.h"
#include "platform/rpi/hub75_pio_repack.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

// One 32x64 panel at 1/16 scan, 2-bit: 2 planes x 16 rows x 64 columns, 2,048 slots a frame.
mm::platform::Hub75PioOptions small() {
    mm::platform::Hub75PioOptions o;
    o.rows = 32;
    o.cols = 64;
    o.chain = 1;
    o.parallel = 1;
    o.scanRate = 16;
    o.bitDepth = 2;
    o.pixelClockHz = 10000000;
    return o;
}

// Wait up to `ms` for the stub to have submitted a frame whose word `index` has `bit` set, or all of `expect` when `bit` is -1.
bool waitForWord(size_t index, int bit, uint32_t expect, int ms = 2000) {
    std::vector<uint32_t> frame(2048);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (mm::platform::stubBackendCopyLastFrame(frame.data(), frame.size()) > index) {
            if (bit >= 0 ? (frame[index] & (1u << bit)) != 0 : frame[index] == expect) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

}  // namespace

TEST_CASE("Hub75PioEngine: the options are refused with reasons before anything opens") {
    std::string error;
    mm::platform::Hub75PioOptions o = small();
    CHECK(mm::platform::hub75PioCheckOptions(o, error));

    o = small(); o.parallel = 0;
    CHECK_FALSE(mm::platform::hub75PioCheckOptions(o, error));
    o = small(); o.parallel = 4;
    CHECK_FALSE(mm::platform::hub75PioCheckOptions(o, error));
    o = small(); o.chain = 0;
    CHECK_FALSE(mm::platform::hub75PioCheckOptions(o, error));
    o = small(); o.chain = 33;
    CHECK_FALSE(mm::platform::hub75PioCheckOptions(o, error));
    o = small(); o.rows = 0;
    CHECK_FALSE(mm::platform::hub75PioCheckOptions(o, error));
    o = small(); o.scanRate = 64;
    CHECK_FALSE(mm::platform::hub75PioCheckOptions(o, error));
    o = small(); o.bitDepth = 1;
    CHECK_FALSE(mm::platform::hub75PioCheckOptions(o, error));
    o = small(); o.bitDepth = 7;
    CHECK_FALSE(mm::platform::hub75PioCheckOptions(o, error));
    o = small(); o.latchBlankWords = 9;
    CHECK_FALSE(mm::platform::hub75PioCheckOptions(o, error));
    o = small(); o.pixelClockHz = 0;
    CHECK_FALSE(mm::platform::hub75PioCheckOptions(o, error));
    // A 32-high panel at 1/32 scan: the encoder needs an even rows-per-scan.
    o = small(); o.scanRate = 32;
    CHECK_FALSE(mm::platform::hub75PioCheckOptions(o, error));
    CHECK(error.find("32-high at 1/16") != std::string::npos);

    // The engine refuses the same geometry on open, and nothing is left running.
    mm::platform::Hub75PioEngine engine;
    CHECK_FALSE(engine.open(o, error, true));
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("Hub75PioEngine: a frame publishes and submits, and the geometry reads back") {
    mm::platform::stubBackendResetCapture();
    mm::platform::Hub75PioEngine engine;
    std::string error;
    REQUIRE(engine.open(small(), error, true));
    CHECK(engine.isRunning());
    CHECK(std::string(engine.backendName()) == "stub (no hardware)");
    CHECK(engine.width() == 64);
    CHECK(engine.height() == 32);
    CHECK(engine.chains() == 1);
    CHECK(engine.frameSlots() == 2048);
    CHECK(engine.pixelClockHz() == 10000000);   // a 4.0 divider, exact
    CHECK(std::abs(engine.refreshHz() - 10000000.0 / 2048.0) < 1.0);
    CHECK(engine.dmaBytesPerSecond() == 40000000);

    std::vector<uint8_t> frame(64 * 32 * 3, 0);
    engine.publishRgb(frame.data());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (engine.framesSubmitted() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(engine.framesSubmitted() > 0);
    CHECK(engine.framesEncoded() > 0);
    CHECK(engine.framesPublished() == 1);
    CHECK(engine.submitErrors() == 0);
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("Hub75PioEngine: the pixel clock rounds to the divider, and tops out at 40 MHz") {
    std::string error;
    mm::platform::Hub75PioEngine engine;
    REQUIRE(engine.open(small(), error, true));
    CHECK(engine.pixelClockHz() == 10000000);
    engine.shutdown();

    mm::platform::Hub75PioOptions o = small();
    o.pixelClockHz = 12000000;   // a 3.333 divider: 3 + 85/256
    REQUIRE(engine.open(o, error, true));
    CHECK(std::abs(static_cast<int64_t>(engine.pixelClockHz()) - 12000000) < 12000);   // within 0.1%
    engine.shutdown();

    o.pixelClockHz = 50000000;   // past the divider's floor: 40 MHz
    REQUIRE(engine.open(o, error, true));
    CHECK(engine.pixelClockHz() == 40000000);
    engine.shutdown();
}

TEST_CASE("Hub75PioEngine: a red pixel at (0,0) lights the top plane's first word, and the row ends on a dark latch") {
    mm::platform::stubBackendResetCapture();
    mm::platform::Hub75PioEngine engine;
    std::string error;
    REQUIRE(engine.open(small(), error, true));

    std::vector<uint8_t> frame(64 * 32 * 3, 0);
    frame[0] = 255;   // pixel (0,0), red
    engine.publishRgb(frame.data());
    // Plane 1, row 0, column 0: slot 1024. R1 of chain 0 is GPIO 11, wide bit 9; the address names the last row, 15, on A-D; OE is clear, the panel lit.
    REQUIRE(waitForWord(1024, 9, 0));
    std::vector<uint32_t> last(2048);
    REQUIRE(mm::platform::stubBackendCopyLastFrame(last.data(), last.size()) == 2048);
    CHECK(last[1024] == 0x00F00200u);
    // Row 0's last word latches in the dark: STB set, OE set (blanked), no colour.
    CHECK(last[63] == ((1u << mm::platform::kWideBitStb) | (1u << mm::platform::kWideBitOe)));
    engine.shutdown();
}

TEST_CASE("Hub75PioEngine: the repack tables match the reference, colour by colour and control by control") {
    const mm::platform::RepackTables tables = mm::platform::buildRepackTables();
    // Every colour combination on each chain, the other chains dark.
    for (int chain = 0; chain < 3; chain++) {
        for (uint16_t v = 0; v < 64; v++) {
            uint16_t slot[3] = {0, 0, 0};
            slot[chain] = v;
            const uint16_t* ptrs[3] = {&slot[0], &slot[1], &slot[2]};
            uint32_t wide = 0;
            mm::platform::repackSlots(ptrs, 3, 1, tables, &wide);
            CHECK(wide == mm::platform::packWideWordReference(slot[0], slot[1], slot[2], 3));
        }
    }
    // Every control combination: the seven control bits ride chain 0's word.
    for (uint16_t v = 0; v < 128; v++) {
        uint16_t slot[3] = {static_cast<uint16_t>(v << 8), 0, 0};
        const uint16_t* ptrs[3] = {&slot[0], &slot[1], &slot[2]};
        uint32_t wide = 0;
        mm::platform::repackSlots(ptrs, 3, 1, tables, &wide);
        CHECK(wide == mm::platform::packWideWordReference(slot[0], slot[1], slot[2], 3));
    }
    // One chain only: the other chains' colour never reaches the word.
    {
        uint16_t slot[3] = {0x003F, 0x003F, 0x003F};
        const uint16_t* ptrs[3] = {&slot[0], &slot[1], &slot[2]};
        uint32_t wide = 0;
        mm::platform::repackSlots(ptrs, 1, 1, tables, &wide);
        CHECK(wide == mm::platform::packWideWordReference(slot[0], slot[1], slot[2], 1));
        CHECK(wide == mm::platform::packWideWordReference(0x003F, 0, 0, 3));
    }
}

TEST_CASE("Hub75PioEngine: latch blanking darkens the words before each latch, and only those") {
    const mm::platform::RepackTables tables = mm::platform::buildRepackTables();
    std::vector<uint32_t> wide(2048, 0);   // every word lit: OE clear throughout
    mm::platform::applyLatchBlanking(wide.data(), wide.size(), 64, 2);
    for (size_t i = 0; i < wide.size(); i++) {
        const bool blanked = (wide[i] & (1u << mm::platform::kWideBitOe)) != 0;
        const size_t inRow = i % 64;
        CHECK(blanked == (inRow == 61 || inRow == 62));   // the two words before each row's latch word, the latch itself untouched
    }
}

TEST_CASE("Hub75PioEngine: a second open is refused, and shutdown is idempotent and leaves a blank frame last") {
    mm::platform::stubBackendResetCapture();
    mm::platform::Hub75PioEngine engine;
    std::string error;
    REQUIRE(engine.open(small(), error, true));
    CHECK_FALSE(engine.open(small(), error, true));
    CHECK(error == "PIO engine already running");

    std::vector<uint8_t> frame(64 * 32 * 3, 255);   // all white, so the blank after it is unmistakable
    engine.publishRgb(frame.data());
    REQUIRE(waitForWord(1024, 9, 0));
    engine.shutdown();
    engine.shutdown();   // a second shutdown is harmless
    CHECK_FALSE(engine.isRunning());
    std::vector<uint32_t> last(2048);
    REQUIRE(mm::platform::stubBackendCopyLastFrame(last.data(), last.size()) == 2048);
    for (uint32_t w : last) CHECK(w == mm::platform::kBlankWideWord);

    // The engine opens again after a shutdown.
    REQUIRE(engine.open(small(), error, true));
    engine.shutdown();
}
