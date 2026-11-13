// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "core/util/ScratchBuffer.h"   // the chain canvas, accounted to the driver and freed with it
#include "light/drivers/Hub75Driver.h"   // the orchestrator, whose edge and depth this output reads; it includes this header back, after its class
#include "light/drivers/Hub75Canvas.h"   // where a light of a panel-by-panel layout sits on the chain canvas, and the plain-rows walk
#include "light/drivers/Hub75Color.h"    // the depth range
#include "light/drivers/Hub75Fit.h"      // whether the layout's panels cable into the chain, and the way out when they do not
#include "light/drivers/Hub75Output.h"
#include "light/layouts/Layouts.h"       // the layout's lights, which the plain-rows walk reads
#include "platform/platform.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mm {

/// Hub75Driver's output on a Raspberry Pi 5's header, through the RP1's PIO: it hands the corrected frame to the PIO seam, behind which a state machine clocks the panels.
///
/// The wiring is fixed, the active 3-channel board's (hub75_pio_pins.h): three chains of HUB75 lines on the Pi's header, each chain up to 32 panels, so the card carries no pin controls and no wiring row. The defaults are James's wall: 128x64 panels, 2 to a chain, 3 chains stacking to 256x192 at 1/32 scan and 6-bit, clocked at 10 MHz.
///
/// **The layout sets the geometry.** `rows` and `cols` are one panel's, `chain` the panels cabled in series, and the layout's lights, cut into whole panels, are the panels. `parallel` follows, bounded by the board's three chains.
///
/// **The scan derives from `rows`.** As on the matrix output, the scan is rows / 2 — the card carries no `scanRate` control — so a 64-row wall is 1/32 without a hand setting. (The card used to read the driver's shared select, whose 1/16 default striped the wall until the scan was set over the API; bench 2026-11-13.)
///
/// **The chain canvas.** The seam's canvas is `cols * chain` wide while the layout's lights run panel by panel, so with a chain each panel row is copied to its place before the push; a layout of another count, or off plain rows, is the warning.
///
/// **The refresh.** A state machine shifts one wide word a pixel clock, so the frame is `bitDepth * scanRows * chainWidth` clocks: 203 Hz on the wall at 10 MHz. The arrangement rates quote that pixel clock, not the library's shift, since the latch rides the last pixel clock (Hub75Canvas.h).
///
/// **Brightness and the curve.** The engine dims by lit time, so the correction keeps full values and bakes the curve itself; the seam's brightness is the global level alone.
class Hub75PioOutput : public Hub75Output {
public:
    /// The chain canvas is `owner`'s memory, counted in its readout and freed on its release.
    explicit Hub75PioOutput(MoonModule& owner) : canvas_(owner) {}

    /// The PIO seam's own defaults, the one home every setting below starts from.
    static constexpr platform::Hub75PioOptions kDefaults{};
    /// The chains the board drives: three, one per HUB75 line set.
    static constexpr uint8_t kParallelMax = 3;

    uint8_t  rows = kDefaults.rows;                    ///< The rows of one panel
    uint16_t cols = kDefaults.cols;                    ///< The columns of one panel
    uint8_t  chain = kDefaults.chain;                  ///< Panels daisy-chained on one output
    uint16_t pixelClockKhz = static_cast<uint16_t>(kDefaults.pixelClockHz / 1000);   ///< The HUB75 pixel clock in kHz; 10,000 is 10 MHz
    uint8_t  latchBlankWords = kDefaults.latchBlankWords;   ///< Blank words the latch holds, 0 for none

    /// The planes the state machine drives: 2 to 6, starting at 6.
    DepthRange depthRange() const override { return DepthRange{2, 6, 6}; }

    /// The panel options and the frames shown each second, below the peripheral select.
    void addControls(ControlList& controls) override {
        controls.addControl("rows", rows, 8, 64);       // 8 is one row address at 1/8 scan; 64 is the panel the board is built for
        controls.addControl("cols", cols, 16, 512);     // 16 is the narrowest panel; 512 by kChainMax keeps the canvas width, 16,384, inside 16 bits
        controls.addControl("chain", chain, 1, kChainMax);
        controls.addControl("bitDepth", owner_->bitDepth, depthRange().min, depthRange().max);
        controls.addSelect("clockEdge", owner_->clockEdgeSel, Hub75Driver::kEdgeOptions, Hub75Driver::kEdgeCount);
        controls.addControl("pixelClock", pixelClockKhz, 1000, 40000);   // 1 to 40 MHz: below 1 MHz the wall crawls, above 40 the divider bottoms out
        controls.addControl("latchBlank", latchBlankWords, 0, 8);   // a few words steady a long ribbon; more only dims the panel
        /// The frames the panels showed in the last second.
        controls.addReadOnly("shown", shownStr_, sizeof(shownStr_));
    }

    /// Every PIO option reopens the panels; the read-only rate does not.
    bool affectsPrepare(const char* name) const override {
        static const char* const kReopen[] = {
            "rows", "cols", "chain", "bitDepth", "clockEdge", "pixelClock", "latchBlank",
        };
        for (const char* n : kReopen) {
            if (std::strcmp(name, n) == 0) return true;
        }
        return false;
    }

