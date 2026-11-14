/// @module Hub75PioOutput
/// @also Hub75Driver, Correction

/// Hub75Driver on a Raspberry Pi 5's header, through the RP1's PIO, pinned on a desktop, no panels attached. The PIO seam records what the output opened and handed over (the `hub75Pio*` seam in platform.h), as the RGB seam's does for its runtime; the bench Pi then only has to show it.

#include "doctest.h"
#include "light/drivers/Hub75PioOutput.h"
#include "light/drivers/Correction.h"
#include "light/drivers/Drivers.h"
#include "light/drivers/Hub75MatrixOutput.h"
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

// A Hub75Driver on a layout of `width` x `height` holding `lights` lights, wired as production wires it, on the heap since a driver and a layer outgrow a test's stack frame. The Rig picks Pi PIO by its backend, not by its offered position (bench v2).
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
        REQUIRE(driver.selectBackendForTest(mm::platform::Hub75Backend::Pio));   // by backend, not by position
        REQUIRE(driver.output() != nullptr);
        REQUIRE(driver.output()->hwBlock() == mm::LedHwBlock::RgbMatrix);
        out().rows = 32;   // panels forming a 16x32 canvas, unless a case changes them
        out().cols = 16;
        out().chain = 1;   // one panel per chain, unless a case chains them: the output's own default is 2, the wall's
    }

    mm::Hub75PioOutput& out() { return *static_cast<mm::Hub75PioOutput*>(driver.output()); }

};

std::unique_ptr<Rig> rig(uint16_t width, uint16_t height, mm::nrOfLightsType lights = 0) {
    return std::make_unique<Rig>(width, height, lights ? lights : static_cast<mm::nrOfLightsType>(width) * height);
}

// A Panels layout in cabling order, each panel in plain rows unless `snake`.
std::unique_ptr<Rig> panelsRig(uint16_t across, uint16_t down, uint16_t panelW, uint16_t panelH, bool snake = false) {
    return std::make_unique<Rig>(across, down, panelW, panelH, uint8_t{0}, snake);
}

const uint8_t* pixel(uint16_t x, uint16_t y) {
    return mm::platform::hub75PioTestFrame() + (static_cast<size_t>(y) * mm::platform::hub75PioWidth() + x) * 3;
}

// A light corrected through a profile of `n` channels, `roles` placing its color, shows its red, green and blue on the panel's first pixel.
void showsColorsThrough(const mm::ChannelRole* roles, uint8_t n) {
    auto r = rig(16, 32);
    r->open();
    r->driver.correctionForTest().rebuild(255, roles, n);
    r->driver.onCorrectionChanged();   // as the container calls it when the wiring changes
    r->paint(0, 10, 20, 30);
    r->driver.tick();
    REQUIRE(mm::platform::hub75PioTestFrameCount() == 1);
    CHECK(pixel(0, 0)[0] == 10); CHECK(pixel(0, 0)[1] == 20); CHECK(pixel(0, 0)[2] == 30);
}

constexpr uint8_t kLinear = static_cast<uint8_t>(mm::Correction::Curve::Linear);   // the curve select indexes Curve

}  // namespace

TEST_CASE("Hub75Driver on a Pi 5 offers Pi PIO and drives it through the PIO output") {
    PioRecordGuard guard;
    auto r = rig(16, 32);
    CHECK(r->driver.selectedBackend() == mm::platform::Hub75Backend::Pio);
    CHECK(std::string(mm::platform::hub75BackendLabel(mm::platform::Hub75Backend::Pio)) == "Pi PIO");
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Status);
    CHECK(r->driver.hwBlock() == mm::LedHwBlock::RgbMatrix);
}

