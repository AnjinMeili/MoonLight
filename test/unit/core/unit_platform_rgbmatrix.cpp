/// @module platform

/// Pins the RGB seam rpi-rgb-led-matrix sits behind. Recording mode stands in for the panels, as NDI's does for its runtime, so what a driver hands over is checked on every host; the bench Pi then only has to show it.

#include "doctest.h"
#include "platform/platform.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include "unit/light/hub75_bench.h"

namespace {

// The PIO check's reason on a Pi 5 running `release`, or empty when PIO serves there.
std::string pioOn(const char* release) {
    PioHost host(release, true);
    const char* why = mm::platform::rgbMatrixPioRefusal();
    return why ? why : "";
}

// The bench wall: 128x64 panels, two chained on each of three outputs.
mm::platform::RgbMatrixOptions benchWall() {
    mm::platform::RgbMatrixOptions o;
    o.rows = 64;
    o.cols = 128;
    o.chain = 2;
    o.parallel = 3;
    o.mapping = "regular";
    return o;
}

}  // namespace

TEST_CASE("RgbMatrix: the canvas is the chain across and the parallel outputs down") {
    RecordGuard guard;
    REQUIRE(mm::platform::rgbMatrixOpen(benchWall()));
    CHECK(mm::platform::rgbMatrixWidth() == 256);
    CHECK(mm::platform::rgbMatrixHeight() == 192);
    CHECK(mm::platform::rgbMatrixLastError() == nullptr);
}

TEST_CASE("RgbMatrix: an open keeps the options, the strings outliving the caller's") {
    RecordGuard guard;
    PioHost pi5("6.18.60+rpt-rpi-2712", true);   // a Pi 5 PIO serves on
    std::string mapping = "adafruit-hat-pwm";
    mm::platform::RgbMatrixOptions o = benchWall();
    o.mapping = mapping.c_str();
    o.panelType = "FM6126A";
    o.rp1Pio = true;
    o.pwmBits = 8;
    REQUIRE(mm::platform::rgbMatrixOpen(o));
    mapping = "overwritten";
    const auto& kept = mm::platform::rgbMatrixTestOptions();
    CHECK(std::string(kept.mapping) == "adafruit-hat-pwm");
    CHECK(std::string(kept.panelType) == "FM6126A");
    CHECK(kept.rp1Pio);
    CHECK(kept.pwmBits == 8);
}

TEST_CASE("RgbMatrix: a pushed frame arrives byte for byte") {
    RecordGuard guard;
    REQUIRE(mm::platform::rgbMatrixOpen(benchWall()));
    std::vector<uint8_t> frame(256 * 192 * 3);
    for (size_t i = 0; i < frame.size(); i++) frame[i] = static_cast<uint8_t>(i * 7);
    mm::platform::rgbMatrixPush(frame.data());
    REQUIRE(mm::platform::rgbMatrixTestFrameCount() == 1);
    CHECK(std::memcmp(mm::platform::rgbMatrixTestFrame(), frame.data(), frame.size()) == 0);
}

TEST_CASE("RgbMatrix: the correction is handed over as given, brightness 0 included, the CIE table on or off") {
    RecordGuard guard;
    REQUIRE(mm::platform::rgbMatrixOpen(benchWall()));
    mm::platform::rgbMatrixCorrection(0, true);
    CHECK(mm::platform::rgbMatrixTestBrightness() == 0);
    CHECK(mm::platform::rgbMatrixTestCie1931());
    mm::platform::rgbMatrixCorrection(200, false);
    CHECK(mm::platform::rgbMatrixTestBrightness() == 200);
    CHECK_FALSE(mm::platform::rgbMatrixTestCie1931());
}

TEST_CASE("RgbMatrix: PIO is refused on a kernel from 6.18.39 up to 6.18.60, which stalls its reopen") {
    for (const char* release : {"6.18.39", "6.18.50+rpt-rpi-2712", "6.18.59"}) {
        CAPTURE(release);
        const std::string why = pioOn(release);
        CHECK(why.find("stalls a PIO reopen") != std::string::npos);
        CHECK(why.find("RIO only") != std::string::npos);
    }
}

