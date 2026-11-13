/// @module platform

/// Pins the GPIO engine against the stub backend: the options it refuses, the PWM timing it derives, the GPIO words its refresh thread submits for a known frame, the dark-first setup order, and the blank frame its shutdown leaves. No hardware is touched; the stub captures every submit and logs every setup step.

#include "doctest.h"
#include "platform/platform.h"
#include "platform/rpi/hub75_gpio_backend.h"
#include "platform/rpi/hub75_gpio_backend_stub.h"
#include "platform/rpi/hub75_gpio_engine.h"
#include "platform/rpi/hub75_gpio_multiplex.h"
#include "platform/rpi/hub75_gpio_pins.h"
#include "platform/rpi/hub75_gpio_pwm.h"
#include "platform/rpi/hub75_gpio_repack.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

// One 32x64 panel at 1/16 scan, 2-bit: 2 planes x 16 rows x 64 columns, 2,048 slots a frame.
mm::platform::Hub75GpioOptions small() {
    mm::platform::Hub75GpioOptions o;
    o.rows = 32;
    o.cols = 64;
    o.chain = 1;
    o.parallel = 1;
    o.scanRate = 16;
    o.bitDepth = 2;
    o.pwmLsbNs = 130;
    o.slowdown = -1;   // auto: the stub resolves it to 1
    return o;
}

