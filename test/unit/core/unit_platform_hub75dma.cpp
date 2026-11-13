/// @module platform

/// Pins the DMA seam the Pi 3/4 GPIO+PWM engine sits behind. Recording mode stands in for the panels, as the PIO seam's does, so what a driver hands over is checked on every host; the bench Pi then only has to show it. The refusal path branches on hub75DmaAvailable(): a Pi 3/4 build takes the engine path, any other host takes the not-a-Pi refusal.

#include "doctest.h"
#include "platform/platform.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include "unit/light/hub75_bench.h"

namespace {

// The bench wall: 128x64 panels, two chained on each of three outputs.
mm::platform::Hub75DmaOptions benchWall() {
    mm::platform::Hub75DmaOptions o;
    o.rows = 64;
    o.cols = 128;
    o.chain = 2;
    o.parallel = 3;
    return o;
}

}  // namespace

TEST_CASE("Hub75Dma: the canvas is the chain across and the parallel outputs down") {
    DmaRecordGuard guard;
    REQUIRE(mm::platform::hub75DmaOpen(benchWall()));
    CHECK(mm::platform::hub75DmaWidth() == 256);
    CHECK(mm::platform::hub75DmaHeight() == 192);
    CHECK(mm::platform::hub75DmaLastError() == nullptr);
    CHECK(mm::platform::hub75DmaWarning() == nullptr);
}

TEST_CASE("Hub75Dma: an open keeps the options") {
    DmaRecordGuard guard;
    mm::platform::Hub75DmaOptions o;
    o.rows = 32;
    o.cols = 64;
    o.chain = 1;
    o.parallel = 1;
    o.scanRate = 16;
    o.bitDepth = 2;
    o.pwmLsbNs = 50;
    o.slowdown = 2;
    o.clockFallingEdge = true;
    REQUIRE(mm::platform::hub75DmaOpen(o));
    const auto& kept = mm::platform::hub75DmaTestOptions();
    CHECK(kept.rows == 32);
    CHECK(kept.cols == 64);
    CHECK(kept.chain == 1);
    CHECK(kept.parallel == 1);
    CHECK(kept.scanRate == 16);
    CHECK(kept.bitDepth == 2);
    CHECK(kept.pwmLsbNs == 50);
    CHECK(kept.slowdown == 2);
    CHECK(kept.clockFallingEdge);
    CHECK(mm::platform::hub75DmaWidth() == 64);
    CHECK(mm::platform::hub75DmaHeight() == 32);
}

TEST_CASE("Hub75Dma: a pushed frame arrives byte for byte") {
    DmaRecordGuard guard;
    REQUIRE(mm::platform::hub75DmaOpen(benchWall()));
    std::vector<uint8_t> frame(256 * 192 * 3);
    for (size_t i = 0; i < frame.size(); i++) frame[i] = static_cast<uint8_t>(i * 7);
    mm::platform::hub75DmaPush(frame.data());
    REQUIRE(mm::platform::hub75DmaTestFrameCount() == 1);
    CHECK(std::memcmp(mm::platform::hub75DmaTestFrame(), frame.data(), frame.size()) == 0);
}

TEST_CASE("Hub75Dma: the correction is handed over as given, brightness 0 included") {
    DmaRecordGuard guard;
    REQUIRE(mm::platform::hub75DmaOpen(benchWall()));
    mm::platform::hub75DmaCorrection(0);
    CHECK(mm::platform::hub75DmaTestBrightness() == 0);
    mm::platform::hub75DmaCorrection(200);
    CHECK(mm::platform::hub75DmaTestBrightness() == 200);
}

TEST_CASE("Hub75Dma: the recorder reports no refresh rate") {
    DmaRecordGuard guard;
    REQUIRE(mm::platform::hub75DmaOpen(benchWall()));
    std::vector<uint8_t> frame(256 * 192 * 3, 0);
    mm::platform::hub75DmaPush(frame.data());
    CHECK(mm::platform::hub75DmaFps() == 0);
}

TEST_CASE("Hub75Dma: without Pi hardware an open outside the recorder fails naming the host") {
    if (mm::platform::hub75DmaAvailable()) return;   // a Pi 3/4 takes the engine path below
    mm::platform::setTestHub75Dma(false);
    CHECK_FALSE(mm::platform::hub75DmaOpen(benchWall()));
    REQUIRE(mm::platform::hub75DmaLastError() != nullptr);
    const std::string why = mm::platform::hub75DmaLastError();
    // This host is not a Pi at all; a Pi without /dev/mem access, or
    // one running the audio driver, refuses with its own reason instead.
    const bool namesHost = why.find("not a Raspberry Pi") != std::string::npos ||
                           why.find("/dev/mem") != std::string::npos ||
                           why.find("snd_bcm2835") != std::string::npos;
    CHECK(namesHost);
    CHECK(mm::platform::hub75DmaWidth() == 0);
}

