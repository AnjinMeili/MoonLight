/// @module Hub75PiDriver
/// @also Drivers, Correction

/// The Pi's HUB75 driver, pinned without panels. The RGB seam records what the driver opened and handed over (the `rgbMatrix*` seam in platform.h), as NDI's does for its runtime; the bench Pi then only has to show it.

#include "doctest.h"
#include "light/drivers/Hub75PiDriver.h"
#include "light/drivers/Correction.h"
#include "light/drivers/Drivers.h"
#include "correction_presets.h"
#include "../core/conditional_controls.h"
#include "light/layers/Buffer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"

#include <cstring>
#include <memory>
#include <string>

namespace {

// The seam is process-global, so a guard leaves it closed and out of recording however a case ends.
struct RecordGuard {
    RecordGuard() { mm::platform::setTestRgbMatrix(true); }
    ~RecordGuard() { mm::platform::setTestRgbMatrix(false); }
};

// A driver on a layout of `width` x `height` holding `lights` lights, wired as production wires it, on the heap since a driver and a layer outgrow a test's stack frame.
struct Rig {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;
    mm::Buffer source;
    mm::Hub75PiDriver driver;

    Rig(uint16_t width, uint16_t height, mm::nrOfLightsType lights) {
        grid.width = width;
        grid.height = height;
        grid.depth = 1;
        layouts.addChild(&grid);
        layouts.applyState();
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.applyState();
        REQUIRE(source.allocate(lights, 3));
        driver.rows = 8;   // panels forming a 16x8 canvas, unless a case changes them
        driver.cols = 16;
        driver.setLayer(&layer);
        driver.setSourceBuffer(&source);
    }

    // Open the panels with a linear, full-brightness correction, as the cases that read values expect.
    void open() {
        mm::Correction correction;
        mm::test::rebuildFromPreset(correction, 255, mm::test::PresetOrder::RGB);
        driver.correctionForTest() = correction;
        driver.applyState();
        driver.prepare();
    }

    void paint(mm::nrOfLightsType i, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t* p = source.data() + static_cast<size_t>(i) * 3;
        p[0] = r; p[1] = g; p[2] = b;
    }
};

std::unique_ptr<Rig> rig(uint16_t width, uint16_t height, mm::nrOfLightsType lights = 0) {
    return std::make_unique<Rig>(width, height, lights ? lights : static_cast<mm::nrOfLightsType>(width) * height);
}

const uint8_t* pixel(uint16_t x, uint16_t y) {
    return mm::platform::rgbMatrixTestFrame() + (static_cast<size_t>(y) * mm::platform::rgbMatrixWidth() + x) * 3;
}

}  // namespace

TEST_CASE("Hub75PiDriver opens the panels with its controls under the library's names") {
    RecordGuard guard;
    auto r = rig(64, 96);
    r->driver.mappingSel = 2;        // adafruit-hat-pwm
    r->driver.panelTypeSel = 1;      // FM6126A
    std::strcpy(r->driver.pixelMapper, "Rotate:90");
    r->driver.rows = 32;
    r->driver.cols = 64;
    r->driver.parallel = 3;
    r->driver.multiplexingSel = 1;   // Stripe
    r->driver.slowdownSel = 3;       // 2
    r->driver.rp1Pio = true;
    r->open();

    const auto& o = mm::platform::rgbMatrixTestOptions();
    CHECK(std::string(o.mapping) == "adafruit-hat-pwm");
    CHECK(std::string(o.panelType) == "FM6126A");
    CHECK(std::string(o.pixelMapper) == "Rotate:90");
    CHECK(o.rows == 32);
    CHECK(o.cols == 64);
    CHECK(o.parallel == 3);
    CHECK(o.multiplexing == 1);
    CHECK(o.slowdown == 2);
    CHECK(o.rp1Pio);
}