// Wait up to `ms` for the stub to have submitted a frame whose word `index` has `bit` set, or all of `expect` when `bit` is -1.
bool waitForWord(size_t index, int bit, uint32_t expect, int ms = 2000) {
    std::vector<uint32_t> frame(2048);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (mm::platform::gpioStubCopyLastFrame(frame.data(), frame.size()) > index) {
            if (bit >= 0 ? (frame[index] & (1u << bit)) != 0 : frame[index] == expect) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

}  // namespace

TEST_CASE("Hub75GpioEngine: the options are refused with reasons before anything opens") {
    std::string error;
    mm::platform::Hub75GpioOptions o = small();
    CHECK(mm::platform::hub75GpioCheckOptions(o, error));

    o = small(); o.parallel = 0;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = small(); o.parallel = 4;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = small(); o.chain = 0;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = small(); o.chain = 33;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = small(); o.rows = 0;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = small(); o.scanRate = 64;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = small(); o.bitDepth = 1;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = small(); o.bitDepth = 7;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = small(); o.pwmLsbNs = 49;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = small(); o.pwmLsbNs = 3001;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = small(); o.slowdown = -2;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = small(); o.slowdown = 5;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = small(); o.multiplexing = 2;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    // Stripe is the one mapper the engine runs -- at the driven scan,
    // rows/4: a 32-high Stripe panel is 1/8, and the direct 1/16 scan
    // with Stripe is refused.
    o = small(); o.multiplexing = 1; o.scanRate = 8;
    CHECK(mm::platform::hub75GpioCheckOptions(o, error));
    o = small(); o.multiplexing = 1;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    CHECK(error.find("Stripe") != std::string::npos);
    // A 32-high panel at 1/32 scan: the encoder needs an even rows-per-scan.
    o = small(); o.scanRate = 32;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    CHECK(error.find("32-high at 1/16") != std::string::npos);

    // The engine refuses the same geometry on open, and nothing is left running.
    mm::platform::Hub75GpioEngine engine;
    CHECK_FALSE(engine.open(o, error, true));
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("Hub75GpioEngine: a frame publishes and submits, and the geometry reads back") {
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    std::string error;
    REQUIRE(engine.open(small(), error, true));
    CHECK(engine.isRunning());
    CHECK(std::string(engine.backendName()) == "stub (no hardware)");
    CHECK(engine.width() == 64);
    CHECK(engine.height() == 32);
    CHECK(engine.chains() == 1);
    CHECK(engine.frameSlots() == 2048);
    CHECK(engine.columnTimeNs() == 78);   // auto slowdown resolves to 1 on the stub
    CHECK(engine.lsbActualNs() == 128);   // 130 ns asks divider 32: 128 ns actual
    // Predicted refresh, computed: 2 planes x 16 rows x (64 cols x 78 ns + 0.5 us dark) = 175,744 ns.
    CHECK(std::abs(engine.refreshHz() - 1e9 / 175744.0) < 1.0);

    std::vector<uint8_t> frame(64 * 32 * 3, 0);
    engine.publishRgb(frame.data());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (engine.framesSubmitted() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(engine.framesSubmitted() > 0);
    CHECK(engine.framesEncoded() > 0);
    CHECK(engine.framesPublished() == 1);
    CHECK(engine.submitErrors() == 0);
    CHECK(engine.pulseTimeouts() == 0);
    CHECK(engine.lastSubmitUs() > 0);   // the stub slept the wire time
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("Hub75GpioEngine: the PWM clocking quantises the LSB to a multiple of 4 ns") {
    const mm::platform::GpioPwmTiming fast = mm::platform::hub75GpioPwmTiming(50);
    CHECK(fast.divider == 12);
    CHECK(fast.tickNs == 24);
    CHECK(fast.lsbActualNs == 48);

    const mm::platform::GpioPwmTiming def = mm::platform::hub75GpioPwmTiming(130);
    CHECK(def.divider == 32);
    CHECK(def.tickNs == 64);
    CHECK(def.lsbActualNs == 128);

    const mm::platform::GpioPwmTiming slow = mm::platform::hub75GpioPwmTiming(3000);
    CHECK(slow.divider == 750);
    CHECK(slow.tickNs == 1500);
    CHECK(slow.lsbActualNs == 3000);

    // Truncation, not rounding: 51 ns takes divider 12, like 50 ns.
    CHECK(mm::platform::hub75GpioPwmTiming(51).divider == 12);
    // The divider clamps to the 12-bit field.
    CHECK(mm::platform::hub75GpioPwmTiming(3000).divider == 750);
    CHECK(mm::platform::hub75GpioPulseTicks(0) == 2);
    CHECK(mm::platform::hub75GpioPulseTicks(5) == 64);
}

TEST_CASE("Hub75GpioEngine: the pulse words split at 16 ticks and carry two zero sentinels") {
    // Plane 0: 2 ticks, one word holding the full range.
    const mm::platform::GpioPulseWords p0 = mm::platform::hub75GpioPulseWords(0);
    CHECK(p0.range == 2);
    CHECK(p0.count == 3);
    CHECK(p0.words[0] == 2);
    CHECK(p0.words[1] == 0);
    CHECK(p0.words[2] == 0);

    // Plane 2: 8 ticks, still one word.
    const mm::platform::GpioPulseWords p2 = mm::platform::hub75GpioPulseWords(2);
    CHECK(p2.range == 8);
    CHECK(p2.count == 3);

    // Plane 3: 16 ticks, the split boundary -- eight words of 2.
    const mm::platform::GpioPulseWords p3 = mm::platform::hub75GpioPulseWords(3);
    CHECK(p3.range == 2);
    CHECK(p3.count == 10);
    for (int i = 0; i < 8; i++) CHECK(p3.words[i] == 2);
    CHECK(p3.words[8] == 0);
    CHECK(p3.words[9] == 0);

    // Plane 5: 64 ticks -- eight words of 8.
    const mm::platform::GpioPulseWords p5 = mm::platform::hub75GpioPulseWords(5);
    CHECK(p5.range == 8);
    CHECK(p5.count == 10);
    uint16_t total = 0;
    for (int i = 0; i < 8; i++) total = static_cast<uint16_t>(total + p5.words[i]);
    CHECK(total == 64);
}

TEST_CASE("Hub75GpioEngine: the column-time model anchors at 78 ns for slowdown 1") {
    CHECK(mm::platform::hub75GpioColumnTimeNs(0) == 66);
    CHECK(mm::platform::hub75GpioColumnTimeNs(1) == 78);
    CHECK(mm::platform::hub75GpioColumnTimeNs(2) == 90);
    CHECK(mm::platform::hub75GpioColumnTimeNs(4) == 114);
    CHECK(mm::platform::hub75GpioColumnTimeNs(-1) == 66);   // clamped
    CHECK(mm::platform::hub75GpioColumnTimeNs(9) == 114);   // clamped
    CHECK(mm::platform::hub75GpioAutoSlowdown(mm::platform::Hub75GpioModel::Pi4) == 2);
    CHECK(mm::platform::hub75GpioAutoSlowdown(mm::platform::Hub75GpioModel::Pi3) == 1);
    CHECK(mm::platform::hub75GpioAutoSlowdown(mm::platform::Hub75GpioModel::Unknown) == 1);
    // The one home for auto slowdown: the resolver the backends and the output's rate prediction share.
    CHECK(mm::platform::hub75GpioResolveSlowdown(-1, mm::platform::Hub75GpioModel::Pi4) == 2);
    CHECK(mm::platform::hub75GpioResolveSlowdown(-1, mm::platform::Hub75GpioModel::Pi3) == 1);
    CHECK(mm::platform::hub75GpioResolveSlowdown(-1, mm::platform::Hub75GpioModel::Unknown) == 1);
    CHECK(mm::platform::hub75GpioResolveSlowdown(3, mm::platform::Hub75GpioModel::Pi4) == 3);
    CHECK(mm::platform::hub75GpioResolveSlowdown(0, mm::platform::Hub75GpioModel::Pi4) == 0);
}

TEST_CASE("Hub75GpioEngine: the model detection reads the device tree first and the revision second") {
    using mm::platform::Hub75GpioModel;
    CHECK(mm::platform::hub75GpioDetectModel("Raspberry Pi 4 Model B Rev 1.2", nullptr) == Hub75GpioModel::Pi4);
    CHECK(mm::platform::hub75GpioDetectModel("Raspberry Pi 3 Model B Plus Rev 1.3", nullptr) == Hub75GpioModel::Pi3);
    CHECK(mm::platform::hub75GpioDetectModel("Raspberry Pi 5 Model B Rev 1.0", nullptr) == Hub75GpioModel::Pi5);
    CHECK(mm::platform::hub75GpioDetectModel("Raspberry Pi Compute Module 4 Rev 1.0", nullptr) == Hub75GpioModel::Pi4);
    CHECK(mm::platform::hub75GpioDetectModel("Raspberry Pi Compute Module 3+ Rev 1.0", nullptr) == Hub75GpioModel::Pi3);
    CHECK(mm::platform::hub75GpioDetectModel(nullptr, "a02082") == Hub75GpioModel::Pi3);   // 3B
    CHECK(mm::platform::hub75GpioDetectModel(nullptr, "a020d3") == Hub75GpioModel::Pi3);   // 3B+
    CHECK(mm::platform::hub75GpioDetectModel(nullptr, "a03140") == Hub75GpioModel::Pi4);   // CM4
    CHECK(mm::platform::hub75GpioDetectModel(nullptr, "c03130") == Hub75GpioModel::Pi4);   // 400
    CHECK(mm::platform::hub75GpioDetectModel(nullptr, "c04170") == Hub75GpioModel::Pi5);   // 5B
    CHECK(mm::platform::hub75GpioDetectModel(nullptr, "") == Hub75GpioModel::Unknown);
    CHECK(mm::platform::hub75GpioDetectModel(nullptr, nullptr) == Hub75GpioModel::Unknown);
    CHECK(mm::platform::hub75GpioDetectModel("something else entirely", nullptr) == Hub75GpioModel::Unknown);

    CHECK(mm::platform::hub75GpioPeripheralBase(Hub75GpioModel::Pi3) == 0x3F000000u);
    CHECK(mm::platform::hub75GpioPeripheralBase(Hub75GpioModel::Pi4) == 0xFE000000u);
    CHECK(mm::platform::hub75GpioPeripheralBase(Hub75GpioModel::Pi5) == 0u);
    CHECK(mm::platform::hub75GpioPeripheralBase(Hub75GpioModel::Unknown) == 0u);
}

TEST_CASE("Hub75GpioEngine: the predicted frame time is shift-bound at 130 ns and OE-bound at 3000 ns") {
    // The wall, 6-bit, 130 ns LSB, 78 ns columns: every plane is
    // shift-bound -- 6 planes x 32 rows x (256 cols x 78 ns + 0.5 us) = 3,929,856 ns, ~254 Hz.
    CHECK(mm::platform::hub75GpioPredictedFrameNs(32, 256, 6, 130, 78) == 3929856ull);
    // The wall at 3000 ns LSB: planes 0-2 shift-bound, planes 3-5
    // OE-bound (24 us, 48 us, 96 us) -- 32 x (3 x 20,468 + 24,000 + 48,000 + 96,000) = 7,340,928 ns, ~136 Hz.
    CHECK(mm::platform::hub75GpioPredictedFrameNs(32, 256, 6, 3000, 78) == 7340928ull);
    // One 64x64 panel, 6-bit, 130 ns: 6 x 32 x (64 x 78 + 500) = 1,054,464 ns, ~948 Hz.
    CHECK(mm::platform::hub75GpioPredictedFrameNs(32, 64, 6, 130, 78) == 1054464ull);
    // Stripe on the small panel: 2 planes x 8 rows x (128 cols x 78 ns + 0.5 us dark) = 167,744 ns -- the same
    // pixel clocks as direct, half the dark intervals.
    CHECK(mm::platform::hub75GpioPredictedFrameNs(8, 128, 2, 130, 78) == 167744ull);
    // The sign, 6-bit, 160 ns LSB, slowdown 3 (102 ns columns): every plane shift-bound --
    // 6 x 8 x (128 x 102 + 500) = 650,688 ns, ~1,537 Hz computed.
    CHECK(mm::platform::hub75GpioPredictedFrameNs(8, 128, 6, 160, 102) == 650688ull);
    // The sign at 3000 ns LSB: planes 0-2 shift-bound, planes 3-5 OE-bound (24 us, 48 us, 96 us) --
    // 8 x (3 x 13,556 + 24,000 + 48,000 + 96,000) = 1,669,344 ns.
    CHECK(mm::platform::hub75GpioPredictedFrameNs(8, 128, 6, 3000, 102) == 1669344ull);
}

TEST_CASE("Hub75GpioEngine: a red pixel at (0,0) lights the top plane's first word, and no control bit leaks into the stream") {
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    std::string error;
    REQUIRE(engine.open(small(), error, true));

    std::vector<uint8_t> frame(64 * 32 * 3, 0);
    frame[0] = 128;   // pixel (0,0), red at half: plane 0 dark, plane 1 lit (bit 7 set, bit 6 clear)
    engine.publishRgb(frame.data());
    // Plane 1, row 0, column 0: slot 1024. R1 of chain 0 is GPIO 11.
    REQUIRE(waitForWord(1024, 11, 0));
    std::vector<uint32_t> last(2048);
    REQUIRE(mm::platform::gpioStubCopyLastFrame(last.data(), last.size()) == 2048);
    CHECK(last[1024] == 0x800u);   // only GPIO 11: the colour bit, no address, no latch, no OE
    CHECK(last[0] == 0u);          // plane 0: the half-red pixel's low bit is clear
    CHECK(last[63] == 0u);         // row 0's last word: no colour, and the latch rides the CPU, not the stream
    engine.shutdown();
}

TEST_CASE("Hub75GpioEngine: the Stripe map folds a 32x64 panel into 16 driven rows x 128 columns") {
    using mm::platform::hub75GpioStripeMap;
    // Every row of a 32x64 panel: driven row (y/16)*8 + y%8, columns
    // +64 where (y%16) < 8 -- both halves of each 16-row group, at both
    // ends of the row.
    for (uint8_t y = 0; y < 32; y++) {
        const auto p0 = hub75GpioStripeMap(0, y, 32, 64);
        const auto p63 = hub75GpioStripeMap(63, y, 32, 64);
        const uint8_t expectRow = static_cast<uint8_t>((y / 16) * 8 + y % 8);
        const bool fold = (y % 16) < 8;
        CHECK(p0.y == expectRow);
        CHECK(p63.y == expectRow);
        CHECK(p0.x == (fold ? 64 : 0));
        CHECK(p63.x == (fold ? 127 : 63));
    }
    // The fold boundary: (y%16) == 7 folds, == 8 does not, in both groups.
    CHECK(hub75GpioStripeMap(0, 7, 32, 64).x == 64);
    CHECK(hub75GpioStripeMap(0, 8, 32, 64).x == 0);
    CHECK(hub75GpioStripeMap(0, 23, 32, 64).x == 64);
    CHECK(hub75GpioStripeMap(0, 24, 32, 64).x == 0);
    // The address at the group boundaries: y=15 and y=16, and y=31.
    CHECK(hub75GpioStripeMap(0, 15, 32, 64).y == 7);
    CHECK(hub75GpioStripeMap(0, 16, 32, 64).y == 8);
    CHECK(hub75GpioStripeMap(0, 31, 32, 64).y == 15);
    // Two logical rows share each driven row, on opposite halves.
    CHECK(hub75GpioStripeMap(0, 0, 32, 64).y == hub75GpioStripeMap(0, 8, 32, 64).y);
    CHECK(hub75GpioStripeMap(0, 0, 32, 64).x == 64);
    CHECK(hub75GpioStripeMap(0, 8, 32, 64).x == 0);
    CHECK(hub75GpioStripeMap(0, 16, 32, 64).y == hub75GpioStripeMap(0, 24, 32, 64).y);
    // A 64-row panel folds the same way, in 32-row groups.
    CHECK(hub75GpioStripeMap(0, 0, 64, 64).y == 0);
    CHECK(hub75GpioStripeMap(0, 15, 64, 64).x == 64);
    CHECK(hub75GpioStripeMap(0, 15, 64, 64).y == 15);
    CHECK(hub75GpioStripeMap(0, 16, 64, 64).x == 0);
    CHECK(hub75GpioStripeMap(0, 16, 64, 64).y == 0);
    CHECK(hub75GpioStripeMap(0, 31, 64, 64).y == 15);
    CHECK(hub75GpioStripeMap(0, 32, 64, 64).y == 16);
    CHECK(hub75GpioStripeMap(0, 63, 64, 64).y == 31);
    CHECK(hub75GpioStripeMap(0, 63, 64, 64).x == 0);
    // A panel too short to fold maps to itself.
    CHECK(hub75GpioStripeMap(5, 1, 2, 64).x == 5);
    CHECK(hub75GpioStripeMap(5, 1, 2, 64).y == 1);
}

TEST_CASE("Hub75GpioEngine: Stripe halves the scan and the rows and doubles the columns; normal is unchanged") {
    using mm::platform::hub75GpioMultiplexScanRate;
    using mm::platform::hub75GpioMultiplexDrivenRows;
    using mm::platform::hub75GpioMultiplexDrivenCols;
    CHECK(hub75GpioMultiplexScanRate(0, 32) == 16);
    CHECK(hub75GpioMultiplexScanRate(0, 64) == 32);
    CHECK(hub75GpioMultiplexScanRate(1, 32) == 8);   // the sign: 1/8 scan of the driven 16 rows
    CHECK(hub75GpioMultiplexScanRate(1, 64) == 16);
    CHECK(hub75GpioMultiplexDrivenRows(0, 32) == 32);
    CHECK(hub75GpioMultiplexDrivenRows(1, 32) == 16);
    CHECK(hub75GpioMultiplexDrivenRows(1, 64) == 32);
    CHECK(hub75GpioMultiplexDrivenCols(0, 64) == 64);
    CHECK(hub75GpioMultiplexDrivenCols(1, 64) == 128);
    CHECK(hub75GpioMultiplexDrivenCols(1, 128) == 256);
    // An unknown type is direct.
    CHECK(hub75GpioMultiplexScanRate(2, 32) == 16);
    CHECK(hub75GpioMultiplexDrivenRows(2, 32) == 32);
    CHECK(hub75GpioMultiplexDrivenCols(2, 64) == 64);
}

TEST_CASE("Hub75GpioEngine: Stripe folds four logical rows onto one address step, and normal leaves each on its own") {
    // One address step, four logical rows: rows 1, 9, 17 and 25 all ride
    // step 1 -- rows 1 and 17 folded to column 64 (R1 and R2), rows 9
    // and 25 at column 0 (R1 and R2).
    std::vector<uint8_t> frame(64 * 32 * 3, 0);
    frame[(1 * 64 + 0) * 3] = 128;    // (0,1): driven (64,1), R1, step 1
    frame[(9 * 64 + 0) * 3] = 128;    // (0,9): driven (0,1), R1, step 1
    frame[(17 * 64 + 0) * 3] = 128;   // (0,17): driven (64,9), R2, step 1
    frame[(25 * 64 + 0) * 3] = 128;   // (0,25): driven (0,9), R2, step 1
    std::vector<uint32_t> last(2048);

    mm::platform::Hub75GpioOptions o = small();
    o.multiplexing = 1;
    o.scanRate = 8;   // Stripe drives the 16 driven rows at 1/8
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    std::string error;
    REQUIRE(engine.open(o, error, true));
    // The canvas stays the logical 64x32; the driven frame is 128 wide
    // and 16 high, the same 2,048 slots a frame.
    CHECK(engine.width() == 64);
    CHECK(engine.height() == 32);
    CHECK(engine.drivenWidth() == 128);
    CHECK(engine.drivenHeight() == 16);
    CHECK(engine.frameSlots() == 2048);
    // Predicted refresh, computed: 2 planes x 8 rows x (128 cols x 78 ns + 0.5 us dark) = 167,744 ns.
    CHECK(std::abs(engine.refreshHz() - 1e9 / 167744.0) < 1.0);
    engine.publishRgb(frame.data());
    // Plane 1, step 1: slots 1024 + 128 + x. R1 of chain 0 is GPIO 11, R2 GPIO 8.
    REQUIRE(waitForWord(1024 + 128 + 64, 11, 0));
    REQUIRE(mm::platform::gpioStubCopyLastFrame(last.data(), last.size()) == 2048);
    CHECK(last[1024 + 128 + 64] == 0x900u);   // rows 1 and 17 share the word: R1 + R2, column 64
    CHECK(last[1024 + 128 + 0] == 0x900u);    // rows 9 and 25 share the word: R1 + R2, column 0
    CHECK(last[1024 + 4 * 128] == 0u);        // step 4: nothing -- the old stripes-of-4 shape put row 1 there
    CHECK(last[128 + 64] == 0u);              // plane 0: the half-red pixels' low bit is clear
    CHECK(last[128 + 0] == 0u);
    engine.shutdown();

    // Normal: each row stays on its own step -- rows 1 and 17 share step
    // 1 (R1 and R2), rows 9 and 25 share step 9, all at column 0.
    o.multiplexing = 0;
    o.scanRate = 16;
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine plain;
    REQUIRE(plain.open(o, error, true));
    CHECK(plain.drivenWidth() == 64);
    CHECK(plain.drivenHeight() == 32);
    plain.publishRgb(frame.data());
    REQUIRE(waitForWord(1024 + 1 * 64, 11, 0));
    REQUIRE(mm::platform::gpioStubCopyLastFrame(last.data(), last.size()) == 2048);
    CHECK(last[1024 + 1 * 64] == 0x900u);
    CHECK(last[1024 + 9 * 64] == 0x900u);
    CHECK(last[1024 + 128 + 64] == 0u);   // nothing at the folded word: step 3, column 0 is dark
    plain.shutdown();
}

TEST_CASE("Hub75GpioEngine: the sign's Stripe geometry -- 3 parallel chains, 64x96 logical, 128 driven columns") {
    mm::platform::Hub75GpioOptions o;
    o.rows = 32;
    o.cols = 64;
    o.chain = 1;
    o.parallel = 3;
    o.scanRate = 8;
    o.multiplexing = 1;
    o.bitDepth = 6;
    o.pwmLsbNs = 160;
    o.slowdown = 3;
    std::string error;
    CHECK(mm::platform::hub75GpioCheckOptions(o, error));
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    REQUIRE(engine.open(o, error, true));
    CHECK(engine.width() == 64);
    CHECK(engine.height() == 96);
    CHECK(engine.chains() == 3);
    CHECK(engine.drivenWidth() == 128);
    CHECK(engine.drivenHeight() == 16);
    CHECK(engine.frameSlots() == 6144);   // 6 planes x 8 rows x 128 columns
    CHECK(engine.columnTimeNs() == 102);   // slowdown 3: 66 + 3*12 ns a column
    // Predicted refresh, computed: 6 planes x 8 rows x (128 cols x 102 ns + 0.5 us dark) = 650,688 ns, ~1,537 Hz.
    CHECK(std::abs(engine.refreshHz() - 1e9 / 650688.0) < 1.0);
    std::vector<uint8_t> frame(64 * 96 * 3, 0);
    engine.publishRgb(frame.data());
    // Wait for the encode, not just a submit: the refresh thread
    // re-submits the initial frame while the published one still waits,
    // so a submit alone does not prove the published frame was encoded.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (engine.framesEncoded() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(engine.framesEncoded() > 0);
    while (engine.framesSubmitted() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(engine.framesSubmitted() > 0);
    CHECK(engine.submitErrors() == 0);
    engine.shutdown();
}

TEST_CASE("Hub75GpioEngine: the repack tables match the reference, and control bits never reach the word") {
    const mm::platform::GpioRepackTables tables = mm::platform::buildGpioRepackTables();
    // Every colour combination on each chain, the other chains dark.
    for (int chain = 0; chain < 3; chain++) {
        for (uint16_t v = 0; v < 64; v++) {
            uint16_t slot[3] = {0, 0, 0};
            slot[chain] = v;
            const uint16_t* ptrs[3] = {&slot[0], &slot[1], &slot[2]};
            uint32_t gpio = 0;
            mm::platform::repackSlotsToGpio(ptrs, 3, 1, tables, &gpio);
            CHECK(gpio == mm::platform::packGpioWordReference(slot[0], slot[1], slot[2], 3));
        }
    }
    // Every control combination on chain 0's word: the address, latch
    // and OE bits change nothing -- the dump loop and the pulser own
    // those lines, so the repack must strip them.
    for (uint16_t v = 0; v < 128; v++) {
        uint16_t slot[3] = {static_cast<uint16_t>((v << 8) | 0x3F), 0, 0};
        const uint16_t* ptrs[3] = {&slot[0], &slot[1], &slot[2]};
        uint32_t gpio = 0;
        mm::platform::repackSlotsToGpio(ptrs, 3, 1, tables, &gpio);
        CHECK(gpio == mm::platform::packGpioWordReference(0x003F, 0, 0, 3));
    }
    // One chain only: the other chains' colour never reaches the word.
    {
        uint16_t slot[3] = {0x003F, 0x003F, 0x003F};
        const uint16_t* ptrs[3] = {&slot[0], &slot[1], &slot[2]};
        uint32_t gpio = 0;
        mm::platform::repackSlotsToGpio(ptrs, 1, 1, tables, &gpio);
        CHECK(gpio == mm::platform::packGpioWordReference(slot[0], slot[1], slot[2], 1));
        CHECK(gpio == mm::platform::packGpioWordReference(0x003F, 0, 0, 3));
    }
}

TEST_CASE("Hub75GpioEngine: the setup runs dark values, then directions, then the PWM, then the mux") {
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    std::string error;
    REQUIRE(engine.open(small(), error, true));
    // NOTE.md rule 1, pinned: values, then directions, then the mux --
    // the hardware backend runs the same four steps in the same order.
    CHECK(mm::platform::gpioStubSetupLog() == "dark-values,directions,pwm-config,oe-mux");
    engine.shutdown();
}

TEST_CASE("Hub75GpioEngine: a second open is refused, and shutdown is idempotent and leaves a blank frame last") {
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    std::string error;
    REQUIRE(engine.open(small(), error, true));
    CHECK_FALSE(engine.open(small(), error, true));
    CHECK(error == "GPIO engine already running");

    std::vector<uint8_t> frame(64 * 32 * 3, 255);   // all white, so the blank after it is unmistakable
    engine.publishRgb(frame.data());
    REQUIRE(waitForWord(1024, 11, 0));
    engine.shutdown();
    engine.shutdown();   // a second shutdown is harmless
    CHECK_FALSE(engine.isRunning());
    std::vector<uint32_t> last(2048);
    REQUIRE(mm::platform::gpioStubCopyLastFrame(last.data(), last.size()) == 2048);
    for (uint32_t w : last) CHECK(w == 0u);   // every colour bit clear

    // The engine opens again after a shutdown, and the second open runs
    // the full dark-first setup again (NOTE.md rule 3: release and
    // re-acquire without restarting the process).
    REQUIRE(engine.open(small(), error, true));
    CHECK(mm::platform::gpioStubSetupLog() ==
          "dark-values,directions,pwm-config,oe-mux,dark-values,directions,pwm-config,oe-mux");
    engine.shutdown();
}
