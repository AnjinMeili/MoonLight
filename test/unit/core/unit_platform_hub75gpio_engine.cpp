/// @module platform

/// Pins the GPIO engine against the stub backend: the options it refuses, the PWM timing it derives, the GPIO words its refresh thread submits for a known frame, the dark-first setup order, and the blank frame its shutdown leaves. No hardware is touched; the stub captures every submit and logs every setup step.

#include "doctest.h"
#include "platform/platform.h"
#include "platform/rpi/hub75_gpio_backend.h"
#include "platform/rpi/hub75_gpio_backend_stub.h"
#include "platform/rpi/hub75_gpio_engine.h"
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