TEST_CASE("RgbMatrix: PIO serves on 6.18.60 and later, and on a kernel before 6.18.39") {
    for (const char* release : {"6.18.60+rpt-rpi-2712", "6.19.2", "6.18.38", "6.12.47+rpt-rpi-2712", "7.0.1"}) {
        CAPTURE(release);
        CHECK(pioOn(release).empty());
    }
}

TEST_CASE("RgbMatrix: PIO is refused without /dev/pio0, whatever the kernel") {
    // A Pi 3 or 4 has no RP1, and a kernel that lacks its PIO driver makes no device.
    PioHost host("6.18.60+rpt-rpi-2712", false);
    REQUIRE(mm::platform::rgbMatrixPioRefusal() != nullptr);
    CHECK(std::string(mm::platform::rgbMatrixPioRefusal()) == "no /dev/pio0 on this system");
}

TEST_CASE("RgbMatrix: an open asking for PIO where the PIO check refuses it opens on RIO and warns why") {
    RecordGuard guard;
    PioHost stalls("6.18.50+rpt-rpi-2712", true);
    mm::platform::RgbMatrixOptions o = benchWall();
    o.rp1Pio = true;
    REQUIRE(mm::platform::rgbMatrixOpen(o));
    CHECK_FALSE(mm::platform::rgbMatrixTestOptions().rp1Pio);
    REQUIRE(mm::platform::rgbMatrixWarning() != nullptr);
    CHECK(std::string(mm::platform::rgbMatrixWarning()).find("RIO only") != std::string::npos);
}

TEST_CASE("RgbMatrix: a warning lasts as long as the panels it describes: a close and a refused open leave none") {
    RecordGuard guard;
    PioHost stalls("6.18.50+rpt-rpi-2712", true);
    mm::platform::RgbMatrixOptions o = benchWall();
    o.rp1Pio = true;
    REQUIRE(mm::platform::rgbMatrixOpen(o));
    REQUIRE(mm::platform::rgbMatrixWarning() != nullptr);
    mm::platform::rgbMatrixClose();
    CHECK(mm::platform::rgbMatrixWarning() == nullptr);

    o.rows = 0;   // no canvas: the open is refused after the PIO check has warned
    REQUIRE_FALSE(mm::platform::rgbMatrixOpen(o));
    CHECK(mm::platform::rgbMatrixWarning() == nullptr);
}

TEST_CASE("RgbMatrix: an open on RIO carries no PIO warning, whatever the kernel") {
    RecordGuard guard;
    PioHost stalls("6.18.50+rpt-rpi-2712", true);
    REQUIRE(mm::platform::rgbMatrixOpen(benchWall()));
    CHECK(mm::platform::rgbMatrixWarning() == nullptr);
}

TEST_CASE("RgbMatrix: an open without real-time priority warns, naming the capability and the reserved core") {
    RecordGuard guard;
    Realtime none(false);
    REQUIRE(mm::platform::rgbMatrixOpen(benchWall()));
    REQUIRE(mm::platform::rgbMatrixWarning() != nullptr);
    CHECK(std::string(mm::platform::rgbMatrixWarning()) ==
          "no real-time priority, so colors may flicker: grant CAP_SYS_NICE and reserve a core with isolcpus");
}

TEST_CASE("RgbMatrix: a refused PIO and a missing real-time priority both warn, the PIO first, neither hiding the other") {
    RecordGuard guard;
    PioHost stalls("6.18.50+rpt-rpi-2712", true);
    Realtime none(false);
    mm::platform::RgbMatrixOptions o = benchWall();
    o.rp1Pio = true;
    REQUIRE(mm::platform::rgbMatrixOpen(o));
    REQUIRE(mm::platform::rgbMatrixWarning() != nullptr);
    CHECK(std::string(mm::platform::rgbMatrixWarning()) ==
          "kernel 6.18.50 stalls a PIO reopen: RIO only; "
          "no real-time priority, so colors may flicker: grant CAP_SYS_NICE and reserve a core with isolcpus");
}

TEST_CASE("RgbMatrix: an open with real-time priority and no PIO asked carries no warning") {
    RecordGuard guard;
    Realtime granted(true);
    REQUIRE(mm::platform::rgbMatrixOpen(benchWall()));
    CHECK(mm::platform::rgbMatrixWarning() == nullptr);
}