TEST_CASE("Hub75PioOutput starts from the seam's own defaults") {
    const mm::platform::Hub75PioOptions defaults{};
    auto owner = std::make_unique<mm::MoonModule>();   // the module its chain canvas would count against; both on the heap, past a test's frame limit
    const auto output = std::make_unique<const mm::Hub75PioOutput>(*owner);
    const auto o = output->options();
    CHECK(o.rows == defaults.rows);
    CHECK(o.cols == defaults.cols);
    CHECK(o.chain == defaults.chain);
    CHECK(o.parallel == 1);   // the layout decides it in open(), not the default
    CHECK(o.scanRate == 32);   // 64 rows, the default, derive 1/32
    CHECK(o.bitDepth == defaults.bitDepth);
    CHECK(o.pixelClockHz == defaults.pixelClockHz);
    CHECK(o.latchBlankWords == defaults.latchBlankWords);
    CHECK(o.clockFallingEdge == defaults.clockFallingEdge);
}

TEST_CASE("Hub75PioOutput derives the scan from the panel's rows: 64 rows is 1/32, 32 rows 1/16, 16 rows 1/8") {
    auto owner = std::make_unique<mm::MoonModule>();   // the module its chain canvas would count against; both on the heap, past a test's frame limit
    auto output = std::make_unique<mm::Hub75PioOutput>(*owner);
    CHECK(output->options().scanRate == 32);   // 64 rows, the default, is 1/32 — the wall needs no hand setting
    output->rows = 32;
    CHECK(output->options().scanRate == 16);
    output->rows = 16;
    CHECK(output->options().scanRate == 8);
}

TEST_CASE("Hub75Driver on Pi PIO starts its one bitDepth at 6 and hands it over as the engine's depth") {
    PioRecordGuard guard;
    auto r = rig(16, 32);
    CHECK(r->driver.bitDepth == 6);
    r->driver.bitDepth = 4;
    r->open();
    CHECK(mm::platform::hub75PioTestOptions().bitDepth == 4);
    CHECK(std::string(r->driver.status()) == "16x32, 4-bit, PIO");
}

TEST_CASE("Hub75PioOutput offers 2 to 8 bits, and a saved depth outside clamps into the range when the panels open") {
    PioRecordGuard guard;
    auto r = rig(16, 32);
    const int i = mm::test::controlIndex(r->driver, "bitDepth");
    REQUIRE(i >= 0);
    CHECK(r->driver.controls()[static_cast<uint8_t>(i)].min == 2);
    CHECK(r->driver.controls()[static_cast<uint8_t>(i)].max == 8);

    r->driver.bitDepth = 11;   // a config loaded from storage sets values and skips the control change
    r->open();
    CHECK(r->driver.bitDepth == 8);
    CHECK(mm::platform::hub75PioTestOptions().bitDepth == 8);

    r->driver.bitDepth = 1;
    r->open();
    CHECK(r->driver.bitDepth == 2);
    CHECK(mm::platform::hub75PioTestOptions().bitDepth == 2);
}

TEST_CASE("Hub75Driver on Pi PIO opens at 8-bit when set, and the status names it") {
    PioRecordGuard guard;
    auto r = rig(16, 32);
    r->driver.bitDepth = 8;
    r->open();
    CHECK(mm::platform::hub75PioTestOptions().bitDepth == 8);
    CHECK(std::string(r->driver.status()) == "16x32, 8-bit, PIO");
}

TEST_CASE("Hub75Driver's bitDepth follows a backend change between the two Pi outputs: a default takes the new output's, a chosen depth is clamped into its range") {
    RecordGuard rgb;
    PioRecordGuard pio;
    auto d = std::make_unique<mm::Hub75Driver>();   // on the heap, as a driver outgrows a test's stack frame
    d->defineControls();
    REQUIRE(d->selectedBackend() == mm::platform::Hub75Backend::RgbMatrix);
    CHECK(d->bitDepth == 8);

    auto pick = [&](mm::platform::Hub75Backend backend) {
        REQUIRE(d->selectBackendForTest(backend));   // by backend, not by position: the offered order is not a constant
    };
    pick(mm::platform::Hub75Backend::Pio);
    REQUIRE(d->selectedBackend() == mm::platform::Hub75Backend::Pio);
    REQUIRE(dynamic_cast<mm::Hub75PioOutput*>(d->output()) != nullptr);   // the swap happened, though both outputs claim the same header block
    CHECK(d->bitDepth == 6);    // the matrix default became the PIO's initial; 8 would be inside the PIO's 2..8 range, but a default still takes the new output's starting depth

    d->bitDepth = 4;
    pick(mm::platform::Hub75Backend::RgbMatrix);
    REQUIRE(dynamic_cast<mm::Hub75MatrixOutput*>(d->output()) != nullptr);
    CHECK(d->bitDepth == 4);    // inside the matrix range, kept
}

