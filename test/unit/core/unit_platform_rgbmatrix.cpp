/// @module platform

/// Pins the RGB seam rpi-rgb-led-matrix sits behind. Recording mode stands in for the panels, as NDI's does for its runtime, so what a driver hands over is checked on every host; the bench Pi then only has to show it.

#include "doctest.h"
#include "platform/platform.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

// The seam is process-global, so a guard leaves it closed and out of recording however a case ends.
struct RecordGuard {
    RecordGuard() { mm::platform::setTestRgbMatrix(true); }
    ~RecordGuard() { mm::platform::setTestRgbMatrix(false); }
};

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
    std::string mapping = "adafruit-hat-pwm";
    mm::platform::RgbMatrixOptions o = benchWall();
    o.mapping = mapping.c_str();
    o.panelType = "FM6126A";
    o.pixelMapper = "Rotate:90";
    o.rp1Pio = true;
    o.pwmBits = 8;
    REQUIRE(mm::platform::rgbMatrixOpen(o));
    mapping = "overwritten";
    const auto& kept = mm::platform::rgbMatrixTestOptions();
    CHECK(std::string(kept.mapping) == "adafruit-hat-pwm");
    CHECK(std::string(kept.panelType) == "FM6126A");
    CHECK(std::string(kept.pixelMapper) == "Rotate:90");
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

TEST_CASE("RgbMatrix: brightness is handed over as given, 0 included") {
    RecordGuard guard;
    REQUIRE(mm::platform::rgbMatrixOpen(benchWall()));
    mm::platform::rgbMatrixBrightness(0);
    CHECK(mm::platform::rgbMatrixTestBrightness() == 0);
    mm::platform::rgbMatrixBrightness(200);
    CHECK(mm::platform::rgbMatrixTestBrightness() == 200);
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
    // Zero rows: the library's Validate refuses it on a Pi, and every other host has no library to try.
    mm::platform::setTestRgbMatrix(false);
    mm::platform::RgbMatrixOptions o = benchWall();
    o.rows = 0;
    CHECK_FALSE(mm::platform::rgbMatrixOpen(o));
    REQUIRE(mm::platform::rgbMatrixLastError() != nullptr);
    CHECK(std::strlen(mm::platform::rgbMatrixLastError()) > 0);
    CHECK(mm::platform::rgbMatrixWidth() == 0);
}

TEST_CASE("RgbMatrix: a recorded open with no panels fails rather than forming an empty canvas") {
    RecordGuard guard;
    mm::platform::RgbMatrixOptions o = benchWall();
    o.chain = 0;
    CHECK_FALSE(mm::platform::rgbMatrixOpen(o));
    CHECK(mm::platform::rgbMatrixLastError() != nullptr);
}

TEST_CASE("RgbMatrix: more parallel chains than the mapping drives fails with a reason, not an abort") {
    // The library aborts the process on this; on a Pi the seam refuses it first, and every other host has no library to try.
    mm::platform::setTestRgbMatrix(false);
    mm::platform::RgbMatrixOptions o = benchWall();
    o.mapping = "adafruit-hat";   // one chain
    CHECK_FALSE(mm::platform::rgbMatrixOpen(o));
    REQUIRE(mm::platform::rgbMatrixLastError() != nullptr);
    const std::string why = mm::platform::rgbMatrixLastError();
    if (why.find("needs a Raspberry Pi build") == std::string::npos) CHECK(why.find("parallel chain") != std::string::npos);
    CHECK(mm::platform::rgbMatrixWidth() == 0);
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
    CHECK(mm::platform::rgbMatrixTestFrame() == shown);   // nothing new pushed, so the same frame stays
}
