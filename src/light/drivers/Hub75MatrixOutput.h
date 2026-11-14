#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "core/util/ScratchBuffer.h"   // the chain canvas, accounted to the driver and freed with it
#include "light/drivers/Hub75Driver.h"   // the orchestrator, whose curve this output reads; it includes this header back, after its class
#include "light/drivers/Hub75Canvas.h"   // where a light of a panel-by-panel layout sits on the chain canvas, and the plain-rows walk
#include "light/drivers/Hub75Color.h"   // who owns the color map, and the depth range that follows
#include "light/drivers/Hub75Fit.h"      // whether the layout's panels cable into the chain, and the way out when they do not
#include "light/drivers/Hub75Output.h"
#include "light/layouts/Layouts.h"       // the layout's lights, which the plain-rows walk reads
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

namespace mm {

/// Hub75Driver's output on a Raspberry Pi's header: it hands the corrected frame to the RGB seam, behind which rpi-rgb-led-matrix times the panels.
///
/// Each control is one of the library's options under its own name, so a working `--led-...` command line reads straight across: `--led-rows=64` is `rows` 64. The settings live here and default to the library's, through `RgbMatrixOptions`, but for `rp1Pio`, on where the kernel runs PIO, and the depth, 8.
///
/// **The layout sets the geometry.** `rows` and `cols` are one panel's, `chain` the panels cabled in series, and the layout's lights, cut into whole panels, are the panels. `parallel` follows, bounded by the mapping.
///
/// **The chain canvas.** The library's canvas is `cols * chain` wide while the layout's lights run panel by panel, so with a chain each panel row is copied to its place before the push; a layout of another count, or off plain rows, is the warning.
///
/// **Brightness, the curve and the depth.** CIE 1931 is the library's own table, up to 11 bits; any other curve is MoonLight's and the library runs direct. The Pi renders at 8: 9 to 11 are offered only on CIE 1931.
class Hub75MatrixOutput : public Hub75Output {
public:
    /// The chain canvas is `owner`'s memory, counted in its readout and freed on its release.
    explicit Hub75MatrixOutput(MoonModule& owner) : canvas_(owner) {}