TEST_CASE("Hub75Driver bakes the correction again for the output a backend change brings, so the curve is applied once") {
    RecordGuard rgb;
    PioRecordGuard pio;
    auto d = std::make_unique<mm::Hub75Driver>();
    d->defineControls();
    REQUIRE(d->selectedBackend() == mm::platform::Hub75Backend::RgbMatrix);
    d->rebuildCorrection(255);
    REQUIRE(d->correction().curve == mm::Correction::Curve::Linear);   // the library's CIE 1931 table is the curve

    REQUIRE(d->selectBackendForTest(mm::platform::Hub75Backend::Pio));
    CHECK(d->correction().curve == mm::Correction::Curve::Cie);   // the PIO output leaves the curve to the correction

    REQUIRE(d->selectBackendForTest(mm::platform::Hub75Backend::RgbMatrix));
    CHECK(d->correction().curve == mm::Correction::Curve::Linear);
}

TEST_CASE("Hub75PioOutput: the Rig still lands on Pi PIO when the Pi header is offered first") {
    RecordGuard rgb;   // offered before Pi PIO
    PioRecordGuard pio;
    auto r = rig(16, 32);
    REQUIRE(r->driver.selectedBackend() == mm::platform::Hub75Backend::Pio);
    REQUIRE(dynamic_cast<mm::Hub75PioOutput*>(r->driver.output()) != nullptr);
    r->open();
    CHECK(mm::platform::hub75PioWidth() == 16);   // the PIO recorder took the frame, not the matrix one
    CHECK(mm::platform::rgbMatrixWidth() == 0);
}

TEST_CASE("Hub75PioOutput opens the wall with its controls: three chains of two 128x64 panels at 1/32 scan") {
    PioRecordGuard guard;
    auto r = panelsRig(2, 3, 128, 64);   // the wall: six 128 by 64 panels
    r->out().chain = 2;
    r->driver.scanSel = 0;   // 1/8: the bus output's select, which the PIO output does not read — the scan derives from rows, so the wall is 1/32 with no hand setting
    r->driver.bitDepth = 4;
    r->driver.clockEdgeSel = 1;   // falling
    r->out().pixelClockKhz = 20000;
    r->out().latchBlankWords = 2;
    r->open();

    const auto& o = mm::platform::hub75PioTestOptions();
    CHECK(o.rows == 64);
    CHECK(o.cols == 128);
    CHECK(o.chain == 2);
    CHECK(o.parallel == 3);
    CHECK(o.scanRate == 32);
    CHECK(o.bitDepth == 4);
    CHECK(o.pixelClockHz == 20000000);
    CHECK(o.latchBlankWords == 2);
    CHECK(o.clockFallingEdge);
    CHECK(std::string(r->driver.status()) == "256x192, 4-bit, PIO");
}

TEST_CASE("Hub75PioOutput hands light i to canvas pixel i, row by row") {
    PioRecordGuard guard;
    auto r = rig(16, 32);
    r->open();
    CHECK(std::string(r->driver.status()) == "16x32, 6-bit, PIO");

    r->paint(0, 10, 20, 30);
    r->paint(3 * 16 + 5, 40, 50, 60);   // (5, 3)
    r->driver.tick();

    REQUIRE(mm::platform::hub75PioTestFrameCount() == 1);
    CHECK(pixel(0, 0)[0] == 10); CHECK(pixel(0, 0)[1] == 20); CHECK(pixel(0, 0)[2] == 30);
    CHECK(pixel(5, 3)[0] == 40); CHECK(pixel(5, 3)[1] == 50); CHECK(pixel(5, 3)[2] == 60);
}

