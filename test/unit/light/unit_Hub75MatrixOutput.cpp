/// @module Hub75MatrixOutput
/// @also Hub75Driver, Correction

/// Hub75Driver on a Raspberry Pi's header, pinned on a desktop, no panels attached. The RGB seam records what the output opened and handed over (the `rgbMatrix*` seam in platform.h), as NDI's does for its runtime; the bench Pi then only has to show it.

#include "doctest.h"
#include "light/drivers/Hub75MatrixOutput.h"
#include "light/drivers/Correction.h"
#include "light/drivers/Drivers.h"
#include "correction_presets.h"
#include "../core/conditional_controls.h"
#include "light/layers/Buffer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/PanelsLayout.h"

#include <cstring>
#include <memory>
#include <string>
#include "hub75_bench.h"

namespace {

// A Hub75Driver on a layout of `width` x `height` holding `lights` lights, wired as production wires it, on the heap since a driver and a layer outgrow a test's stack frame. The Rig picks the Pi header by its backend, not by its offered position (bench v2).
struct Rig : Hub75Bench {
    // One raster of `width` by `height`, as a single panel's layout is.
    Rig(uint16_t width, uint16_t height, mm::nrOfLightsType lights) {
        grid.width = width;
        grid.height = height;
        grid.depth = 1;
        layouts.addChild(&grid);
        finish(lights);
    }

    // `across` by `down` panels of `panelW` by `panelH`, chained row by row, each walked `wiring` (0 rows, 1 columns) with `snake` inside: off is what a chained card takes.
    Rig(uint16_t across, uint16_t down, uint16_t panelW, uint16_t panelH, uint8_t wiring, bool snake) {
        panels.horizontalPanels = across;
        panels.verticalPanels = down;
        panels.panelWidth = panelW;
        panels.panelHeight = panelH;
        panels.wiringOrder = wiring;
        panels.snake = snake;
        layouts.addChild(&panels);
        finish(static_cast<mm::nrOfLightsType>(across) * down * panelW * panelH);
        out().rows = panelH;
        out().cols = panelW;
    }

    void finish(mm::nrOfLightsType lights) {
        wire(lights);
        driver.defineControls();
        REQUIRE(driver.selectBackendForTest(mm::platform::Hub75Backend::RgbMatrix));   // by backend, not by position
        REQUIRE(driver.output() != nullptr);
        REQUIRE(driver.output()->hwBlock() == mm::LedHwBlock::RgbMatrix);
        out().rows = 8;   // panels forming a 16x8 canvas, unless a case changes them
        out().cols = 16;
        out().rp1Pio = false;   // RIO, as on every host, unless a case turns PIO on
    }

    mm::Hub75MatrixOutput& out() { return *static_cast<mm::Hub75MatrixOutput*>(driver.output()); }

};

std::unique_ptr<Rig> rig(uint16_t width, uint16_t height, mm::nrOfLightsType lights = 0) {
    return std::make_unique<Rig>(width, height, lights ? lights : static_cast<mm::nrOfLightsType>(width) * height);
}

// A Panels layout in cabling order, each panel in plain rows unless `snake`.
std::unique_ptr<Rig> panelsRig(uint16_t across, uint16_t down, uint16_t panelW, uint16_t panelH, bool snake = false) {
    return std::make_unique<Rig>(across, down, panelW, panelH, uint8_t{0}, snake);
}

const uint8_t* pixel(uint16_t x, uint16_t y) {
    return mm::platform::rgbMatrixTestFrame() + (static_cast<size_t>(y) * mm::platform::rgbMatrixWidth() + x) * 3;
}

// A light corrected through a profile of `n` channels, `roles` placing its color, shows its red, green and blue on the panel's first pixel.
void showsColorsThrough(const mm::ChannelRole* roles, uint8_t n) {
    auto r = rig(16, 8);
    r->open();
    r->driver.correctionForTest().rebuild(255, roles, n);
    r->driver.onCorrectionChanged();   // as the container calls it when the wiring changes
    r->paint(0, 10, 20, 30);
    r->driver.tick();
    REQUIRE(mm::platform::rgbMatrixTestFrameCount() == 1);
    CHECK(pixel(0, 0)[0] == 10); CHECK(pixel(0, 0)[1] == 20); CHECK(pixel(0, 0)[2] == 30);
}

constexpr uint8_t kLinear = static_cast<uint8_t>(mm::Correction::Curve::Linear);   // the curve select indexes Curve
constexpr uint8_t kCie = static_cast<uint8_t>(mm::Correction::Curve::Cie);

}  // namespace