TEST_CASE("Hub75Dma: a recorded open refuses what the engine would refuse, with its reason") {
    DmaRecordGuard guard;
    // A 32-high panel at 1/32 scan: the encoder needs an even rows-per-scan.
    mm::platform::Hub75DmaOptions o = benchWall();
    o.rows = 32;
    CHECK_FALSE(mm::platform::hub75DmaOpen(o));
    REQUIRE(mm::platform::hub75DmaLastError() != nullptr);
    CHECK(std::string(mm::platform::hub75DmaLastError()).find("32-high at 1/16") != std::string::npos);
    CHECK(mm::platform::hub75DmaWidth() == 0);
    CHECK(mm::platform::hub75DmaHeight() == 0);

    o = benchWall();
    o.chain = 0;
    CHECK_FALSE(mm::platform::hub75DmaOpen(o));
    o = benchWall();
    o.parallel = 0;
    CHECK_FALSE(mm::platform::hub75DmaOpen(o));
    o = benchWall();
    o.bitDepth = 8;
    CHECK_FALSE(mm::platform::hub75DmaOpen(o));
    o = benchWall();
    o.pwmLsbNs = 10;
    CHECK_FALSE(mm::platform::hub75DmaOpen(o));
    o = benchWall();
    o.pwmLsbNs = 3001;
    CHECK_FALSE(mm::platform::hub75DmaOpen(o));
    o = benchWall();
    o.slowdown = 5;
    CHECK_FALSE(mm::platform::hub75DmaOpen(o));
    o = benchWall();
    o.scanRate = 64;
    CHECK_FALSE(mm::platform::hub75DmaOpen(o));
    CHECK(mm::platform::hub75DmaWidth() == 0);
    CHECK(mm::platform::hub75DmaHeight() == 0);
}

TEST_CASE("Hub75Dma: after close there is no canvas, and a push is ignored") {
    DmaRecordGuard guard;
    REQUIRE(mm::platform::hub75DmaOpen(benchWall()));
    mm::platform::hub75DmaClose();
    CHECK(mm::platform::hub75DmaWidth() == 0);
    CHECK(mm::platform::hub75DmaHeight() == 0);
    const uint8_t pixel[3] = {1, 2, 3};
    mm::platform::hub75DmaPush(pixel);
    CHECK(mm::platform::hub75DmaTestFrameCount() == 0);
    CHECK(mm::platform::hub75DmaFps() == 0);
}

TEST_CASE("Hub75Dma: a null frame is ignored") {
    DmaRecordGuard guard;
    REQUIRE(mm::platform::hub75DmaOpen(benchWall()));
    mm::platform::hub75DmaPush(nullptr);
    CHECK(mm::platform::hub75DmaTestFrameCount() == 0);
}

TEST_CASE("Hub75Dma: the newest of several pushes is the one shown, whole, until the next") {
    DmaRecordGuard guard;
    REQUIRE(mm::platform::hub75DmaOpen(benchWall()));
    std::vector<uint8_t> frame(256 * 192 * 3);
    for (uint8_t v : {1, 2, 3}) {   // three frames faster than the panels take them
        std::fill(frame.begin(), frame.end(), v);
        mm::platform::hub75DmaPush(frame.data());
    }
    const uint8_t* shown = mm::platform::hub75DmaTestFrame();
    REQUIRE(shown != nullptr);
    CHECK(std::count(shown, shown + frame.size(), uint8_t{3}) == static_cast<long>(frame.size()));
    CHECK(mm::platform::hub75DmaTestFrame() == shown);   // no new push, so the same frame stays
}

TEST_CASE("Hub75Dma: a recorded open, close and reopen works in one process") {
    DmaRecordGuard guard;
    REQUIRE(mm::platform::hub75DmaOpen(benchWall()));
    mm::platform::hub75DmaClose();
    CHECK(mm::platform::hub75DmaWidth() == 0);
    REQUIRE(mm::platform::hub75DmaOpen(benchWall()));   // MoonLight re-opens on every setting change
    CHECK(mm::platform::hub75DmaWidth() == 256);
    CHECK(mm::platform::hub75DmaHeight() == 192);
}