TEST_CASE("Hub75PioOutput refuses a layout smaller than one panel, in full, and opens nothing") {
    PioRecordGuard guard;
    auto r = rig(4, 2);   // 8 lights, a panel is 16 by 32
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Error);
    CHECK(std::string(r->driver.status()) == "The layout holds fewer lights than one 16x32 panel.");
    CHECK(mm::platform::hub75PioWidth() == 0);
}

TEST_CASE("Hub75PioOutput takes a layout of the canvas turned on its side as a match, since the layout owns rotation") {
    PioRecordGuard guard;
    auto r = std::make_unique<Rig>(1, 1, 32, 16, uint8_t{1}, false);   // one panel 32 across and 16 down, its rows walked down the columns
    r->out().rows = 32;   // the card's 16x32 panel, turned on its side by the layout
    r->out().cols = 16;
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Status);
    CHECK(std::string(r->driver.status()) == "16x32, 6-bit, PIO");
}

TEST_CASE("Hub75PioOutput follows two stacked layouts: two panels' worth of lights make two outputs, and the status says so") {
    PioRecordGuard guard;
    mm::GridLayout second;   // outlives the rig, whose Layouts holds it
    second.width = 16;
    second.height = 32;
    second.depth = 1;
    auto r = rig(16, 32);
    r->layouts.addChild(&second);
    r->layouts.applyState();
    r->layer.applyState();
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Status);
    CHECK(std::string(r->driver.status()) == "16x64, 6-bit, PIO");
    CHECK(mm::platform::hub75PioTestOptions().parallel == 2);
}

TEST_CASE("Hub75PioOutput shows as many of a larger layout's lights as the panels hold") {
    PioRecordGuard guard;
    auto r = rig(20, 40);
    r->open();
    r->paint(511, 1, 2, 3);   // the canvas's last pixel, (15, 31)
    r->paint(512, 9, 9, 9);   // one past the canvas
    r->driver.tick();

    REQUIRE(mm::platform::hub75PioTestFrameCount() == 1);
    CHECK(pixel(15, 31)[0] == 1); CHECK(pixel(15, 31)[2] == 3);
}

TEST_CASE("Hub75PioOutput joins a light count other than the canvas's with the walk's panel, so none hides another") {
    PioRecordGuard guard;
    auto r = rig(20, 40);   // a raster wider than the 16x32 panel: another light count, and panel 1 leaves plain rows
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Warning);
    CHECK(std::string(r->driver.status()) ==
          "layout 800 lights, panels 512; "
          "the layout's panel 1 does not run in plain rows of 16: lay the panels out in cabling order, each in plain rows with snake off");
}

TEST_CASE("Hub75PioOutput warns of lights past the last whole panel, and judges only the panels it drives") {
    PioRecordGuard guard;
    mm::GridLayout extra;   // 8 lights after one 16x32 panel in plain rows; outlives the rig, whose Layouts holds it
    extra.width = 4;
    extra.height = 2;
    extra.depth = 1;
    auto r = rig(16, 32, 520);
    r->layouts.addChild(&extra);
    r->layouts.applyState();
    r->layer.applyState();
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Warning);
    CHECK(std::string(r->driver.status()) == "layout 520 lights, panels 512");
}

TEST_CASE("Hub75PioOutput dims by lit time and leaves the curve to the correction, the values kept whole") {
    PioRecordGuard guard;
    auto r = rig(16, 32);
    r->open();
    REQUIRE(mm::test::setControlValue(r->driver, "curve", kLinear));
    r->driver.rebuildCorrection(128);
    CHECK_FALSE(r->driver.appliesCurve());
    CHECK(r->driver.dimsByTime());

    r->paint(0, 200, 200, 200);
    r->driver.tick();

    CHECK(mm::platform::hub75PioTestBrightness() == 128);
    CHECK(pixel(0, 0)[0] == 200);   // the engine dims it by time, not the value
}

TEST_CASE("Hub75PioOutput shows a wide profile's red, green and blue, wherever the profile puts them") {
    PioRecordGuard guard;
    const WideProfile wide;
    showsColorsThrough(wide.roles, 32);
}

TEST_CASE("Hub75PioOutput shows a white-first profile's colors, not its white") {
    PioRecordGuard guard;
    const mm::ChannelRole wrgb[4] = {mm::ChannelRole::White, mm::ChannelRole::Red, mm::ChannelRole::Green, mm::ChannelRole::Blue};
    showsColorsThrough(wrgb, 4);
}