TEST_CASE("Hub75PiDriver leaves the GPIO slowdown to the library's model default on auto") {
    RecordGuard guard;
    auto r = rig(16, 8);
    r->open();
    CHECK(mm::platform::rgbMatrixTestOptions().slowdown == -1);
    CHECK(std::string(mm::platform::rgbMatrixTestOptions().panelType).empty());
}

TEST_CASE("Hub75PiDriver hands each light to the panels at its place on the canvas") {
    RecordGuard guard;
    auto r = rig(16, 8);
    r->open();
    CHECK(std::string(r->driver.status()) == "16x8, 11-bit");

    r->paint(0, 10, 20, 30);
    r->paint(3 * 16 + 5, 40, 50, 60);   // (5, 3)
    r->driver.tick();

    REQUIRE(mm::platform::rgbMatrixTestFrameCount() == 1);
    CHECK(pixel(0, 0)[0] == 10); CHECK(pixel(0, 0)[1] == 20); CHECK(pixel(0, 0)[2] == 30);
    CHECK(pixel(5, 3)[0] == 40); CHECK(pixel(5, 3)[1] == 50); CHECK(pixel(5, 3)[2] == 60);
}

TEST_CASE("Hub75PiDriver fills the overlap of a smaller layout, leaves the rest dark and says so") {
    RecordGuard guard;
    auto r = rig(4, 2);
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Warning);
    CHECK(std::string(r->driver.status()) == "layout 4x2, panels 16x8");

    r->paint(4 + 3, 200, 100, 50);   // (3, 1), the layout's last light
    r->driver.tick();

    REQUIRE(mm::platform::rgbMatrixTestFrameCount() == 1);
    CHECK(pixel(3, 1)[0] == 200);
    CHECK(pixel(4, 1)[0] == 0);   // past the layout's width
    CHECK(pixel(3, 2)[0] == 0);   // past its height
}

TEST_CASE("Hub75PiDriver shows the corner of a larger layout that the panels cover") {
    RecordGuard guard;
    auto r = rig(20, 10);
    r->open();
    r->paint(7 * 20 + 15, 1, 2, 3);   // (15, 7), the canvas's last pixel
    r->paint(7 * 20 + 16, 9, 9, 9);   // (16, 7), off the canvas
    r->driver.tick();

    REQUIRE(mm::platform::rgbMatrixTestFrameCount() == 1);
    CHECK(pixel(15, 7)[0] == 1); CHECK(pixel(15, 7)[2] == 3);
}

TEST_CASE("Hub75PiDriver gives brightness to the library and keeps every color level") {
    RecordGuard guard;
    auto r = rig(16, 8);
    r->open();
    r->driver.rampBrightness(128);

    r->paint(0, 200, 200, 200);
    r->driver.tick();

    CHECK(mm::platform::rgbMatrixTestBrightness() == 128);
    CHECK(pixel(0, 0)[0] == 200);   // dimmed by lit time, not by value
}

TEST_CASE("Hub75PiDriver sends linear values and hides the curve, the library correcting once") {
    auto driver = std::make_unique<mm::Hub75PiDriver>();
    driver->defineControls();
    CHECK(mm::test::isHidden(*driver, "curve"));
    // A curve saved as CIE, by a state document or a copied driver, still bakes linear.
    REQUIRE(mm::test::setControlValue(*driver, "curve", static_cast<uint8_t>(0)));
    driver->rebuildCorrection(255);
    CHECK(driver->correctionForTest().curve == mm::Correction::Curve::Linear);
}

TEST_CASE("Hub75PiDriver shows a wide profile's red, green and blue, wherever the profile puts them") {
    // A 32-channel moving head, its color at 9 to 11: correcting one light writes all 32 channels.
    RecordGuard guard;
    auto r = rig(16, 8);
    r->open();
    mm::ChannelRole roles[32] = {};
    roles[9] = mm::ChannelRole::Red;
    roles[10] = mm::ChannelRole::Green;
    roles[11] = mm::ChannelRole::Blue;
    r->driver.correctionForTest().rebuild(255, roles, 32);
    r->driver.onCorrectionChanged();   // as the container calls it when the wiring changes
    r->paint(0, 10, 20, 30);
    r->driver.tick();
    REQUIRE(mm::platform::rgbMatrixTestFrameCount() == 1);
    CHECK(pixel(0, 0)[0] == 10); CHECK(pixel(0, 0)[1] == 20); CHECK(pixel(0, 0)[2] == 30);
}