    /// The options as the platform takes them.
    platform::Hub75PioOptions options() const {
        platform::Hub75PioOptions o;
        o.rows = rows;
        o.cols = cols;
        o.chain = chain;
        o.parallel = derivedParallel_ ? derivedParallel_ : 1;   // the layout decides it, in open()
        o.scanRate = scanRate();
        o.bitDepth = owner_ ? owner_->bitDepth : depthRange().initial;   // a detached output answers its range's starting depth
        o.pixelClockHz = static_cast<uint32_t>(pixelClockKhz) * 1000;
        o.latchBlankWords = latchBlankWords;
        o.clockFallingEdge = owner_ && owner_->clockEdgeSel == Hub75Driver::kEdgeFalling;
        return o;
    }

    /// Open the panels; the canvas they form is the frame, whatever the layout's box, and warnFit() compares the light counts.
    const char* open(uint16_t /*width*/, uint16_t /*height*/) override {
        close();
        // A depth loaded from storage can sit outside the range, since a load sets values and skips the control change.
        if (owner_) owner_->bitDepth = std::clamp(owner_->bitDepth, depthRange().min, depthRange().max);
        const uint8_t bits = owner_ ? owner_->bitDepth : depthRange().initial;

        // The layout sets the geometry: its lights, cut into whole panels, are the panels, and the card's chain divides them into outputs.
        const uint32_t perPanel = static_cast<uint32_t>(rows) * cols;
        const uint32_t layoutLights = owner_ && owner_->layer() ? owner_->layer()->physicalLightCount() : 0;
        const uint32_t panels = perPanel ? layoutLights / perPanel : 0;
        // The rate counts address steps, rows / 2: the encoder's scan rows times its pairs, one shift of the chain each.
        const uint16_t scanRows = rows > 1 ? static_cast<uint16_t>(rows / 2) : 1;
        // The arrangement rate is the pixel clock's own: the shift is one clock a pixel, the latch rides the last one, and no lit time outlasts a row's shift.
        const uint16_t shiftTenths = pixelClockKhz ? static_cast<uint16_t>(10'000'000u / pixelClockKhz) : 10000;
        const CardGeometry geometry{{rows, cols, scanRows, bits, 0, 0, 0, shiftTenths, 0}, chain, kParallelMax, "Pi PIO"};
        const GeometryFit fit = checkGeometry(panels, chain, kParallelMax);
        message_[0] = '\0';
        if (fit != GeometryFit::Ok) {
            geometryMessage(fit, panels, geometry, message_, sizeof(message_));
            return message_;
        }
        derivedParallel_ = static_cast<uint8_t>(panels / chain);

        const platform::Hub75PioOptions o = options();
        if (!platform::hub75PioOpen(o)) {
            const char* why = platform::hub75PioLastError();
            return why ? why : "the panels would not open";
        }
        open_ = true;
        canvasW_ = platform::hub75PioWidth();
        canvasH_ = platform::hub75PioHeight();
        placed_ = {rows, cols, chain};
        // A chain is not a raster: the layout's lights run panel by panel, so the push reads a second frame they are placed on.
        if (chain > 1 && !canvas_.resize(static_cast<size_t>(lights()) * 3)) {
            close();
            return "not enough memory for the chain canvas";
        }
        std::snprintf(status_, sizeof(status_), "%ux%u, %u-bit, PIO", static_cast<unsigned>(canvasW_), static_cast<unsigned>(canvasH_),
                      static_cast<unsigned>(o.bitDepth));
        warnFit(layoutLights);
        return nullptr;
    }

    /// The canvas and the depth, as open() formatted them.
    const char* describe() const override { return status_; }

    /// What degrades the running panels, in full, none hiding another: the seam's warning, then a light count other than the canvas's, then the walk's panel.
    const char* warning() const override { return message_[0] ? message_ : nullptr; }

    /// The panels' canvas, one light per pixel.
    nrOfLightsType lights() const MM_NONBLOCKING override {
        return open_ ? static_cast<nrOfLightsType>(canvasW_) * canvasH_ : 0;
    }

    /// Hand the frame over, at the global brightness the engine dims by.
    void show(const uint8_t* rgb, uint8_t brightness) MM_NONBLOCKING override {
        platform::hub75PioCorrection(brightness);
        if (canvas_.bytes() == 0) {
            platform::hub75PioPush(rgb);
            return;
        }
        placeOnCanvas(rgb);
        platform::hub75PioPush(canvas_.data());
    }

    /// Publish the frames shown in the last second.
    void tick1s() MM_NONBLOCKING override {
        if (!open_) return;
        const uint16_t fps = platform::hub75PioFps();
        if (fps == lastShown_) return;   // a changed value alone re-serializes on the 1 Hz tick
        lastShown_ = fps;
        mm::formatTo(shownStr_, sizeof(shownStr_), "%u fps", fps);
    }