TEST_CASE("Hub75PioOutput hands over a layout with fewer lights than its box, the rest dark") {
    PioRecordGuard guard;
    auto r = rig(16, 32, 20);   // a ring or any shape short of its box
    r->open();
    r->paint(19, 7, 8, 9);     // (3, 1), the last light
    r->driver.tick();
    REQUIRE(mm::platform::hub75PioTestFrameCount() == 1);
    CHECK(pixel(3, 1)[0] == 7);
    CHECK(pixel(4, 1)[0] == 0);
}

TEST_CASE("Hub75PioOutput refuses panels another driver drives, and leaves them to it") {
    PioRecordGuard guard;
    auto first = rig(16, 32);
    auto second = rig(16, 32);
    auto drivers = std::make_unique<mm::Drivers>();   // after the rigs, so it goes first
    drivers->addChild(&first->driver);
    drivers->addChild(&second->driver);
    first->open();
    second->open();
    CHECK(second->driver.severity() == mm::MoonModule::Severity::Error);
    second->driver.tick();
    CHECK(mm::platform::hub75PioTestFrameCount() == 0);
    second->driver.release();
    CHECK(mm::platform::hub75PioWidth() == 16);   // still the first driver's
}

TEST_CASE("Hub75PioOutput and the matrix output refuse to share the Pi's header") {
    RecordGuard rgb;
    PioRecordGuard pio;
    // Bare benches, not Rigs: two drivers on one header, each on its own bench.
    auto a = std::make_unique<Hub75Bench>();
    a->grid.width = 16; a->grid.height = 32; a->grid.depth = 1;
    a->layouts.addChild(&a->grid);
    a->wire(512);
    a->driver.defineControls();
    auto& matrix = *static_cast<mm::Hub75MatrixOutput*>(a->driver.output());
    matrix.rows = 32; matrix.cols = 16; matrix.rp1Pio = false;
    auto b = std::make_unique<Hub75Bench>();
    b->grid.width = 16; b->grid.height = 32; b->grid.depth = 1;
    b->layouts.addChild(&b->grid);
    b->wire(512);
    b->driver.defineControls();
    REQUIRE(b->driver.selectBackendForTest(mm::platform::Hub75Backend::Pio));   // Pi PIO now, by backend
    auto drivers = std::make_unique<mm::Drivers>();   // after the benches, so it goes first
    drivers->addChild(&a->driver);
    drivers->addChild(&b->driver);
    a->open();
    REQUIRE(a->driver.severity() == mm::MoonModule::Severity::Status);
    b->open();
    CHECK(b->driver.severity() == mm::MoonModule::Severity::Error);
    CHECK(std::string(b->driver.status()) == "peripheral already in use by another driver: pick a different one");
    CHECK(mm::platform::hub75PioWidth() == 0);
    CHECK(mm::platform::rgbMatrixWidth() == 16);   // still the first driver's
}

TEST_CASE("Hub75PioOutput reopens the panels on every PIO option and on nothing else") {
    PioRecordGuard guard;
    auto r = rig(16, 32);
    for (const char* n : {"rows", "cols", "chain", "bitDepth", "clockEdge",
                          "pixelClock", "latchBlank", "peripheral"}) {
        CAPTURE(n);
        CHECK(r->driver.affectsPrepare(n));
    }
    CHECK_FALSE(r->driver.affectsPrepare("scanRate"));   // no such control on the PIO card: the scan derives from rows
    CHECK_FALSE(r->driver.affectsPrepare("curve"));
    CHECK_FALSE(r->driver.affectsPrepare("localBrightness"));
    CHECK_FALSE(r->driver.affectsPrepare("shown"));
}

