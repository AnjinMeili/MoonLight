/// @module platform

/// Pins the GPIO seam the Pi 3/4 GPIO+PWM engine sits behind. Recording mode stands in for the panels, as the PIO seam's does, so what a driver hands over is checked on every host; the bench Pi then only has to show it. The refusal path branches on hub75GpioAvailable(): a Pi 3/4 build takes the engine path, any other host takes the not-a-Pi refusal.

#include "doctest.h"
#include "platform/platform.h"
#include "platform/rpi/hub75_gpio_backend.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include "unit/light/hub75_bench.h"

namespace {

// The bench wall: 128x64 panels, two chained on each of three outputs.
mm::platform::Hub75GpioOptions benchWall() {
    mm::platform::Hub75GpioOptions o;
    o.rows = 64;
    o.cols = 128;
    o.chain = 2;
    o.parallel = 3;
    return o;
}

}  // namespace

TEST_CASE("Hub75Gpio: the canvas is the chain across and the parallel outputs down") {
    GpioRecordGuard guard;
    REQUIRE(mm::platform::hub75GpioOpen(benchWall()));
    CHECK(mm::platform::hub75GpioWidth() == 256);
    CHECK(mm::platform::hub75GpioHeight() == 192);
    CHECK(mm::platform::hub75GpioLastError() == nullptr);
    CHECK(mm::platform::hub75GpioWarning() == nullptr);
}

TEST_CASE("Hub75Gpio: an open keeps the options") {
    GpioRecordGuard guard;
    mm::platform::Hub75GpioOptions o;
    o.rows = 32;
    o.cols = 64;
    o.chain = 1;
    o.parallel = 1;
    o.scanRate = 16;
    o.bitDepth = 2;
    o.pwmLsbNs = 50;
    o.slowdown = 2;
    o.clockFallingEdge = true;
    REQUIRE(mm::platform::hub75GpioOpen(o));
    const auto& kept = mm::platform::hub75GpioTestOptions();
    CHECK(kept.rows == 32);
    CHECK(kept.cols == 64);
    CHECK(kept.chain == 1);
    CHECK(kept.parallel == 1);
    CHECK(kept.scanRate == 16);
    CHECK(kept.bitDepth == 2);
    CHECK(kept.pwmLsbNs == 50);
    CHECK(kept.slowdown == 2);
    CHECK(kept.clockFallingEdge);
    CHECK(mm::platform::hub75GpioWidth() == 64);
    CHECK(mm::platform::hub75GpioHeight() == 32);
}

TEST_CASE("Hub75Gpio: a pushed frame arrives byte for byte") {
    GpioRecordGuard guard;
    REQUIRE(mm::platform::hub75GpioOpen(benchWall()));
    std::vector<uint8_t> frame(256 * 192 * 3);
    for (size_t i = 0; i < frame.size(); i++) frame[i] = static_cast<uint8_t>(i * 7);
    mm::platform::hub75GpioPush(frame.data());
    REQUIRE(mm::platform::hub75GpioTestFrameCount() == 1);
    CHECK(std::memcmp(mm::platform::hub75GpioTestFrame(), frame.data(), frame.size()) == 0);
}

TEST_CASE("Hub75Gpio: the correction is handed over as given, brightness 0 included") {
    GpioRecordGuard guard;
    REQUIRE(mm::platform::hub75GpioOpen(benchWall()));
    mm::platform::hub75GpioCorrection(0);
    CHECK(mm::platform::hub75GpioTestBrightness() == 0);
    mm::platform::hub75GpioCorrection(200);
    CHECK(mm::platform::hub75GpioTestBrightness() == 200);
}

TEST_CASE("Hub75Gpio: the recorder reports no refresh rate") {
    GpioRecordGuard guard;
    REQUIRE(mm::platform::hub75GpioOpen(benchWall()));
    std::vector<uint8_t> frame(256 * 192 * 3, 0);
    mm::platform::hub75GpioPush(frame.data());
    CHECK(mm::platform::hub75GpioFps() == 0);
}

TEST_CASE("Hub75Gpio: without Pi hardware an open outside the recorder fails naming the host") {
    if (mm::platform::hub75GpioAvailable()) return;   // a Pi 3/4 takes the engine path below
    mm::platform::setTestHub75Gpio(false);
    CHECK_FALSE(mm::platform::hub75GpioOpen(benchWall()));
    REQUIRE(mm::platform::hub75GpioLastError() != nullptr);
    const std::string why = mm::platform::hub75GpioLastError();
    // This host is not a Pi at all; a Pi without /dev/mem access, or
    // one running the audio driver, refuses with its own reason instead.
    // A Pi 5 refuses by name too: its header belongs to the PIO output.
    const bool namesHost = why.find("not a Raspberry Pi") != std::string::npos ||
                           why.find("Raspberry Pi 5") != std::string::npos ||
                           why.find("/dev/mem") != std::string::npos ||
                           why.find("snd_bcm2835") != std::string::npos;
    CHECK(namesHost);
    CHECK(mm::platform::hub75GpioWidth() == 0);
}

