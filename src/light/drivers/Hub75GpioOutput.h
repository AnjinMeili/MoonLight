// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "core/util/ScratchBuffer.h"   // the chain canvas, accounted to the driver and freed with it
#include "light/drivers/Hub75Driver.h"   // the orchestrator, whose scan, edge and depth this output reads; it includes this header back, after its class
#include "light/drivers/Hub75Canvas.h"   // where a light of a panel-by-panel layout sits on the chain canvas, and the plain-rows walk
#include "light/drivers/Hub75Color.h"    // the depth range
#include "light/drivers/Hub75Fit.h"      // whether the layout's panels cable into the chain, and the way out when they do not
#include "light/drivers/Hub75Output.h"
#include "light/layouts/Layouts.h"       // the layout's lights, which the plain-rows walk reads
#include "platform/platform.h"
#include "platform/rpi/hub75_gpio_multiplex.h"   // the Stripe scan and driven width: rows/4, cols*2

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mm {

/// Hub75Driver's output on a Raspberry Pi 3/4's header, through the CPU bit-bang + PWM OE pulser engine: it hands the corrected frame to the GPIO seam, behind which a realtime thread clocks the panels.
///
/// NOTE -- the engine behind this output drives the panel columns from the CPU and times OE with the hardware PWM pulser, not with a DMA channel: measured DMA GPIO rates on Pi 4 (~1.5-1.8 MHz of GPIO operations) put a DMA-streamed wall at 15-18 Hz computed, far below the flicker threshold, while the CPU loop sustains ~262 ns per column at slowdown 1 (bench-calibrated; hub75GpioColumnTimeNs); the name says what it does. The design decision, with numbers, is in MOONLIGHT-INTEGRATION.md.
///
/// The wiring is fixed, the active 3-channel board's (hub75_gpio_pins.h): three chains of HUB75 lines on the Pi's header, each chain up to 32 panels, so the card carries no pin controls and no wiring row. The defaults are James's wall: 128x64 panels, 2 to a chain, 3 chains stacking to 256x192 at 1/32 scan and 6-bit, lit at a 130 ns LSB.
///
/// **The layout sets the geometry.** `rows` and `cols` are one panel's, `chain` the panels cabled in series, and the layout's lights, cut into whole panels, are the panels. `parallel` follows, bounded by the board's three chains. The scan follows the rows, rows/2, as the matrix output's does -- a 64-row panel is 1/32, a 32-row one 1/16 -- so there is no scan control to leave at the wrong rate: the Pi 5 wall came up with black stripes from a 1/16 default on 1/32 panels (NOTE-2), and this output cannot repeat it. Under Stripe the scan is the driven panel's, rows/4 (see below).
///
/// **Multiplexing.** An outdoor panel whose rows are wired through an internal multiplexer takes `multiplexing` Stripe, which reshapes the frame before the encoder (hub75_gpio_multiplex.h): stretch factor 2, so a 32x64 panel is driven as 16 rows x 128 columns, rows 0..7 and 16..23 folding to columns 64..127, at 1/8 scan. The mapping is hzeller's Stripe algorithm, reimplemented from the algorithm with attribution -- see that header's license caveat -- and BENCH-V5 verified it on the Pi 4 sign: the test pattern renders right, red rows top to bottom, green columns left to right, held row 0 at top. The mapping is one entry of the multiplex table there, normal first and Stripe second; a further mapping is a further entry.
/// @attribution rpi-rgb-led-matrix (the Stripe mapping)
///
/// **The chain canvas.** The seam's canvas is `cols * chain` wide while the layout's lights run panel by panel, so with a chain each panel row is copied to its place before the push; a layout of another count, or off plain rows, is the warning.
///
/// **The refresh.** A realtime thread clocks one GPIO word a column at ~262 ns a column (slowdown 1, 288 ns at slowdown 3 -- the delay loops dominate the column: each loop's first GPLEV0 read costs ~107 ns, further reads in the loop ~6.4 ns each; hub75GpioColumnTimeNs) while the PWM pulser times OE, so the frame is `bitDepth * scanRows * chainWidth` columns plus the dark intervals: ~77 Hz computed on the wall at 6-bit and slowdown 1, ~73 Hz at the Pi 4's auto slowdown 2 -- computed, not measured, until the bench reads the `shown` rate on the wall, and below the flicker threshold at 6-bit, so the wall stays on the PIO path. Under Stripe the chain width doubles and the scan rows halve, so the pixel clocks stay the same and only the dark intervals halve: 612 / 584 / 558 Hz predicted on the sign at 6-bit, true slowdowns 1 / 2 / 3, 160 ns LSB, against 608-616 / 584-585 / 555-560 fps measured (BENCH-V6) -- the model is bench-calibrated on the sign within +-3%, after the v5 calibration (22 ns plus 82 ns a slowdown unit) was falsified by reading the card's slowdown index as the slowdown. A Pi 3 reads lower and has no bench behind it yet. The arrangement rates quote that column time and the LSB time, since a long LSB makes the top planes OE-bound instead of shift-bound.
///
/// **Brightness and the curve.** The engine dims by lit time, so the correction keeps full values and bakes the curve itself; the seam's brightness is the global level alone.
class Hub75GpioOutput : public Hub75Output {
public:
    /// The chain canvas is `owner`'s memory, counted in its readout and freed on its release.
    explicit Hub75GpioOutput(MoonModule& owner) : canvas_(owner) {}

    /// The GPIO seam's own defaults, the one home every setting below starts from.
    static constexpr platform::Hub75GpioOptions kDefaults{};
    /// The chains the board drives: three, one per HUB75 line set.
    static constexpr uint8_t kParallelMax = 3;
    /// The GPIO slowdown: auto takes the engine's own choice for the model (Pi 4: 2, Pi 3: 1), and 0 to 4 slows the column writes for a panel that wants more setup time.
    static constexpr uint8_t kSlowdownCount = 6;
    static constexpr const char* kSlowdownOptions[kSlowdownCount] = {"auto", "0", "1", "2", "3", "4"};
    /// An outdoor panel's scan wiring: normal is direct, Stripe drives the panel stretched (half the rows, double the columns), as --led-multiplexing does. The options mirror the multiplex table's order (normal first, Stripe second); the count is asserted against the table, so a further entry fails here until the control names it.
    static constexpr uint8_t kMultiplexingCount = 2;
    static constexpr const char* kMultiplexingOptions[kMultiplexingCount] = {"normal", "Stripe"};
    static_assert(kMultiplexingCount == platform::hub75GpioMultiplexCount(),
                  "the multiplexing control must name every table entry");

    uint8_t  rows = kDefaults.rows;                    ///< The rows of one panel
    uint16_t cols = kDefaults.cols;                    ///< The columns of one panel
    uint8_t  chain = kDefaults.chain;                  ///< Panels daisy-chained on one output
    uint8_t  multiplexingSel = kDefaults.multiplexing; ///< An outdoor panel's scan wiring: Stripe stretches the panel (half the rows, double the columns), as --led-multiplexing=1 does
    uint16_t pwmLsbNs = kDefaults.pwmLsbNs;            ///< The shortest plane's lit time in ns; the PWM quantises it to a multiple of 4 ns
    uint8_t  slowdownSel = 0;                          ///< Slows the GPIO for a fast Pi; auto is the engine's per model. The select's INDEX, not the slowdown: 0=auto, 1=slowdown 0, ..., 5=slowdown 4 -- see slowdownRequested() and the slowdownTrue readout.

    /// The planes the engine drives: 2 to 6, starting at 6.
    DepthRange depthRange() const override { return DepthRange{2, 6, 6}; }

    /// The panel options and the frames shown each second, below the peripheral select.
    void addControls(ControlList& controls) override {
        controls.addControl("rows", rows, 8, 64);       // 8 is one row address at 1/8 scan; 64 is the panel the board is built for
        controls.addControl("cols", cols, 16, 512);     // 16 is the narrowest panel; 512 by kChainMax keeps the canvas width, 16,384, inside 16 bits
        controls.addControl("chain", chain, 1, kChainMax);
        controls.addSelect("multiplexing", multiplexingSel, kMultiplexingOptions, kMultiplexingCount);
        controls.addControl("bitDepth", owner_->bitDepth, depthRange().min, depthRange().max);
        controls.addSelect("clockEdge", owner_->clockEdgeSel, Hub75Driver::kEdgeOptions, Hub75Driver::kEdgeCount);
        controls.addControl("pwmLsbNs", pwmLsbNs, 50, 3000);   // 50 to 3000 ns: shorter is faster and thinner at the low end, longer steadies a dim panel
        controls.addSelect("slowdown", slowdownSel, kSlowdownOptions, kSlowdownCount);
        /// The slowdown the engine runs, resolved through the seam (auto takes the model's own): the card stores the select's INDEX, so a stored 3 is slowdown 2 -- this readout quotes the resolved value, so a bench note cannot misread the index again (BENCH-V6).
        controls.addReadOnly("slowdownTrue", slowdownTrueStr_, sizeof(slowdownTrueStr_));
        /// The frames the panels showed in the last second.
        controls.addReadOnly("shown", shownStr_, sizeof(shownStr_));
    }

    /// Every GPIO option reopens the panels; the read-only rate does not. The scan is not listed: it follows the rows, and rows reopens.
    bool affectsPrepare(const char* name) const override {
        static const char* const kReopen[] = {
            "rows", "cols", "chain", "multiplexing", "bitDepth", "clockEdge", "pwmLsbNs", "slowdown",
        };
        for (const char* n : kReopen) {
            if (std::strcmp(name, n) == 0) return true;
        }
        return false;
    }

    /// The panel geometry this output drives, carried across an output swap so a backend change keeps the panels' rows, columns and chain.
    bool panelGeometry(PanelGeometry& geometry) const override {
        geometry = PanelGeometry{rows, cols, chain};
        return true;
    }
    /// Take a panel geometry carried across an output swap.
    void setPanelGeometry(const PanelGeometry& geometry) override {
        rows = geometry.rows;
        cols = geometry.cols;
        chain = geometry.chain;
    }

    /// The options as the platform takes them.
    platform::Hub75GpioOptions options() const {
        platform::Hub75GpioOptions o;
        o.rows = rows;
        o.cols = cols;
        o.chain = chain;
        o.parallel = derivedParallel_ ? derivedParallel_ : 1;   // the layout decides it, in open()
        o.scanRate = scanRate();
        o.multiplexing = multiplexingSel;
        o.bitDepth = owner_ ? owner_->bitDepth : depthRange().initial;   // a detached output answers its range's starting depth
        o.pwmLsbNs = pwmLsbNs;
        o.slowdown = slowdownRequested();
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
        // The rate counts the driven address steps and the driven columns: rows/2 steps of cols a row direct, rows/4 steps of cols*2 a row under Stripe -- the pixel clocks stay the same, the dark intervals halve.
        const uint16_t scanRows = scanRate();
        const uint16_t drivenCols = platform::hub75GpioMultiplexDrivenCols(multiplexingSel, cols);
        // The arrangement rate is the column time's own: one column step per pixel at the engine's model through the seam (hub75GpioPredictedColumnTimeNs -- 249 ns plus 13 ns a slowdown unit, bench-calibrated), auto quoting the slowdown the engine resolves for this host, and 0.5 us of dark interval per row for the address + latch. A long LSB makes the top planes OE-bound instead, which the rate formula counts.
        const uint8_t effSlowdown = static_cast<uint8_t>(platform::hub75GpioResolvedSlowdown(slowdownRequested()));
        const uint16_t shiftTenths = static_cast<uint16_t>(platform::hub75GpioPredictedColumnTimeNs(effSlowdown) * 10);
        // The dump skips the encoder's hold words (the pulser, not the encoder's OE bits, sets the lit time), so the rate counts the shift words alone -- no encoderHold.
        const CardGeometry geometry{{rows, cols, scanRows, bits, pwmLsbNs, 0, 0, shiftTenths, 5000, drivenCols}, chain, kParallelMax, "Pi GPIO"};
        const GeometryFit fit = checkGeometry(panels, chain, kParallelMax);
        message_[0] = '\0';
        if (fit != GeometryFit::Ok) {
            geometryMessage(fit, panels, geometry, message_, sizeof(message_));
            return message_;
        }
        derivedParallel_ = static_cast<uint8_t>(panels / chain);

        const platform::Hub75GpioOptions o = options();
        if (!platform::hub75GpioOpen(o)) {
            const char* why = platform::hub75GpioLastError();
            return why ? why : "the panels would not open";
        }
        open_ = true;
        canvasW_ = platform::hub75GpioWidth();
        canvasH_ = platform::hub75GpioHeight();
        placed_ = {rows, cols, chain};
        // A chain is not a raster: the layout's lights run panel by panel, so the push reads a second frame they are placed on.
        if (chain > 1 && !canvas_.resize(static_cast<size_t>(lights()) * 3)) {
            close();
            return "not enough memory for the chain canvas";
        }
        std::snprintf(status_, sizeof(status_), "%ux%u, %u-bit, GPIO", static_cast<unsigned>(canvasW_), static_cast<unsigned>(canvasH_),
                      static_cast<unsigned>(o.bitDepth));
        std::snprintf(slowdownTrueStr_, sizeof(slowdownTrueStr_), "%u", static_cast<unsigned>(effSlowdown));
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
        platform::hub75GpioCorrection(brightness);
        if (canvas_.bytes() == 0) {
            platform::hub75GpioPush(rgb);
            return;
        }
        placeOnCanvas(rgb);
        platform::hub75GpioPush(canvas_.data());
    }

    /// Publish the frames shown in the last second.
    void tick1s() MM_NONBLOCKING override {
        if (!open_) return;
        const uint16_t fps = platform::hub75GpioFps();
        if (fps == lastShown_) return;   // a changed value alone re-serializes on the 1 Hz tick
        lastShown_ = fps;
        mm::formatTo(shownStr_, sizeof(shownStr_), "%u fps", fps);
    }

    /// Stop the refresh and darken the panels.
    void close() override {
        if (open_) platform::hub75GpioClose();
        open_ = false;
        canvasW_ = canvasH_ = 0;
        canvas_.resize(0);
        shownStr_[0] = '\0';   // a closed panel shows an empty rate
        slowdownTrueStr_[0] = '\0';   // and no slowdown
        lastShown_ = 0xFFFF;
    }

    /// The chain canvas, when the panels form a chain.
    size_t heapBytes() const override { return canvas_.bytes(); }

    /// The Pi's header, which one HUB75 driver per Pi drives; the matrix and PIO outputs claim the same block, so the three never drive the header together.
    LedHwBlock hwBlock() const override { return LedHwBlock::RgbMatrix; }
    /// The engine dims by lit time, so the correction keeps full values.
    bool dimsByTime() const MM_NONBLOCKING override { return true; }
    /// The engine applies no curve, so the correction bakes the selected one.
    bool appliesCurve() const override { return false; }

private:
    /// The slowdown the card asks for: -1 for auto, else 0..4. The engine resolves auto by model at start(); the rate prediction resolves it the same way through the seam, so both quote one slowdown. The card stores the select's INDEX -- slowdownSel 3 is slowdown 2 -- and the slowdownTrue readout publishes the resolved value, so a bench note quotes the slowdown the engine runs (BENCH-V6 misread the index as the slowdown and falsified the v5 rate calibration).
    int8_t slowdownRequested() const {
        return slowdownSel == 0 || slowdownSel >= kSlowdownCount ? -1 : static_cast<int8_t>(slowdownSel - 1);
    }

    /// The scan the rows wire: rows/2, as the matrix output derives it -- a 64-row panel is 1/32, a 32-row one 1/16 -- or rows/4 under Stripe, the driven panel's scan: a 32-row Stripe panel is 1/8. There is no scan control, so the wall cannot come up striped from a stale default; a rows value whose scan is not 8, 16 or 32 is refused by the engine with its reason.
    uint8_t scanRate() const {
        return platform::hub75GpioMultiplexScanRate(multiplexingSel, rows);
    }

    /// Append to the seam's warning, each part whole and joined with "; " as the seam joins its own, a light count other than the canvas's, then the walk's panel.
    void warnFit(uint32_t layoutLights) {
        detail::Text t{message_, sizeof(message_)};
        if (const char* seam = platform::hub75GpioWarning()) t.add("%s", seam);
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
    char     slowdownTrueStr_[4] = "";   ///< the resolved slowdown as one digit, empty while closed
    char     status_[32] = {};   ///< the canvas and the depth, at most "65535x65535, 6-bit, GPIO"
    Placed   placed_;
    ScratchBuffer<uint8_t> canvas_;   ///< the placed frame of a chain, empty for one panel per output, whose lights are already a raster
};

/// Make the GPIO output, which Hub75Driver makes only where `hasHub75Gpio` holds, so an ESP32 image links none of it.
inline Hub75Output* makeHub75GpioOutput(Hub75Driver& owner) { return new Hub75GpioOutput(owner); }

} // namespace mm