TEST_CASE("Hub75PioOutput names why the panels would not open, and stays inert") {
#ifdef MM_HUB75_PIO_HAVE_PIOLIB
    return;   // where piolib is linked the open would reach it and light real panels, so the case is a piolib-less host's alone
#else
    PioRecordGuard guard;
    auto r = rig(16, 32);
    mm::platform::setTestHub75Pio(false);   // a fit geometry on a host without piolib: the seam refuses the open
    r->driver.prepare();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Error);
    CHECK(std::string(r->driver.status()) == "needs a Raspberry Pi build with piolib");
    r->driver.tick();   // must not crash, and has no frame to hand over
    CHECK(mm::platform::hub75PioWidth() == 0);
#endif
}

TEST_CASE("Hub75PioOutput's chain canvas counts in the driver's memory while the panels run, and goes with them") {
    PioRecordGuard guard;
    auto r = panelsRig(2, 1, 16, 32);   // two 16x32 panels in cabling order, one chain: a 1024-light canvas
    r->out().chain = 2;
    r->open();
    REQUIRE(r->driver.severity() == mm::MoonModule::Severity::Status);
    CHECK(r->out().heapBytes() == 1024 * 3);
    CHECK(r->driver.dynamicBytes() == 2 * 1024 * 3);   // the corrected frame and the canvas

    r->driver.release();
    CHECK(r->driver.dynamicBytes() == 0);

    r->open();
    REQUIRE(r->driver.dynamicBytes() == 2 * 1024 * 3);
    BusPort port;   // the card now offers LCD_CAM first and keeps Pi PIO picked
    r->driver.rebuildControls();
    REQUIRE(r->driver.selectedBackend() == mm::platform::Hub75Backend::Pio);
    REQUIRE(r->driver.selectBackendForTest(mm::platform::Hub75Backend::LcdCam));   // the bus output takes over and the PIO output goes, canvas and all
    CHECK(r->driver.dynamicBytes() < 1024 * 3 * 2);
}

TEST_CASE("Hub75PioOutput closes the panels on release") {
    PioRecordGuard guard;
    auto r = rig(16, 32);
    r->open();
    REQUIRE(mm::platform::hub75PioWidth() == 16);
    r->driver.release();
    CHECK(mm::platform::hub75PioWidth() == 0);
}

// Lights run panel by panel in cabling order, so the PIO output puts each where its panel sits on the chain canvas; the expected cells are the placement rule by hand.

TEST_CASE("Hub75Driver on Pi PIO puts the second panel of a chain to the right of the first") {
    PioRecordGuard guard;
    auto r = rig(32, 32);   // 1024 lights: two 16 by 32 panels
    r->out().chain = 2;
    r->paint(0, 200, 0, 0);       // panel 0, first light
    r->paint(15, 0, 200, 0);      // panel 0, last light of its first row
    r->paint(16, 0, 0, 200);      // panel 0, first light of its second row
    r->paint(512, 100, 100, 100); // panel 1, first light
    r->paint(1023, 200, 200, 0);  // panel 1, last light
    r->open();
    r->driver.tick();
    REQUIRE(mm::platform::hub75PioWidth() == 32);
    CHECK(pixel(0, 0)[0] == 200);
    CHECK(pixel(15, 0)[1] == 200);
    CHECK(pixel(0, 1)[2] == 200);
    CHECK(pixel(16, 0)[0] == 100);
    CHECK(pixel(31, 31)[0] == 200);
    CHECK(pixel(31, 31)[1] == 200);
}

TEST_CASE("Hub75Driver on Pi PIO stacks the chains: six panels, two to a chain on three outputs, make a 32 by 96 canvas") {
    PioRecordGuard guard;
    auto r = rig(32, 96);   // 3072 lights: six 16 by 32 panels
    r->out().chain = 2;
    r->paint(1024, 200, 0, 0);     // panel 2: place 0 of chain 1
    r->paint(2560, 0, 200, 0);     // panel 5: place 1 of chain 2
    r->paint(3071, 0, 0, 200);     // the last light of the last panel
    r->open();
    r->driver.tick();
    REQUIRE(mm::platform::hub75PioWidth() == 32);
    REQUIRE(mm::platform::hub75PioHeight() == 96);
    CHECK(pixel(0, 32)[0] == 200);
    CHECK(pixel(16, 64)[1] == 200);
    CHECK(pixel(31, 95)[2] == 200);
}

