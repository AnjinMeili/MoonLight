/// @module Hub75Driver
/// @also DriverBase

/// What Hub75Driver asks of its output and of its siblings: the peripheral one sibling drives is refused to the next, and the output decides how brightness and the curve reach the panel.

#include "doctest.h"
#include "light/drivers/Hub75Driver.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"
#include "light/layers/Buffer.h"
#include "light/layers/Layer.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/Layouts.h"
#include "platform/platform.h"

#include <cstring>
#include <memory>
#include <string>
#include "hub75_bench.h"

namespace {

// Two HUB75 drivers under one container on a 16x32 layout, the box the default 1/16 scan pairs into rows; on the heap, since drivers and a layer outgrow a test's stack frame.
struct Siblings {
    mm::MoonModule owner;
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;
    mm::Buffer source;
    mm::Hub75Driver first;
    mm::Hub75Driver second;

    Siblings() {
        grid.width = 16;
        grid.height = 32;
        grid.depth = 1;
        layouts.addChild(&grid);
        layouts.applyState();
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.applyState();
        REQUIRE(source.allocate(16 * 32, 3));
        for (mm::Hub75Driver* d : {&first, &second}) {
            owner.addChild(d);
            d->setLayer(&layer);
            d->setSourceBuffer(&source);
            mm::Correction correction;
            mm::test::rebuildFromPreset(correction, 255, mm::test::PresetOrder::RGB);
            d->correctionForTest() = correction;
            d->applyState();
        }
    }
    // The owner releases a driver before destroying it, which frees what prepare() allocated.
    ~Siblings() {
        for (mm::Hub75Driver* d : {&first, &second}) {
            d->release();
            owner.removeChild(d);
        }
    }
};

bool says(const mm::MoonModule& m, const char* text) {
    return m.status() && std::strstr(m.status(), text) != nullptr;
}

// A driver that answers whether its output applies the curve, as a Hub75Driver forwards its output's answer.
struct CurvingDriver : mm::DriverBase {
    bool outputCurves = true;
    void setSourceBuffer(mm::Buffer* /*buf*/) override {}
    bool appliesCurve() const override { return outputCurves; }
    void pickCurve(mm::Correction::Curve curve) { curveSel_ = static_cast<uint8_t>(curve); }
};

}  // namespace

TEST_CASE("Hub75Driver stays idle and names why while a sibling driver drives its peripheral") {
    BusPort guard;
    auto s = std::make_unique<Siblings>();
    s->first.prepare();
    REQUIRE(s->first.hwBlock() == mm::LedHwBlock::LcdCam);

    s->second.prepare();
    CHECK(says(s->second, "already in use by another driver"));
    CHECK(s->second.severity() == mm::MoonModule::Severity::Error);
    CHECK(s->second.hwBlock() == mm::LedHwBlock::None);
    CHECK(s->first.hwBlock() == mm::LedHwBlock::LcdCam);
}

TEST_CASE("Hub75Driver takes the peripheral once the sibling driving it releases it") {
    BusPort guard;
    auto s = std::make_unique<Siblings>();
    s->first.prepare();
    s->second.prepare();
    REQUIRE(s->second.hwBlock() == mm::LedHwBlock::None);

    s->first.release();
    CHECK(s->first.hwBlock() == mm::LedHwBlock::None);
    s->second.prepare();
    CHECK(says(s->second, "1/16 scan"));
    CHECK(s->second.hwBlock() == mm::LedHwBlock::LcdCam);
}

TEST_CASE("Hub75Driver on the bus shows its plain status when the layout holds more lights than its box") {
    BusPort guard;
    mm::GridLayout overlap;   // a second grid on the first: a 16x32 box holding 1,024 lights
    overlap.width = 16;
    overlap.height = 32;
    overlap.depth = 1;
    auto s = std::make_unique<Siblings>();
    s->layouts.addChild(&overlap);
    s->layouts.applyState();
    s->layer.applyState();
    REQUIRE(s->layer.physicalLightCount() == 1024);

    s->first.prepare();
    CHECK(s->first.severity() == mm::MoonModule::Severity::Status);
    CHECK(std::string(s->first.status()).rfind("16x32, 1/16 scan", 0) == 0);
    s->layouts.removeChild(&overlap);
}

TEST_CASE("Hub75Driver keeps its status readable across a peripheral change that replaces its output") {
    BusPort guard;
    RecordGuard recorder;   // the Pi header offered second, after LCD_CAM
    auto s = std::make_unique<Siblings>();
    s->first.defineControls();   // the card offers both backends, as it does once the API serves it
    s->first.prepare();
    REQUIRE(says(s->first, "1/16 scan"));
    const std::string shown = s->first.status();

    s->first.peripheralSel_ = 1;   // the Pi header
    s->first.rebuildControls();   // as the scheduler does on a peripheral change: deletes the bus output before the next prepare
    REQUIRE(s->first.selectedBackend() == mm::platform::Hub75Backend::RgbMatrix);
    CHECK(std::string(s->first.status()) == shown);   // the driver's copy, still the last prepare's
}

TEST_CASE("Hub75Driver on the bus dims by lit time and leaves the curve to its correction") {
    auto d = std::make_unique<mm::Hub75Driver>();   // on the heap, as a driver outgrows a test's stack frame
    CHECK(d->dimsByTime());
    CHECK_FALSE(d->appliesCurve());
}

TEST_CASE("A driver whose output applies the curve bakes a Linear correction, whatever curve is picked") {
    auto d = std::make_unique<CurvingDriver>();
    d->pickCurve(mm::Correction::Curve::Cie);
    d->rebuildCorrection(255);
    CHECK(d->correction().curve == mm::Correction::Curve::Linear);

    // The picked curve again, once the output leaves the curve to the correction.
    d->outputCurves = false;
    d->rebuildCorrection(255);
    CHECK(d->correction().curve == mm::Correction::Curve::Cie);
    d->release();
}