TEST_CASE("Hub75Driver on a Pi offers the Pi header and drives it through the matrix output") {
    RecordGuard guard;
    auto r = rig(16, 8);
    CHECK(r->driver.selectedBackend() == mm::platform::Hub75Backend::RgbMatrix);
    CHECK(std::string(mm::platform::hub75BackendLabel(mm::platform::Hub75Backend::RgbMatrix)) == "Pi header");
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Status);
    CHECK(r->driver.hwBlock() == mm::LedHwBlock::RgbMatrix);
}

TEST_CASE("Hub75MatrixOutput starts from the library's own defaults") {
    const mm::platform::RgbMatrixOptions library{};
    auto owner = std::make_unique<mm::MoonModule>();   // the module its chain canvas would count against; both on the heap, past a test's frame limit
    const auto output = std::make_unique<const mm::Hub75MatrixOutput>(*owner);
    const auto o = output->options();
    CHECK(o.rows == library.rows);
    CHECK(o.cols == library.cols);
    CHECK(o.chain == library.chain);
    CHECK(o.pwmBits == 8);   // the Pi renders at 8, where the library's own default is 11
    CHECK(o.pwmLsbNs == library.pwmLsbNs);
    CHECK(o.hardwarePulse == library.hardwarePulse);
    CHECK(o.busyWaiting == library.busyWaiting);
}

TEST_CASE("Hub75Driver on the Pi header starts its one bitDepth at 8 and hands it over as the library's pwm bits") {
    RecordGuard guard;
    auto r = rig(16, 8);
    CHECK(r->driver.bitDepth == 8);
    r->driver.bitDepth = 9;
    r->open();
    CHECK(mm::platform::rgbMatrixTestOptions().pwmBits == 9);
    CHECK(std::string(r->driver.status()) == "16x8, 9-bit");
}

TEST_CASE("Hub75Driver's bitDepth follows a backend change: a default takes the new output's, a chosen depth is clamped into its range") {
    RecordGuard guard;
    BusPort port;   // LCD_CAM offered first, the Pi header second
    auto d = std::make_unique<mm::Hub75Driver>();   // on the heap, as a driver outgrows a test's stack frame
    d->defineControls();
    REQUIRE(d->selectedBackend() == mm::platform::Hub75Backend::LcdCam);
    CHECK(d->bitDepth == 6);

    auto pick = [&](mm::platform::Hub75Backend backend) {
        REQUIRE(d->selectBackendForTest(backend));   // by backend, not by position: the offered order is not a constant
    };
    pick(mm::platform::Hub75Backend::RgbMatrix);
    REQUIRE(d->selectedBackend() == mm::platform::Hub75Backend::RgbMatrix);
    CHECK(d->bitDepth == 8);    // the bus's default became the Pi's, which renders at 8

    d->bitDepth = 9;
    pick(mm::platform::Hub75Backend::LcdCam);
    CHECK(d->bitDepth == 6);    // a chosen depth past the bus's six, clamped

    d->bitDepth = 4;
    pick(mm::platform::Hub75Backend::RgbMatrix);
    CHECK(d->bitDepth == 4);    // inside the Pi's range, kept
}

TEST_CASE("Hub75Driver bakes the correction again for the output a backend change brings, so the curve is applied once") {
    RecordGuard guard;
    BusPort port;   // LCD_CAM offered first, the Pi header second
    auto d = std::make_unique<mm::Hub75Driver>();
    d->defineControls();
    REQUIRE(d->selectedBackend() == mm::platform::Hub75Backend::LcdCam);
    d->rebuildCorrection(255);
    REQUIRE(d->correction().curve == mm::Correction::Curve::Cie);   // the bus output leaves the curve to the correction

    REQUIRE(d->selectBackendForTest(mm::platform::Hub75Backend::RgbMatrix));
    CHECK(d->correction().curve == mm::Correction::Curve::Linear);   // the library's CIE 1931 table is the curve now

    REQUIRE(d->selectBackendForTest(mm::platform::Hub75Backend::LcdCam));
    CHECK(d->correction().curve == mm::Correction::Curve::Cie);
}