TEST_CASE("Hub75Driver on Pi PIO leaves a chain of one as it was: light i is canvas pixel i") {
    PioRecordGuard guard;
    auto r = rig(16, 96);   // three 16 by 32 panels, one to a chain
    r->out().chain = 1;
    r->paint(514, 200, 0, 0);
    r->open();
    r->driver.tick();
    CHECK(pixel(2, 32)[0] == 200);   // light 514 is panel 1 (row 32 of the canvas), column 2
}

// The panels follow the layout; Hub75Fit's cases pin the geometry text, these that the output shows it whole and leaves the panels closed.

TEST_CASE("Hub75PioOutput derives the parallel outputs from the layout, and the card has no such control") {
    PioRecordGuard guard;
    auto r = panelsRig(2, 3, 16, 32);   // six 16 by 32 panels
    r->out().chain = 2;
    CHECK(mm::test::controlIndex(r->driver, "parallel") < 0);
    r->open();
    CHECK(mm::platform::hub75PioTestOptions().parallel == 3);
    CHECK(std::string(r->driver.status()) == "32x96, 6-bit, PIO");
}

TEST_CASE("Hub75PioOutput names the fix in full for panels that do not cable into the chain, and opens nothing") {
    PioRecordGuard guard;
    auto r = rig(112, 32);   // seven 16 by 32 panels
    r->out().chain = 2;
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Error);
    const std::string s = r->driver.status();
    CHECK(s.rfind("7 panels cannot be cabled in chains of 2.", 0) == 0);
    CHECK(s.find("Cable all 7: 1 chain of 7") != std::string::npos);
    CHECK(s.find("Keep chains of 2: use 6 panels, 1 spare, as 3 chains") != std::string::npos);
    CHECK(s.size() > 64);   // past 64 bytes, so the status holds a long message whole
    CHECK(mm::platform::hub75PioWidth() == 0);
}

TEST_CASE("Hub75PioOutput says how many chains the board drives when the layout needs more") {
    PioRecordGuard guard;
    auto r = rig(64, 32);   // four 16 by 32 panels, one to a chain: four outputs
    r->out().chain = 1;
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Error);
    CHECK(std::string(r->driver.status()).rfind("4 panels in chains of 1 need 4 chains, but the Pi PIO mapping drives at most 3.", 0) == 0);
    CHECK(mm::platform::hub75PioWidth() == 0);
}

// A raster across chained panels matches every count, so only the walk catches it: the wall shows bands.
TEST_CASE("Hub75PioOutput warns when a raster layout feeds chained panels, naming the panel and the fix, and the panels still run") {
    PioRecordGuard guard;
    auto r = rig(256, 64);   // one raster across two 128 by 64 panels
    r->out().rows = 64;
    r->out().cols = 128;
    r->out().chain = 2;
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Warning);
    CHECK(std::string(r->driver.status()) ==
          "the layout's panel 1 does not run in plain rows of 128: lay the panels out in cabling order, each in plain rows with snake off");
    CHECK(mm::platform::hub75PioWidth() == 256);
}

TEST_CASE("Hub75PioOutput warns when a Panels layout snakes inside its panels, the Panels layout's default") {
    PioRecordGuard guard;
    auto r = panelsRig(2, 1, 16, 32, true);
    r->out().chain = 2;
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Warning);
    CHECK(std::string(r->driver.status()).rfind("the layout's panel 1 does not run in plain rows of 16", 0) == 0);
}

TEST_CASE("Hub75PioOutput quotes the rates from the pixel clock") {
    PioRecordGuard guard;
    auto r = panelsRig(5, 1, 128, 64);   // five 128 by 64 panels, which chains of 2 cannot cable
    r->out().rows = 64;
    r->out().cols = 128;
    r->out().chain = 2;
    r->open();
    CHECK(std::string(r->driver.status()).find("1 chain of 5 (640x64, 81 Hz)") != std::string::npos);

    r->out().pixelClockKhz = 20000;   // twice the clock, twice the rate
    r->open();
    CHECK(std::string(r->driver.status()).find("1 chain of 5 (640x64, 163 Hz)") != std::string::npos);
}