TEST_CASE("Hub75Gpio: the seam resolves auto slowdown for this host, the same answer the engine runs") {
    // Auto is the per-model default: 2 on a Pi 4, 1 everywhere else; a chosen slowdown passes through.
    const int expected = mm::platform::hub75GpioHostModel() == mm::platform::Hub75GpioModel::Pi4 ? 2 : 1;
    CHECK(mm::platform::hub75GpioResolvedSlowdown(-1) == expected);
    CHECK(mm::platform::hub75GpioResolvedSlowdown(3) == 3);
    CHECK(mm::platform::hub75GpioResolvedSlowdown(0) == 0);
}

TEST_CASE("Hub75Gpio: a recorded open refuses what the engine would refuse, with its reason") {
    GpioRecordGuard guard;
    // A 32-high panel at 1/32 scan: the encoder needs an even rows-per-scan.
    mm::platform::Hub75GpioOptions o = benchWall();
    o.rows = 32;
    CHECK_FALSE(mm::platform::hub75GpioOpen(o));
    REQUIRE(mm::platform::hub75GpioLastError() != nullptr);
    CHECK(std::string(mm::platform::hub75GpioLastError()).find("32-high at 1/16") != std::string::npos);
    CHECK(mm::platform::hub75GpioWidth() == 0);
    CHECK(mm::platform::hub75GpioHeight() == 0);

    o = benchWall();
    o.chain = 0;
    CHECK_FALSE(mm::platform::hub75GpioOpen(o));
    o = benchWall();
    o.parallel = 0;
    CHECK_FALSE(mm::platform::hub75GpioOpen(o));
    o = benchWall();
    o.bitDepth = 8;
    CHECK_FALSE(mm::platform::hub75GpioOpen(o));
    o = benchWall();
    o.pwmLsbNs = 10;
    CHECK_FALSE(mm::platform::hub75GpioOpen(o));
    o = benchWall();
    o.pwmLsbNs = 3001;
    CHECK_FALSE(mm::platform::hub75GpioOpen(o));
    o = benchWall();
    o.slowdown = 5;
    CHECK_FALSE(mm::platform::hub75GpioOpen(o));
    o = benchWall();
    o.scanRate = 64;
    CHECK_FALSE(mm::platform::hub75GpioOpen(o));
    CHECK(mm::platform::hub75GpioWidth() == 0);
    CHECK(mm::platform::hub75GpioHeight() == 0);
}

TEST_CASE("Hub75Gpio: after close there is no canvas, and a push is ignored") {
    GpioRecordGuard guard;
    REQUIRE(mm::platform::hub75GpioOpen(benchWall()));
    mm::platform::hub75GpioClose();
    CHECK(mm::platform::hub75GpioWidth() == 0);
    CHECK(mm::platform::hub75GpioHeight() == 0);
    const uint8_t pixel[3] = {1, 2, 3};
    mm::platform::hub75GpioPush(pixel);
    CHECK(mm::platform::hub75GpioTestFrameCount() == 0);
    CHECK(mm::platform::hub75GpioFps() == 0);
}

TEST_CASE("Hub75Gpio: a null frame is ignored") {
    GpioRecordGuard guard;
    REQUIRE(mm::platform::hub75GpioOpen(benchWall()));
    mm::platform::hub75GpioPush(nullptr);
    CHECK(mm::platform::hub75GpioTestFrameCount() == 0);
}

TEST_CASE("Hub75Gpio: the newest of several pushes is the one shown, whole, until the next") {
    GpioRecordGuard guard;
    REQUIRE(mm::platform::hub75GpioOpen(benchWall()));
    std::vector<uint8_t> frame(256 * 192 * 3);
    for (uint8_t v : {1, 2, 3}) {   // three frames faster than the panels take them
        std::fill(frame.begin(), frame.end(), v);
        mm::platform::hub75GpioPush(frame.data());
    }
    const uint8_t* shown = mm::platform::hub75GpioTestFrame();
    REQUIRE(shown != nullptr);
    CHECK(std::count(shown, shown + frame.size(), uint8_t{3}) == static_cast<long>(frame.size()));
    CHECK(mm::platform::hub75GpioTestFrame() == shown);   // no new push, so the same frame stays
}

TEST_CASE("Hub75Gpio: a recorded open, close and reopen works in one process") {
    GpioRecordGuard guard;
    REQUIRE(mm::platform::hub75GpioOpen(benchWall()));
    mm::platform::hub75GpioClose();
    CHECK(mm::platform::hub75GpioWidth() == 0);
    REQUIRE(mm::platform::hub75GpioOpen(benchWall()));   // MoonLight re-opens on every setting change
    CHECK(mm::platform::hub75GpioWidth() == 256);
    CHECK(mm::platform::hub75GpioHeight() == 192);
}