TEST_CASE("Hub75MatrixOutput: the Rig still lands on the Pi header when LCD_CAM is offered first") {
    BusPort port;   // offered before the Pi header
    RecordGuard guard;
    auto r = rig(16, 8);
    REQUIRE(r->driver.selectedBackend() == mm::platform::Hub75Backend::RgbMatrix);
    REQUIRE(dynamic_cast<mm::Hub75MatrixOutput*>(r->driver.output()) != nullptr);
    r->open();
    CHECK(mm::platform::rgbMatrixWidth() == 16);   // the matrix recorder took the frame
}

TEST_CASE("Hub75MatrixOutput opens the panels with its controls under the library's names") {
    RecordGuard guard;
    PioHost pi5("6.18.60+rpt-rpi-2712", true);   // a Pi 5 PIO serves on
    auto r = rig(64, 96);
    r->out().mappingSel = 2;        // adafruit-hat-pwm
    r->out().panelTypeSel = 1;      // FM6126A
    r->out().rows = 32;
    r->out().cols = 64;
    r->out().multiplexingSel = 1;   // Stripe
    r->out().slowdownSel = 3;       // 2
    r->out().rp1Pio = true;
    r->out().inverse = true;
    r->out().hardwarePulse = false;
    r->out().rowAddrSel = 2;        // direct row select
    r->out().scanModeSel = 1;       // interlaced
    r->out().pwmLsbNs = 200;
    r->out().ditherBits = 1;
    r->out().limitRefresh = 120;
    r->out().busyWaiting = !mm::platform::RgbMatrixOptions{}.busyWaiting;
    r->open();

    const auto& o = mm::platform::rgbMatrixTestOptions();
    CHECK(o.rowAddrType == 2);
    CHECK(o.scanMode == 1);
    CHECK(o.pwmLsbNs == 200);
    CHECK(o.ditherBits == 1);
    CHECK(o.limitRefreshHz == 120);
    CHECK(o.busyWaiting == !mm::platform::RgbMatrixOptions{}.busyWaiting);
    CHECK(o.chain == 1);
    CHECK(std::string(o.mapping) == "adafruit-hat-pwm");
    CHECK(std::string(o.panelType) == "FM6126A");
    CHECK(o.rows == 32);
    CHECK(o.cols == 64);
    CHECK(o.parallel == 3);
    CHECK(o.multiplexing == 1);
    CHECK(o.slowdown == 2);
    CHECK(o.rp1Pio);
    CHECK(o.inverse);
    CHECK_FALSE(o.hardwarePulse);
    CHECK(std::string(r->driver.status()) == "64x96, 8-bit, PIO");
}

TEST_CASE("Hub75MatrixOutput leaves the GPIO slowdown to the library's model default on auto") {
    RecordGuard guard;
    auto r = rig(16, 8);
    r->open();
    CHECK(mm::platform::rgbMatrixTestOptions().slowdown == -1);
    CHECK(std::string(mm::platform::rgbMatrixTestOptions().panelType).empty());
}

TEST_CASE("Hub75MatrixOutput offers the library's documented GPIO slowdown range, up to 10") {
    RecordGuard guard;
    auto r = rig(16, 8);
    r->out().slowdownSel = 11;   // the last option, "10"
    r->open();
    CHECK(mm::platform::rgbMatrixTestOptions().slowdown == 10);
}

TEST_CASE("Hub75MatrixOutput on a kernel that stalls PIO hides rp1Pio and opens on RIO, saying why") {
    RecordGuard guard;
    PioHost stalls("6.18.50+rpt-rpi-2712", true);
    auto r = rig(16, 8);
    CHECK(mm::test::isHidden(r->driver, "rp1Pio"));
    r->out().rp1Pio = true;        // saved on a kernel PIO served on
    r->open();
    CHECK_FALSE(mm::platform::rgbMatrixTestOptions().rp1Pio);
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Warning);
    CHECK(std::string(r->driver.status()).find("RIO only") != std::string::npos);
}

TEST_CASE("Hub75MatrixOutput starts on PIO where the kernel runs it, and on RIO where the check refuses") {
    mm::MoonModule owner;
    {
        PioHost pi5("6.18.60+rpt-rpi-2712", true);
        CHECK(std::make_unique<mm::Hub75MatrixOutput>(owner)->rp1Pio);
    }
    {
        PioHost stalls("6.18.50+rpt-rpi-2712", true);
        CHECK_FALSE(std::make_unique<mm::Hub75MatrixOutput>(owner)->rp1Pio);
    }
    {
        PioHost pi4("6.18.60+rpt-rpi-v8", false);   // no /dev/pio0
        CHECK_FALSE(std::make_unique<mm::Hub75MatrixOutput>(owner)->rp1Pio);
    }
}

