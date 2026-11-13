/// @module platform

/// Pins the PIO seam the RP1 state machine sits behind. Recording mode stands in for the panels, as the RGB seam's does, so what a driver hands over is checked on every host; the bench Pi then only has to show it. The refusal paths branch on the piolib build: a build with piolib linked refuses through the kernel check, one without through the missing library. Availability is host-gated separately: with piolib, a Pi 5 is offered the backend and a Pi 4 is not.

#include "doctest.h"
#include "platform/platform.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include "unit/light/hub75_bench.h"

namespace {

// The bench wall: 128x64 panels, two chained on each of three outputs.
mm::platform::Hub75PioOptions benchWall() {
    mm::platform::Hub75PioOptions o;
    o.rows = 64;
    o.cols = 128;
    o.chain = 2;
    o.parallel = 3;
    return o;
}

}  // namespace

TEST_CASE("Hub75Pio: the canvas is the chain across and the parallel outputs down") {
    PioRecordGuard guard;
    REQUIRE(mm::platform::hub75PioOpen(benchWall()));
    CHECK(mm::platform::hub75PioWidth() == 256);
    CHECK(mm::platform::hub75PioHeight() == 192);
    CHECK(mm::platform::hub75PioLastError() == nullptr);
    CHECK(mm::platform::hub75PioWarning() == nullptr);
}

TEST_CASE("Hub75Pio: an open keeps the options") {
    PioRecordGuard guard;
    mm::platform::Hub75PioOptions o;
    o.rows = 32;
    o.cols = 64;
    o.chain = 1;
    o.parallel = 1;
    o.scanRate = 16;
    o.bitDepth = 2;
    o.pixelClockHz = 5000000;
    o.latchBlankWords = 3;
    o.clockFallingEdge = true;
    REQUIRE(mm::platform::hub75PioOpen(o));
    const auto& kept = mm::platform::hub75PioTestOptions();
    CHECK(kept.rows == 32);
    CHECK(kept.cols == 64);
    CHECK(kept.chain == 1);
    CHECK(kept.parallel == 1);
    CHECK(kept.scanRate == 16);
    CHECK(kept.bitDepth == 2);
    CHECK(kept.pixelClockHz == 5000000);
    CHECK(kept.latchBlankWords == 3);
    CHECK(kept.clockFallingEdge);
    CHECK(mm::platform::hub75PioWidth() == 64);
    CHECK(mm::platform::hub75PioHeight() == 32);
}

TEST_CASE("Hub75Pio: a pushed frame arrives byte for byte") {
    PioRecordGuard guard;
    REQUIRE(mm::platform::hub75PioOpen(benchWall()));
    std::vector<uint8_t> frame(256 * 192 * 3);
    for (size_t i = 0; i < frame.size(); i++) frame[i] = static_cast<uint8_t>(i * 7);
    mm::platform::hub75PioPush(frame.data());
    REQUIRE(mm::platform::hub75PioTestFrameCount() == 1);
    CHECK(std::memcmp(mm::platform::hub75PioTestFrame(), frame.data(), frame.size()) == 0);
}

TEST_CASE("Hub75Pio: the correction is handed over as given, brightness 0 included") {
    PioRecordGuard guard;
    REQUIRE(mm::platform::hub75PioOpen(benchWall()));
    mm::platform::hub75PioCorrection(0);
    CHECK(mm::platform::hub75PioTestBrightness() == 0);
    mm::platform::hub75PioCorrection(200);
    CHECK(mm::platform::hub75PioTestBrightness() == 200);
}

TEST_CASE("Hub75Pio: the recorder reports no refresh rate") {
    PioRecordGuard guard;
    REQUIRE(mm::platform::hub75PioOpen(benchWall()));
    std::vector<uint8_t> frame(256 * 192 * 3, 0);
    mm::platform::hub75PioPush(frame.data());
    CHECK(mm::platform::hub75PioFps() == 0);
}

TEST_CASE("Hub75Pio: without piolib an open outside the recorder fails naming the library") {
#ifdef MM_HUB75_PIO_HAVE_PIOLIB
    return;   // a piolib build takes the kernel path below
#else
    mm::platform::setTestHub75Pio(false);
    CHECK_FALSE(mm::platform::hub75PioOpen(benchWall()));
    REQUIRE(mm::platform::hub75PioLastError() != nullptr);
    CHECK(std::string(mm::platform::hub75PioLastError()) == "needs a Raspberry Pi build with piolib");
    CHECK(mm::platform::hub75PioWidth() == 0);
#endif
}

TEST_CASE("Hub75Pio: a stalling kernel refuses the open outright, since the PIO output has no RIO fallback") {
#ifndef MM_HUB75_PIO_HAVE_PIOLIB
    return;   // without piolib the library's reason comes first
#else
    mm::platform::setTestHub75Pio(false);
    PioHost stalls("6.18.50+rpt-rpi-2712", true);
    CHECK_FALSE(mm::platform::hub75PioOpen(benchWall()));
    REQUIRE(mm::platform::hub75PioLastError() != nullptr);
    CHECK(std::string(mm::platform::hub75PioLastError()).find("stalls a PIO reopen") != std::string::npos);
    CHECK(mm::platform::hub75PioWidth() == 0);
#endif
}