TEST_CASE("RgbMatrix: a kernel release that does not read as a version leaves PIO to the device") {
    CHECK(pioOn("").empty());
    CHECK(pioOn("rpi-custom").empty());
}

TEST_CASE("RgbMatrix: after close there is no canvas, and a push is ignored") {
    RecordGuard guard;
    REQUIRE(mm::platform::rgbMatrixOpen(benchWall()));
    mm::platform::rgbMatrixClose();
    CHECK(mm::platform::rgbMatrixWidth() == 0);
    CHECK(mm::platform::rgbMatrixHeight() == 0);
    const uint8_t pixel[3] = {1, 2, 3};
    mm::platform::rgbMatrixPush(pixel);
    CHECK(mm::platform::rgbMatrixTestFrameCount() == 0);
    CHECK(mm::platform::rgbMatrixFps() == 0);
}

TEST_CASE("RgbMatrix: a null frame is ignored") {
    RecordGuard guard;
    REQUIRE(mm::platform::rgbMatrixOpen(benchWall()));
    mm::platform::rgbMatrixPush(nullptr);
    CHECK(mm::platform::rgbMatrixTestFrameCount() == 0);
}

TEST_CASE("RgbMatrix: an open that cannot drive panels fails with a reason and leaves no canvas") {
    // Zero rows: a build that lacks the library refuses any open with its reason.
    mm::platform::setTestRgbMatrix(false);
    mm::platform::RgbMatrixOptions o = benchWall();
    o.rows = 0;
    CHECK_FALSE(mm::platform::rgbMatrixOpen(o));
    REQUIRE(mm::platform::rgbMatrixLastError() != nullptr);
    CHECK(std::strlen(mm::platform::rgbMatrixLastError()) > 0);
    CHECK(mm::platform::rgbMatrixWidth() == 0);
}

TEST_CASE("RgbMatrix: a recorded open with no panels fails, leaving no canvas in either direction") {
    RecordGuard guard;
    for (bool noRows : {false, true}) {
        CAPTURE(noRows);
        mm::platform::RgbMatrixOptions o = benchWall();
        if (noRows) o.rows = 0;   // the width alone still reads as panels across
        else o.chain = 0;
        CHECK_FALSE(mm::platform::rgbMatrixOpen(o));
        CHECK(mm::platform::rgbMatrixLastError() != nullptr);
        CHECK(mm::platform::rgbMatrixWidth() == 0);
        CHECK(mm::platform::rgbMatrixHeight() == 0);
    }
}

TEST_CASE("RgbMatrix: the newest of several pushes is the one shown, whole, until the next") {
    RecordGuard guard;
    REQUIRE(mm::platform::rgbMatrixOpen(benchWall()));
    std::vector<uint8_t> frame(256 * 192 * 3);
    for (uint8_t v : {1, 2, 3}) {   // three frames faster than the panels take them
        std::fill(frame.begin(), frame.end(), v);
        mm::platform::rgbMatrixPush(frame.data());
    }
    const uint8_t* shown = mm::platform::rgbMatrixTestFrame();
    REQUIRE(shown != nullptr);
    CHECK(std::count(shown, shown + frame.size(), uint8_t{3}) == static_cast<long>(frame.size()));
    CHECK(mm::platform::rgbMatrixTestFrame() == shown);   // no new push, so the same frame stays
}

TEST_CASE("RgbMatrix: the seam says how many parallel chains a mapping drives, and a test can set the answer") {
    mm::platform::setTestRgbMatrix(true);
    CHECK(mm::platform::rgbMatrixMaxChains("regular") == 3);   // the stand-in's default, the Pi header's
    mm::platform::setTestRgbMatrixMaxChains(1);
    CHECK(mm::platform::rgbMatrixMaxChains("adafruit-hat") == 1);
    mm::platform::setTestRgbMatrix(false);
    mm::platform::setTestRgbMatrix(true);   // opening a recording again puts the default back
    CHECK(mm::platform::rgbMatrixMaxChains("regular") == 3);
    mm::platform::setTestRgbMatrix(false);
}