TEST_CASE("Hub75MatrixOutput shows rp1Pio where the kernel runs PIO") {
    RecordGuard guard;
    PioHost pi5("6.18.60+rpt-rpi-2712", true);
    auto r = rig(16, 8);
    CHECK_FALSE(mm::test::isHidden(r->driver, "rp1Pio"));
}

TEST_CASE("Hub75MatrixOutput hands light i to canvas pixel i, row by row") {
    RecordGuard guard;
    auto r = rig(16, 8);
    r->open();
    CHECK(std::string(r->driver.status()) == "16x8, 8-bit");

    r->paint(0, 10, 20, 30);
    r->paint(3 * 16 + 5, 40, 50, 60);   // (5, 3)
    r->driver.tick();

    REQUIRE(mm::platform::rgbMatrixTestFrameCount() == 1);
    CHECK(pixel(0, 0)[0] == 10); CHECK(pixel(0, 0)[1] == 20); CHECK(pixel(0, 0)[2] == 30);
    CHECK(pixel(5, 3)[0] == 40); CHECK(pixel(5, 3)[1] == 50); CHECK(pixel(5, 3)[2] == 60);
}

TEST_CASE("Hub75MatrixOutput refuses a layout smaller than one panel, in full, and opens nothing") {
    RecordGuard guard;
    auto r = rig(4, 2);   // 8 lights, a panel is 16 by 8
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Error);
    CHECK(std::string(r->driver.status()) == "The layout holds fewer lights than one 16x8 panel.");
    CHECK(mm::platform::rgbMatrixWidth() == 0);
}

TEST_CASE("Hub75MatrixOutput takes a layout of the canvas turned on its side as a match, since the layout owns rotation") {
    RecordGuard guard;
    auto r = std::make_unique<Rig>(1, 1, 8, 16, uint8_t{1}, false);   // one panel 8 across and 16 down, its rows walked down the columns
    r->out().rows = 8;   // the card's 16x8 panel, turned on its side by the layout
    r->out().cols = 16;
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Status);
    CHECK(std::string(r->driver.status()) == "16x8, 8-bit");
}

TEST_CASE("Hub75MatrixOutput follows two stacked layouts: two panels' worth of lights make two outputs, and the status says so") {
    RecordGuard guard;
    mm::GridLayout second;   // outlives the rig, whose Layouts holds it
    second.width = 16;
    second.height = 8;
    second.depth = 1;
    auto r = rig(16, 8);
    r->layouts.addChild(&second);
    r->layouts.applyState();
    r->layer.applyState();
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Status);
    CHECK(std::string(r->driver.status()) == "16x16, 8-bit");
    CHECK(mm::platform::rgbMatrixTestOptions().parallel == 2);
}

TEST_CASE("Hub75MatrixOutput shows as many of a larger layout's lights as the panels hold") {
    RecordGuard guard;
    auto r = rig(20, 10);
    r->open();
    r->paint(127, 1, 2, 3);   // the canvas's last pixel, (15, 7)
    r->paint(128, 9, 9, 9);   // one past the canvas
    r->driver.tick();

    REQUIRE(mm::platform::rgbMatrixTestFrameCount() == 1);
    CHECK(pixel(15, 7)[0] == 1); CHECK(pixel(15, 7)[2] == 3);
}

TEST_CASE("Hub75MatrixOutput joins every warning that applies with \"; \", the seam's first, so none hides another") {
    RecordGuard guard;
    Realtime none(false);
    auto r = rig(20, 10);   // a raster wider than the 16x8 panel: another light count, and panel 1 leaves plain rows
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Warning);
    CHECK(std::string(r->driver.status()) ==
          "no real-time priority, so colors may flicker: grant CAP_SYS_NICE and reserve a core with isolcpus; layout 200 lights, panels 128; "
          "the layout's panel 1 does not run in plain rows of 16: lay the panels out in cabling order, each in plain rows with snake off");
}