TEST_CASE("Hub75Pio: without /dev/pio0 the open is refused, whatever the kernel") {
#ifndef MM_HUB75_PIO_HAVE_PIOLIB
    return;   // without piolib the library's reason comes first
#else
    mm::platform::setTestHub75Pio(false);
    PioHost host("6.18.60+rpt-rpi-2712", false);
    CHECK_FALSE(mm::platform::hub75PioOpen(benchWall()));
    REQUIRE(mm::platform::hub75PioLastError() != nullptr);
    CHECK(std::string(mm::platform::hub75PioLastError()) == "no /dev/pio0 on this system");
#endif
}

TEST_CASE("Hub75Pio: the recorder offers the backend on every host") {
    PioRecordGuard guard;
    CHECK(mm::platform::hub75PioAvailable());
}

TEST_CASE("Hub75Pio: the test hook forces availability either way, and the host decides again after it") {
    mm::platform::setTestHub75Pio(false);
    {
        PioAvailable off(0);
        CHECK_FALSE(mm::platform::hub75PioAvailable());
    }
    {
        PioAvailable on(1);
        CHECK(mm::platform::hub75PioAvailable());
    }
#ifndef MM_HUB75_PIO_HAVE_PIOLIB
    CHECK_FALSE(mm::platform::hub75PioAvailable());   // after the guards the host decides again: no piolib, not offered
#endif
}

TEST_CASE("Hub75Pio: with piolib, the host gates availability: a Pi 5 is offered the backend, a Pi 4 and a stall kernel are not") {
#ifndef MM_HUB75_PIO_HAVE_PIOLIB
    return;   // without piolib the host never offers it: the hook test above pins that
#else
    mm::platform::setTestHub75Pio(false);
    {
        PioHost pi5("6.18.60+rpt-rpi-2712", true);   // a Pi 5 PIO serves on
        CHECK(mm::platform::hub75PioAvailable());
    }
    {
        PioHost pi4("6.18.60+rpt-rpi-2712", false);   // the same kernel, no /dev/pio0: a Pi 4
        CHECK_FALSE(mm::platform::hub75PioAvailable());
    }
    {
        PioHost stalls("6.18.50+rpt-rpi-2712", true);   // a stall kernel, device present
        CHECK_FALSE(mm::platform::hub75PioAvailable());
    }
#endif
}

TEST_CASE("Hub75Pio: a recorded open refuses what the engine would refuse, with its reason") {
    PioRecordGuard guard;
    // A 32-high panel at 1/32 scan: the encoder needs an even rows-per-scan.
    mm::platform::Hub75PioOptions o = benchWall();
    o.rows = 32;
    CHECK_FALSE(mm::platform::hub75PioOpen(o));
    REQUIRE(mm::platform::hub75PioLastError() != nullptr);
    CHECK(std::string(mm::platform::hub75PioLastError()).find("32-high at 1/16") != std::string::npos);
    CHECK(mm::platform::hub75PioWidth() == 0);
    CHECK(mm::platform::hub75PioHeight() == 0);

    o = benchWall();
    o.chain = 0;
    CHECK_FALSE(mm::platform::hub75PioOpen(o));
    o = benchWall();
    o.parallel = 0;
    CHECK_FALSE(mm::platform::hub75PioOpen(o));
    o = benchWall();
    o.bitDepth = 8;
    CHECK_FALSE(mm::platform::hub75PioOpen(o));
    o = benchWall();
    o.pixelClockHz = 0;
    CHECK_FALSE(mm::platform::hub75PioOpen(o));
    o = benchWall();
    o.scanRate = 64;
    CHECK_FALSE(mm::platform::hub75PioOpen(o));
    CHECK(mm::platform::hub75PioWidth() == 0);
    CHECK(mm::platform::hub75PioHeight() == 0);
}

TEST_CASE("Hub75Pio: after close there is no canvas, and a push is ignored") {
    PioRecordGuard guard;
    REQUIRE(mm::platform::hub75PioOpen(benchWall()));
    mm::platform::hub75PioClose();
    CHECK(mm::platform::hub75PioWidth() == 0);
    CHECK(mm::platform::hub75PioHeight() == 0);
    const uint8_t pixel[3] = {1, 2, 3};
    mm::platform::hub75PioPush(pixel);
    CHECK(mm::platform::hub75PioTestFrameCount() == 0);
    CHECK(mm::platform::hub75PioFps() == 0);
}

TEST_CASE("Hub75Pio: a null frame is ignored") {
    PioRecordGuard guard;
    REQUIRE(mm::platform::hub75PioOpen(benchWall()));
    mm::platform::hub75PioPush(nullptr);
    CHECK(mm::platform::hub75PioTestFrameCount() == 0);
}

TEST_CASE("Hub75Pio: the newest of several pushes is the one shown, whole, until the next") {
    PioRecordGuard guard;
    REQUIRE(mm::platform::hub75PioOpen(benchWall()));
    std::vector<uint8_t> frame(256 * 192 * 3);
    for (uint8_t v : {1, 2, 3}) {   // three frames faster than the panels take them
        std::fill(frame.begin(), frame.end(), v);
        mm::platform::hub75PioPush(frame.data());
    }
    const uint8_t* shown = mm::platform::hub75PioTestFrame();
    REQUIRE(shown != nullptr);
    CHECK(std::count(shown, shown + frame.size(), uint8_t{3}) == static_cast<long>(frame.size()));
    CHECK(mm::platform::hub75PioTestFrame() == shown);   // no new push, so the same frame stays
}