    /// Stop the refresh and darken the panels.
    void close() override {
        if (open_) platform::hub75PioClose();
        open_ = false;
        canvasW_ = canvasH_ = 0;
        canvas_.resize(0);
        shownStr_[0] = '\0';   // a closed panel shows an empty rate
        lastShown_ = 0xFFFF;
    }

    /// The chain canvas, when the panels form a chain.
    size_t heapBytes() const override { return canvas_.bytes(); }

    /// The Pi's header, which one HUB75 driver per Pi drives; the matrix output claims the same block, so the two never drive the header together.
    LedHwBlock hwBlock() const override { return LedHwBlock::RgbMatrix; }
    /// The engine dims by lit time, so the correction keeps full values.
    bool dimsByTime() const MM_NONBLOCKING override { return true; }
    /// The engine applies no curve, so the correction bakes the selected one.
    bool appliesCurve() const override { return false; }

private:
    /// The scan rate the panel's rows imply: rows / 2, as the matrix
    /// output derives it — 64 rows is 1/32, 32 rows 1/16, 16 rows 1/8.
    /// A rows count whose half is not 8, 16 or 32 is refused at open by
    /// the engine, with the reason in the status line.
    uint8_t scanRate() const {
        return rows > 1 ? static_cast<uint8_t>(rows / 2) : 1;
    }

    /// Append to the seam's warning, each part whole and joined with "; " as the seam joins its own, a light count other than the canvas's, then the walk's panel.
    void warnFit(uint32_t layoutLights) {
        detail::Text t{message_, sizeof(message_)};
        if (const char* seam = platform::hub75PioWarning()) t.add("%s", seam);
        if (layoutLights != lights())
            t.add("%slayout %u lights, panels %u", t.n ? "; " : "", static_cast<unsigned>(layoutLights), static_cast<unsigned>(lights()));
        const uint32_t off = offPanel();
        if (off != PanelWalk::kNone)
            t.add("%sthe layout's panel %u does not run in plain rows of %u: lay the panels out in cabling order, each in plain rows with snake off",
                  t.n ? "; " : "", static_cast<unsigned>(off + 1), static_cast<unsigned>(placed_.cols));
    }

    /// The first driven panel, counted from 0, whose lights leave plain rows, or PanelWalk::kNone; lights past the last whole panel are the count's.
    uint32_t offPanel() const {
        Layouts* layouts = owner_ && owner_->layer() ? owner_->layer()->layouts() : nullptr;
        if (!layouts) return PanelWalk::kNone;
        struct Driven { PanelWalk walk; nrOfLightsType lights; };
        Driven driven{PanelWalk(placed_.rows, placed_.cols), lights()};
        layouts->placeLights(CoordSink{[](void* c, nrOfLightsType i, lengthType x, lengthType y, lengthType) {
                                           auto* d = static_cast<Driven*>(c);
                                           if (i < d->lights) d->walk.light(i, x, y);
                                       },
                                       nullptr, &driven});
        return driven.walk.firstOffPanel();
    }

    /// Copy the corrected frame onto the canvas a panel row at a time: a panel's row is contiguous in both, so each row is one blit.
    void placeOnCanvas(const uint8_t* rgb) MM_NONBLOCKING {
        const size_t width = static_cast<size_t>(placed_.cols) * placed_.chain;
        const size_t rowBytes = static_cast<size_t>(placed_.cols) * 3;
        const nrOfLightsType n = lights();
        for (nrOfLightsType i = 0; i + placed_.cols <= n; i += placed_.cols) {
            const CanvasPos p = chainCanvasPos(i, placed_.rows, placed_.cols, placed_.chain);
            const size_t to = (static_cast<size_t>(p.y) * width + p.x) * 3;
            if (to + rowBytes > canvas_.bytes()) continue;
            std::memcpy(canvas_.data() + to, rgb + static_cast<size_t>(i) * 3, rowBytes);
        }
    }

    /// One panel and its chain as the panels were opened, so a control changed before the reopen moves no row.
    struct Placed {
        uint16_t rows = 0;
        uint16_t cols = 0;
        uint8_t  chain = 0;
    };

    bool     open_ = false;
    uint8_t  derivedParallel_ = 0;   ///< the outputs the layout's panels divide into, set by open()
    char     message_[kGeometryMessageMax] = "";   ///< the way out of a geometry that does not fit, or the running panels' warnings, whole
    uint16_t canvasW_ = 0;
    uint16_t canvasH_ = 0;
    uint16_t lastShown_ = 0xFFFF;   // not 0: the first real 0 must still publish
    char     shownStr_[16] = "";
    char     status_[32] = {};   ///< the canvas and the depth, at most "65535x65535, 6-bit, PIO"
    Placed   placed_;
    ScratchBuffer<uint8_t> canvas_;   ///< the placed frame of a chain, empty for one panel per output, whose lights are already a raster
};

/// Make the PIO output, which Hub75Driver makes only where `hasHub75Pio` holds, so an ESP32 image links none of it.
inline Hub75Output* makeHub75PioOutput(Hub75Driver& owner) { return new Hub75PioOutput(owner); }

} // namespace mm