TEST_CASE("Hub75MatrixOutput warns of lights past the last whole panel, and judges only the panels it drives") {
    RecordGuard guard;
    mm::GridLayout extra;   // 8 lights after one 16x8 panel in plain rows; outlives the rig, whose Layouts holds it
    extra.width = 4;
    extra.height = 2;
    extra.depth = 1;
    auto r = rig(16, 8, 136);
    r->layouts.addChild(&extra);
    r->layouts.applyState();
    r->layer.applyState();
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Warning);
    CHECK(std::string(r->driver.status()) == "layout 136 lights, panels 128");
}

TEST_CASE("Hub75MatrixOutput on CIE 1931 gives brightness and the curve to the library, the values kept whole") {
    RecordGuard guard;
    auto r = rig(16, 8);
    r->open();
    r->driver.rampBrightness(128);

    r->paint(0, 200, 200, 200);
    r->driver.tick();

    CHECK(mm::platform::rgbMatrixTestBrightness() == 128);
    CHECK(mm::platform::rgbMatrixTestCie1931());
    CHECK(pixel(0, 0)[0] == 200);   // the library dims it, not the value
}

TEST_CASE("Hub75MatrixOutput on CIE 1931 has the correction bake Linear, the curve select still shown") {
    RecordGuard guard;
    auto r = rig(16, 8);
    CHECK_FALSE(mm::test::isHidden(r->driver, "curve"));
    r->driver.rebuildCorrection(255);
    CHECK(r->driver.appliesCurve());
    CHECK(r->driver.correctionForTest().curve == mm::Correction::Curve::Linear);
}

TEST_CASE("Hub75MatrixOutput on another curve leaves brightness and the curve to the correction, the library direct") {
    RecordGuard guard;
    auto r = rig(16, 8);
    r->open();
    REQUIRE(mm::test::setControlValue(r->driver, "curve", kLinear));
    r->driver.rebuildCorrection(128);
    CHECK_FALSE(r->driver.appliesCurve());
    CHECK_FALSE(r->driver.dimsByTime());

    r->paint(0, 200, 200, 200);
    r->driver.tick();

    CHECK(mm::platform::rgbMatrixTestBrightness() == 255);
    CHECK_FALSE(mm::platform::rgbMatrixTestCie1931());
    CHECK(pixel(0, 0)[0] >= 99);    // 200 at half brightness, scaled by the correction
    CHECK(pixel(0, 0)[0] <= 101);
}

TEST_CASE("Hub75MatrixOutput shows a wide profile's red, green and blue, wherever the profile puts them") {
    RecordGuard guard;
    const WideProfile wide;
    showsColorsThrough(wide.roles, 32);
}

TEST_CASE("Hub75MatrixOutput shows a white-first profile's colors, not its white") {
    RecordGuard guard;
    const mm::ChannelRole wrgb[4] = {mm::ChannelRole::White, mm::ChannelRole::Red, mm::ChannelRole::Green, mm::ChannelRole::Blue};
    showsColorsThrough(wrgb, 4);
}

TEST_CASE("Hub75MatrixOutput hands over a layout with fewer lights than its box, the rest dark") {
    RecordGuard guard;
    auto r = rig(16, 8, 20);   // a ring or any shape short of its box
    r->open();
    r->paint(19, 7, 8, 9);     // (3, 1), the last light
    r->driver.tick();
    REQUIRE(mm::platform::rgbMatrixTestFrameCount() == 1);
    CHECK(pixel(3, 1)[0] == 7);
    CHECK(pixel(4, 1)[0] == 0);
}

TEST_CASE("Hub75MatrixOutput refuses panels another driver drives, and leaves them to it") {
    RecordGuard guard;
    auto first = rig(16, 8);
    auto second = rig(16, 8);
    auto drivers = std::make_unique<mm::Drivers>();   // after the rigs, so it goes first
    drivers->addChild(&first->driver);
    drivers->addChild(&second->driver);
    first->open();
    second->open();
    CHECK(second->driver.severity() == mm::MoonModule::Severity::Error);
    second->driver.tick();
    CHECK(mm::platform::rgbMatrixTestFrameCount() == 0);
    second->driver.release();
    CHECK(mm::platform::rgbMatrixWidth() == 16);   // still the first driver's
}