TEST_CASE("Hub75PiDriver shows a white-first profile's colors, not its white") {
    RecordGuard guard;
    auto r = rig(16, 8);
    r->open();
    const mm::ChannelRole wrgb[4] = {mm::ChannelRole::White, mm::ChannelRole::Red, mm::ChannelRole::Green, mm::ChannelRole::Blue};
    r->driver.correctionForTest().rebuild(255, wrgb, 4);
    r->driver.onCorrectionChanged();
    r->paint(0, 10, 20, 30);
    r->driver.tick();
    REQUIRE(mm::platform::rgbMatrixTestFrameCount() == 1);
    CHECK(pixel(0, 0)[0] == 10); CHECK(pixel(0, 0)[1] == 20); CHECK(pixel(0, 0)[2] == 30);
}

TEST_CASE("Hub75PiDriver hands over a layout with fewer lights than its box, the rest dark") {
    RecordGuard guard;
    auto r = rig(16, 8, 20);   // a ring or any shape short of its box
    r->open();
    r->paint(19, 7, 8, 9);     // (3, 1), the last light
    r->driver.tick();
    REQUIRE(mm::platform::rgbMatrixTestFrameCount() == 1);
    CHECK(pixel(3, 1)[0] == 7);
    CHECK(pixel(4, 1)[0] == 0);
}

TEST_CASE("Hub75PiDriver refuses panels another driver drives, and leaves them to it") {
    RecordGuard guard;
    auto first = rig(16, 8);
    auto second = rig(16, 8);
    auto drivers = std::make_unique<mm::Drivers>();   // after the rigs, so it goes first
    drivers->addChild(&first->driver);
    drivers->addChild(&second->driver);
    first->open();
    second->open();
    CHECK(second->driver.severity() == mm::MoonModule::Severity::Error);
    CHECK(std::strcmp(second->driver.status(), "another driver drives the panels") == 0);
    second->driver.tick();
    CHECK(mm::platform::rgbMatrixTestFrameCount() == 0);
    second->driver.release();
    CHECK(mm::platform::rgbMatrixWidth() == 16);   // still the first driver's
}

TEST_CASE("Hub75PiDriver reopens the panels on every library option and on nothing else") {
    auto driver = std::make_unique<mm::Hub75PiDriver>();
    for (const char* n : {"mapping", "panelType", "pixelMapper", "rows", "cols", "chain", "parallel",
                          "multiplexing", "rowAddrType", "scanMode", "pwmBits", "pwmLsbNs", "ditherBits",
                          "slowdown", "rp1Pio", "limitRefresh"}) {
        CAPTURE(n);
        CHECK(driver->affectsPrepare(n));
    }
    CHECK_FALSE(driver->affectsPrepare("localBrightness"));
    CHECK_FALSE(driver->affectsPrepare("shown"));
}

TEST_CASE("Hub75PiDriver names why the panels would not open, and stays inert") {
    mm::platform::setTestRgbMatrix(false);
    auto r = rig(16, 8);
    r->driver.rows = 0;   // refused on a Pi by the library, and every other host has none to try
    r->driver.prepare();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Error);
    CHECK(std::strlen(r->driver.status()) > 0);
    r->driver.tick();   // must not crash, and has nothing to hand over
    CHECK(mm::platform::rgbMatrixWidth() == 0);
}

TEST_CASE("Hub75PiDriver closes the panels on release") {
    RecordGuard guard;
    auto r = rig(16, 8);
    r->open();
    REQUIRE(mm::platform::rgbMatrixWidth() == 16);
    r->driver.release();
    CHECK(mm::platform::rgbMatrixWidth() == 0);
}