    /// The library's GPIO mappings for a 64-bit Pi, in its own order; the -pi1 ones fit a Pi 1, which runs 32-bit builds alone, and compute-module needs a build switch its default build leaves off.
    static constexpr uint8_t kMappingCount = 4;
    static constexpr const char* kMappingOptions[kMappingCount] = {"regular", "adafruit-hat", "adafruit-hat-pwm", "classic"};
    /// Driver chips that need an init sequence; none for most panels.
    static constexpr uint8_t kPanelTypeCount = 3;
    static constexpr const char* kPanelTypeOptions[kPanelTypeCount] = {"none", "FM6126A", "FM6127"};
    /// The library's multiplexers, by its numbering.
    static constexpr uint8_t kMultiplexingCount = 25;
    static constexpr const char* kMultiplexingOptions[kMultiplexingCount] = {
        "normal", "Stripe", "Checkered", "Spiral", "ZStripe", "ZnMirrorZStripe", "coreman", "Kaler2Scan",
        "ZStripeUneven", "P10-128x4-Z", "QiangLiQ8", "InversedZStripe", "P10Outdoor1R1G1-1",
        "P10Outdoor1R1G1-2", "P10Outdoor1R1G1-3", "P10CoremanMapper", "P8Outdoor1R1G1", "FlippedStripe",
        "P10Outdoor32x16HalfScan", "P10Outdoor32x16QuarterScanMapper", "P3Outdoor64x64MultiplexMapper",
        "DoubleZ", "P4Outdoor80x40Mapper", "ZnMirrorZStripe40x20", "P10Outdoor1R1G1-2-40x20"};
    /// How rows are addressed, by the library's numbering.
    static constexpr uint8_t kRowAddrCount = 6;
    static constexpr const char* kRowAddrOptions[kRowAddrCount] = {
        "default", "AB-addressed", "direct row select", "ABC-addressed", "ABC shift + DE direct", "ABC direct"};
    /// Progressive or interlaced, by the library's numbering.
    static constexpr uint8_t kScanModeCount = 2;
    static constexpr const char* kScanModeOptions[kScanModeCount] = {"progressive", "interlaced"};
    /// The GPIO slowdown: auto takes the library's own choice for the model, and 0 to 10 is the library's documented range (a Pi 4 on some panels needs 8).
    static constexpr uint8_t kSlowdownCount = 12;
    static constexpr const char* kSlowdownOptions[kSlowdownCount] = {"auto", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10"};

    /// The library's own defaults, the one home every setting below starts from.
    static constexpr platform::RgbMatrixOptions kDefaults{};

    uint8_t  mappingSel = 0;                           ///< Which header pins carry which line, `--led-gpio-mapping`
    uint8_t  panelTypeSel = 0;                         ///< A driver chip needing an init sequence, such as FM6126A, `--led-panel-type`
    uint8_t  rows = kDefaults.rows;                    ///< The rows of one panel, `--led-rows`
    uint16_t cols = kDefaults.cols;                    ///< The columns of one panel, `--led-cols`
    uint8_t  chain = kDefaults.chain;                  ///< Panels daisy-chained on one output, `--led-chain`
    uint8_t  multiplexingSel = kDefaults.multiplexing; ///< An outdoor panel's scan wiring, `--led-multiplexing`
    uint8_t  rowAddrSel = kDefaults.rowAddrType;       ///< How rows are addressed, `--led-row-addr-type`
    uint8_t  scanModeSel = kDefaults.scanMode;         ///< Progressive or interlaced, `--led-scan-mode`
    uint16_t pwmLsbNs = kDefaults.pwmLsbNs;            ///< The shortest lit time in nanoseconds, `--led-pwm-lsb-nanoseconds`
    uint8_t  ditherBits = kDefaults.ditherBits;        ///< Shorter lit time, thinner low planes: faster, dimmer, `--led-pwm-dither-bits`
    uint8_t  slowdownSel = 0;                          ///< Slows the GPIO for a fast Pi; auto is the library's per model, `--led-slowdown-gpio`
    bool     rp1Pio = platform::rgbMatrixPioRefusal() == nullptr;   ///< A Pi 5 clocks through PIO rather than RIO, on where the kernel runs it
    uint16_t limitRefresh = kDefaults.limitRefreshHz;  ///< A refresh cap in Hz, 0 unlimited, `--led-limit-refresh`
    bool     inverse = kDefaults.inverse;              ///< For a panel whose colors come out inverted, `--led-inverse`
    bool     hardwarePulse = kDefaults.hardwarePulse;  ///< The PWM block times the pulses on a Pi 3 or 4, `--led-[no-]hardware-pulse`
    bool     busyWaiting = kDefaults.busyWaiting;      ///< Busy-waits while limiting refresh rather than sleeping, `--led-[no-]busy-waiting`

    /// The library's `--led-pwm-bits`, rendered at 8 whoever owns the map: 9 to 11 are the planes its CIE 1931 table fills, offered only on that curve.
    DepthRange depthRange() const override { return depthRangeFor(mapOwner()); }

    /// A change of curve moves the map to the other owner, so a depth the new range does not hold is clamped and the card rebuilt to show it.
    bool onControlChanged(const char* name) override {
        if (!owner_ || !name || std::strcmp(name, "curve") != 0) return false;
        owner_->bitDepth = clampDepth(owner_->bitDepth, mapOwner());
        return true;
    }

    /// Which header pins carry which line, above the peripheral select, where the bus output puts its board.
    void addWiringControls(ControlList& controls) override {
        controls.addSelect("mapping", mappingSel, kMappingOptions, kMappingCount);
    }

    /// The library's panel options and the frames shown each second, below the peripheral select.
    void addControls(ControlList& controls) override {
        controls.addSelect("panelType", panelTypeSel, kPanelTypeOptions, kPanelTypeCount);
        controls.addControl("rows", rows, 8, 64);       // the library's Validate: 8 to 64
        controls.addControl("cols", cols, 16, 512);     // 16 is the library's floor; 512 by kChainMax keeps the canvas width, 16,384, inside 16 bits
        controls.addControl("chain", chain, 1, kChainMax);
        controls.addSelect("multiplexing", multiplexingSel, kMultiplexingOptions, kMultiplexingCount);
        controls.addSelect("rowAddrType", rowAddrSel, kRowAddrOptions, kRowAddrCount);
        controls.addSelect("scanMode", scanModeSel, kScanModeOptions, kScanModeCount);
        controls.addControl("bitDepth", owner_->bitDepth, depthRange().min, depthRange().max);
        controls.addControl("pwmLsbNs", pwmLsbNs, 50, 3000);   // the library's Validate: 50 to 3000
        controls.addControl("ditherBits", ditherBits, 0, kDitherMax);
        controls.addSelect("slowdown", slowdownSel, kSlowdownOptions, kSlowdownCount);
        // Shown only where the PIO check allows PIO; elsewhere RIO drives the panels whatever is saved.
        controls.addControl("rp1Pio", rp1Pio);
        controls.setHidden(controls.count() - 1, platform::rgbMatrixPioRefusal() != nullptr);
        controls.addControl("limitRefresh", limitRefresh, 0, 1000);   // a cap steadies a slow chain or matches a camera, both well below 1000
        controls.addControl("inverse", inverse);
        controls.addControl("hardwarePulse", hardwarePulse);
        controls.addControl("busyWaiting", busyWaiting);
        /// The frames the panels showed in the last second.
        controls.addReadOnly("shown", shownStr_, sizeof(shownStr_));
    }

    /// Every library option reopens the panels; the read-only rate does not.
    bool affectsPrepare(const char* name) const override {
        // A curve change reopens the panels only when the depth it clamps to is not the one the library was opened at.
        if (std::strcmp(name, "curve") == 0) return owner_ && clampDepth(owner_->bitDepth, mapOwner()) != openDepth_;
        static const char* const kReopen[] = {
            "mapping", "panelType", "rows", "cols", "chain", "multiplexing", "rowAddrType",
            "scanMode", "bitDepth", "pwmLsbNs", "ditherBits", "slowdown", "rp1Pio", "limitRefresh",
            "inverse", "hardwarePulse", "busyWaiting",
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
    platform::RgbMatrixOptions options() const {
        platform::RgbMatrixOptions o;
        o.mapping = mappingName();
        o.panelType = panelTypeSel > 0 && panelTypeSel < kPanelTypeCount ? kPanelTypeOptions[panelTypeSel] : "";
        o.rows = rows;
        o.cols = cols;
        o.chain = chain;
        o.parallel = derivedParallel_ ? derivedParallel_ : 1;   // the layout decides it, in open()
        o.multiplexing = multiplexingSel;
        o.rowAddrType = rowAddrSel;
        o.scanMode = scanModeSel;
        o.pwmBits = owner_ ? owner_->bitDepth : depthRange().initial;   // a detached output answers its range's starting depth
        o.pwmLsbNs = pwmLsbNs;
        o.ditherBits = ditherBits;
        o.slowdown = slowdownSel == 0 || slowdownSel >= kSlowdownCount ? -1 : static_cast<int8_t>(slowdownSel - 1);
        o.rp1Pio = rp1Pio;
        o.limitRefreshHz = limitRefresh;
        o.inverse = inverse;
        o.hardwarePulse = hardwarePulse;
        o.busyWaiting = busyWaiting;
        return o;
    }

    /// Open the panels; the canvas they form is the frame, whatever the layout's box, and warnFit() compares the light counts.
    const char* open(uint16_t /*width*/, uint16_t /*height*/) override {
        close();
        // A depth loaded from storage can sit outside the curve's range, since a load sets values and skips the control change.
        if (owner_) owner_->bitDepth = clampDepth(owner_->bitDepth, mapOwner());
        const uint8_t bits = owner_ ? owner_->bitDepth : depthRange().initial;
        openDepth_ = bits;

        // The layout sets the geometry: its lights, cut into whole panels, are the panels, and the card's chain divides them into outputs.
        const uint32_t perPanel = static_cast<uint32_t>(rows) * cols;
        const uint32_t layoutLights = owner_ && owner_->layer() ? owner_->layer()->physicalLightCount() : 0;
        const uint32_t panels = perPanel ? layoutLights / perPanel : 0;
        const char* const mapping = mappingName();
        const uint8_t drives = platform::rgbMatrixMaxChains(mapping);   // 0: the host cannot say, and the open refuses what the library would
        const uint8_t limit = drives ? drives : 255;
        const uint16_t scanRows = rows > 1 ? rows / 2 : 1;
        // The library clocks the planes from 11 minus the depth upward, each keeping its long lit time, and the dither shortens them all.
        const uint8_t first = static_cast<uint8_t>(bits < 11 ? 11 - bits : 0);
        const CardGeometry geometry{{rows, cols, scanRows, bits, pwmLsbNs, first, ditherBits}, chain, limit, mapping};
        const GeometryFit fit = checkGeometry(panels, chain, limit);
        message_[0] = '\0';
        if (fit != GeometryFit::Ok) {
            geometryMessage(fit, panels, geometry, message_, sizeof(message_));
            return message_;
        }
        derivedParallel_ = static_cast<uint8_t>(panels / chain);

        const platform::RgbMatrixOptions o = options();
        if (!platform::rgbMatrixOpen(o)) {
            const char* why = platform::rgbMatrixLastError();
            return why ? why : "the panels would not open";
        }
        open_ = true;
        canvasW_ = platform::rgbMatrixWidth();
        canvasH_ = platform::rgbMatrixHeight();
        placed_ = {rows, cols, chain};
        // A chain is not a raster: the layout's lights run panel by panel, so the push reads a second frame they are placed on.
        if (chain > 1 && !canvas_.resize(static_cast<size_t>(lights()) * 3)) {
            close();
            return "not enough memory for the chain canvas";
        }
        std::snprintf(status_, sizeof(status_), "%ux%u, %u-bit%s", static_cast<unsigned>(canvasW_), static_cast<unsigned>(canvasH_),
                      static_cast<unsigned>(o.pwmBits), platform::rgbMatrixOnPio() ? ", PIO" : "");
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

    /// Hand the frame over, with the library's table and the brightness when it applies the curve, direct at full level when the correction did.
    void show(const uint8_t* rgb, uint8_t brightness) MM_NONBLOCKING override {
        const bool cie = curveIsCie();
        platform::rgbMatrixCorrection(cie ? brightness : 255, cie);
        if (canvas_.bytes() == 0) {
            platform::rgbMatrixPush(rgb);
            return;
        }
        placeOnCanvas(rgb);
        platform::rgbMatrixPush(canvas_.data());
    }

    /// Publish the frames shown in the last second.
    void tick1s() MM_NONBLOCKING override {
        if (!open_) return;
        const uint16_t fps = platform::rgbMatrixFps();
        if (fps == lastShown_) return;   // a changed value alone re-serializes on the 1 Hz tick
        lastShown_ = fps;
        mm::formatTo(shownStr_, sizeof(shownStr_), "%u fps", fps);
    }

    /// Stop the refresh and darken the panels.
    void close() override {
        if (open_) platform::rgbMatrixClose();
        open_ = false;
        canvasW_ = canvasH_ = 0;
        canvas_.resize(0);
        shownStr_[0] = '\0';   // a closed panel shows an empty rate
        lastShown_ = 0xFFFF;
    }

    /// The chain canvas, when the panels form a chain.
    size_t heapBytes() const override { return canvas_.bytes(); }

    /// The Pi's header, which one HUB75 driver per Pi drives.
    LedHwBlock hwBlock() const override { return LedHwBlock::RgbMatrix; }
    /// With CIE 1931 the library applies brightness, so the correction keeps full values.
    bool dimsByTime() const MM_NONBLOCKING override { return curveIsCie(); }
    /// With CIE 1931 the library's table is the curve, so the correction bakes Linear.
    bool appliesCurve() const override { return curveIsCie(); }

private:
    /// Who applies the one color map: the library for the CIE 1931 curve its table realizes, MoonLight for every other. A detached output answers the default curve's.
    MapOwner mapOwner() const MM_NONBLOCKING {
        return owner_ ? mapOwnerFor(owner_->selectedCurve()) : MapOwner::Library;
    }

    /// Whether the library applies the map.
    bool curveIsCie() const MM_NONBLOCKING { return mapOwner() == MapOwner::Library; }

    /// The selected GPIO mapping's library name.
    const char* mappingName() const { return kMappingOptions[mappingSel < kMappingCount ? mappingSel : 0]; }

    /// Append to the seam's warning, each part whole and joined with "; " as the seam joins its own, a light count other than the canvas's, then the walk's panel.
    void warnFit(uint32_t layoutLights) {
        detail::Text t{message_, sizeof(message_)};
        if (const char* seam = platform::rgbMatrixWarning()) t.add("%s", seam);
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
    uint8_t  openDepth_ = 0;         ///< the `pwm_bits` the library was opened at, so a curve change knows whether the depth moved
    uint8_t  derivedParallel_ = 0;   ///< the outputs the layout's panels divide into, set by open()
    char     message_[kGeometryMessageMax] = "";   ///< the way out of a geometry that does not fit, or the running panels' warnings, whole
    uint16_t canvasW_ = 0;
    uint16_t canvasH_ = 0;
    uint16_t lastShown_ = 0xFFFF;   // not 0: the first real 0 must still publish
    char     shownStr_[16] = "";
    char     status_[32] = {};   ///< the canvas and the depth, at most "65535x65535, 11-bit, PIO"
    Placed   placed_;
    ScratchBuffer<uint8_t> canvas_;   ///< the placed frame of a chain, empty for one panel per output, whose lights are already a raster
};

/// Make the Pi output, which Hub75Driver makes only where `hasRgbMatrix` holds, so an ESP32 image links none of it.
inline Hub75Output* makeHub75MatrixOutput(Hub75Driver& owner) { return new Hub75MatrixOutput(owner); }

} // namespace mm