TEST_CASE("Hub75MatrixOutput reopens the panels on every library option and on nothing else") {
    RecordGuard guard;
    auto r = rig(16, 8);
    for (const char* n : {"mapping", "panelType", "rows", "cols", "chain", "multiplexing",
                          "rowAddrType", "scanMode", "bitDepth", "pwmLsbNs", "ditherBits", "slowdown",
                          "rp1Pio", "limitRefresh", "inverse", "hardwarePulse", "busyWaiting", "peripheral"}) {
        CAPTURE(n);
        CHECK(r->driver.affectsPrepare(n));
    }
    CHECK_FALSE(r->driver.affectsPrepare("localBrightness"));
    CHECK_FALSE(r->driver.affectsPrepare("shown"));
}

TEST_CASE("Hub75MatrixOutput names why the panels would not open, and stays inert") {
    RecordGuard guard;
    auto r = rig(16, 8);
    mm::platform::setTestRgbMatrix(false);   // a fit geometry on a host without the library: the seam refuses the open
    // Where the library is linked the open would reach it and light real panels, so the case is this host's alone.
    if (mm::platform::rgbMatrixAvailable()) return;
    r->driver.prepare();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Error);
    CHECK(std::string(r->driver.status()) == "needs a Raspberry Pi build with rpi-rgb-led-matrix");
    r->driver.tick();   // must not crash, and has no frame to hand over
    CHECK(mm::platform::rgbMatrixWidth() == 0);
}

TEST_CASE("Hub75MatrixOutput's chain canvas counts in the driver's memory while the panels run, and goes with them") {
    RecordGuard guard;
    auto r = panelsRig(2, 1, 16, 8);   // two 16x8 panels in cabling order, one chain: a 256-light canvas
    r->out().chain = 2;
    r->open();
    REQUIRE(r->driver.severity() == mm::MoonModule::Severity::Status);
    CHECK(r->out().heapBytes() == 256 * 3);
    CHECK(r->driver.dynamicBytes() == 2 * 256 * 3);   // the corrected frame and the canvas

    r->driver.release();
    CHECK(r->driver.dynamicBytes() == 0);

    r->open();
    REQUIRE(r->driver.dynamicBytes() == 2 * 256 * 3);
    BusPort port;   // the card now offers LCD_CAM first and keeps the Pi header picked
    r->driver.rebuildControls();
    REQUIRE(r->driver.selectedBackend() == mm::platform::Hub75Backend::RgbMatrix);
    REQUIRE(r->driver.selectBackendForTest(mm::platform::Hub75Backend::LcdCam));   // the bus output takes over and the Pi output goes, canvas and all
    CHECK(r->driver.dynamicBytes() < 256 * 3 * 2);
}

TEST_CASE("Hub75MatrixOutput closes the panels on release") {
    RecordGuard guard;
    auto r = rig(16, 8);
    r->open();
    REQUIRE(mm::platform::rgbMatrixWidth() == 16);
    r->driver.release();
    CHECK(mm::platform::rgbMatrixWidth() == 0);
}

// Lights run panel by panel in cabling order, so the Pi output puts each where its panel sits on the chain canvas; the expected cells are the placement rule by hand.

TEST_CASE("Hub75Driver on a Pi puts the second panel of a chain to the right of the first") {
    RecordGuard guard;
    auto r = rig(32, 8);   // 256 lights: two 16 by 8 panels
    r->out().chain = 2;
    r->paint(0, 200, 0, 0);       // panel 0, first light
    r->paint(15, 0, 200, 0);      // panel 0, last light of its first row
    r->paint(16, 0, 0, 200);      // panel 0, first light of its second row
    r->paint(128, 100, 100, 100); // panel 1, first light
    r->paint(255, 200, 200, 0);   // panel 1, last light
    r->open();
    r->driver.tick();
    REQUIRE(mm::platform::rgbMatrixWidth() == 32);
    CHECK(pixel(0, 0)[0] == 200);
    CHECK(pixel(15, 0)[1] == 200);
    CHECK(pixel(0, 1)[2] == 200);
    CHECK(pixel(16, 0)[0] == 100);
    CHECK(pixel(31, 7)[0] == 200);
    CHECK(pixel(31, 7)[1] == 200);
}

TEST_CASE("Hub75Driver on a Pi stacks the chains: six panels, two to a chain on three outputs, make a 32 by 24 canvas") {
    RecordGuard guard;
    auto r = rig(32, 24);   // 768 lights: six 16 by 8 panels
    r->out().chain = 2;
    r->paint(256, 200, 0, 0);      // panel 2: place 0 of chain 1
    r->paint(640, 0, 200, 0);      // panel 5: place 1 of chain 2
    r->paint(767, 0, 0, 200);      // the last light of the last panel
    r->open();
    r->driver.tick();
    REQUIRE(mm::platform::rgbMatrixWidth() == 32);
    REQUIRE(mm::platform::rgbMatrixHeight() == 24);
    CHECK(pixel(0, 8)[0] == 200);
    CHECK(pixel(16, 16)[1] == 200);
    CHECK(pixel(31, 23)[2] == 200);
}

TEST_CASE("Hub75Driver on a Pi leaves a chain of one as it was: light i is canvas pixel i") {
    RecordGuard guard;
    auto r = rig(16, 24);   // three 16 by 8 panels, one to a chain
    r->out().chain = 1;
    r->paint(130, 200, 0, 0);
    r->open();
    r->driver.tick();
    CHECK(pixel(2, 8)[0] == 200);   // light 130 is panel 1 (row 8 of the canvas), column 2
}

// One color map, linked to the depth: the library's CIE 1931 table fills 11 planes, MoonLight's curve 8 (an 8-bit value shifted up by 3); the Pi renders at 8.

TEST_CASE("Hub75Driver on a Pi offers 9 to 11 bits only on the CIE 1931 curve") {
    RecordGuard guard;
    auto r = rig(16, 8);
    auto depthMax = [&] {
        const int i = mm::test::controlIndex(r->driver, "bitDepth");
        REQUIRE(i >= 0);
        return r->driver.controls()[static_cast<uint8_t>(i)].max;
    };
    CHECK(r->driver.bitDepth == 8);
    CHECK(depthMax() == 11);   // the default curve is CIE 1931

    REQUIRE(mm::test::setControlValue(r->driver, "curve", kLinear));
    r->driver.onControlChanged("curve");
    CHECK(depthMax() == 8);
}

TEST_CASE("Hub75Driver on a Pi clamps a saved depth into the curve's range on a switch, and keeps 8 on the way back") {
    RecordGuard guard;
    auto r = rig(16, 8);
    r->driver.bitDepth = 11;   // allowed on the default CIE 1931 curve

    REQUIRE(mm::test::setControlValue(r->driver, "curve", kLinear));
    r->driver.onControlChanged("curve");
    CHECK(r->driver.bitDepth == 8);

    REQUIRE(mm::test::setControlValue(r->driver, "curve", kCie));
    r->driver.onControlChanged("curve");
    CHECK(r->driver.bitDepth == 8);   // back on CIE 1931 the user raises it again, it is not restored
}

TEST_CASE("Hub75Driver on a Pi brings a depth saved above 8 under another curve down to 8 when the panels open") {
    RecordGuard guard;
    auto r = rig(16, 8);
    REQUIRE(mm::test::setControlValue(r->driver, "curve", kLinear));   // a config loaded from storage sets values and skips the control change
    r->driver.bitDepth = 11;
    r->open();
    CHECK(r->driver.bitDepth == 8);
    CHECK(mm::platform::rgbMatrixTestOptions().pwmBits == 8);
    CHECK(std::string(r->driver.status()) == "16x8, 8-bit");
}

// The panels follow the layout; Hub75Fit's cases pin the geometry text, these that the output shows it whole and leaves the panels closed.


TEST_CASE("Hub75MatrixOutput derives the parallel outputs from the layout, and the card has no such control") {
    RecordGuard guard;
    auto r = panelsRig(2, 3, 16, 8);   // six 16 by 8 panels
    r->out().chain = 2;
    CHECK(mm::test::controlIndex(r->driver, "parallel") < 0);
    r->open();
    CHECK(mm::platform::rgbMatrixTestOptions().parallel == 3);
    CHECK(std::string(r->driver.status()) == "32x24, 8-bit");
}

TEST_CASE("Hub75MatrixOutput names the fix in full for panels that do not cable into the chain, and opens nothing") {
    RecordGuard guard;
    auto r = rig(112, 8);   // seven 16 by 8 panels
    r->out().chain = 2;
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Error);
    const std::string s = r->driver.status();
    CHECK(s.rfind("7 panels cannot be cabled in chains of 2.", 0) == 0);
    CHECK(s.find("Cable all 7: 1 chain of 7") != std::string::npos);
    CHECK(s.find("Keep chains of 2: use 6 panels, 1 spare, as 3 chains") != std::string::npos);
    CHECK(s.size() > 64);   // past 64 bytes, so the status holds a long message whole
    CHECK(mm::platform::rgbMatrixWidth() == 0);
}

TEST_CASE("Hub75MatrixOutput says how many chains the mapping drives when the layout needs more") {
    RecordGuard guard;
    ChainLimit limit(1);
    auto r = rig(48, 8);   // three 16 by 8 panels, one to a chain: three outputs
    r->out().mappingSel = 1;   // adafruit-hat
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Error);
    CHECK(std::string(r->driver.status()).rfind("3 panels in chains of 1 need 3 chains, but the adafruit-hat mapping drives at most 1.", 0) == 0);
    CHECK(mm::platform::rgbMatrixWidth() == 0);
}

// A raster across chained panels matches every count, so only the walk catches it: the wall shows bands.
TEST_CASE("Hub75MatrixOutput warns when a raster layout feeds chained panels, naming the panel and the fix, and the panels still run") {
    RecordGuard guard;
    auto r = rig(256, 64);   // one raster across two 128 by 64 panels
    r->out().rows = 64;
    r->out().cols = 128;
    r->out().chain = 2;
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Warning);
    CHECK(std::string(r->driver.status()) ==
          "the layout's panel 1 does not run in plain rows of 128: lay the panels out in cabling order, each in plain rows with snake off");
    CHECK(mm::platform::rgbMatrixWidth() == 256);
}

TEST_CASE("Hub75MatrixOutput warns when a Panels layout snakes inside its panels, the Panels layout's default") {
    RecordGuard guard;
    auto r = panelsRig(2, 1, 16, 8, true);
    r->out().chain = 2;
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Warning);
    CHECK(std::string(r->driver.status()).rfind("the layout's panel 1 does not run in plain rows of 16", 0) == 0);
}

TEST_CASE("Hub75MatrixOutput quotes the rates from the card's LSB time") {
    RecordGuard guard;
    auto r = panelsRig(5, 1, 128, 64);   // five 128 by 64 panels, which chains of 2 cannot cable
    r->out().rows = 64;
    r->out().cols = 128;
    r->out().chain = 2;
    r->driver.bitDepth = 11;
    r->open();
    CHECK(std::string(r->driver.status()).find("1 chain of 5 (640x64, 57 Hz)") != std::string::npos);

    r->out().pwmLsbNs = 50;
    r->open();
    CHECK(std::string(r->driver.status()).find("1 chain of 5 (640x64, 71 Hz)") != std::string::npos);
}

TEST_CASE("Hub75Driver on a Pi reopens the panels on a curve change only when the depth had to change") {
    RecordGuard guard;
    auto r = rig(16, 8);
    r->driver.bitDepth = 11;
    r->open();   // CIE 1931 at 11 bits
    REQUIRE(mm::test::setControlValue(r->driver, "curve", kLinear));
    r->driver.onControlChanged("curve");
    CHECK(r->driver.affectsPrepare("curve"));   // the library holds 11 planes the new map leaves empty
    r->driver.prepare();
    CHECK(mm::platform::rgbMatrixTestOptions().pwmBits == 8);
    CHECK_FALSE(r->driver.affectsPrepare("curve"));   // reopened at 8: a further switch changes no depth

    REQUIRE(mm::test::setControlValue(r->driver, "curve", kCie));
    r->driver.onControlChanged("curve");
    CHECK_FALSE(r->driver.affectsPrepare("curve"));   // back on CIE 1931 keeps 8: the open stands
}

TEST_CASE("Hub75MatrixOutput quotes a Pi at 8 bits from the planes the library clocks, and the dither lifts it") {
    RecordGuard guard;
    auto r = panelsRig(8, 1, 128, 64); // eight 128 by 64 panels, which chains of 3 cannot cable
    r->out().rows = 64;
    r->out().cols = 128;
    r->out().chain = 3;
    r->open();
    CHECK(std::string(r->driver.status()).find("1 chain of 8 (1024x64, 54 Hz)") != std::string::npos);   // planes 3 to 10

    r->out().ditherBits = 2;   // every plane's lit time a quarter
    r->open();
    CHECK(std::string(r->driver.status()).find("1 chain of 8 (1024x64, 63 Hz)") != std::string::npos);
}
