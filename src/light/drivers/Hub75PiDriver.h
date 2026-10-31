#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "light/drivers/DriverBase.h"
#include "light/layers/Layer.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

namespace mm {

/// Drives HUB75 LED panels from a Raspberry Pi's own header, through Henner Zeller's rpi-rgb-led-matrix.
/// @card Hub75PiDriver.png
///
/// The sibling of @ref Hub75Driver, which drives the same panels from an ESP32's pins. On a Pi the library times the panels and MoonLight hands it each frame, so a Pi 5 drives a 256x192 wall from the process that renders it.
///
/// @attribution hzeller
///
/// @guide
///
/// ## The controls are the library's
///
/// Each control is one of the library's options under its own name, so a working `--led-...` command line reads straight across: `--led-rows=64` is `rows` 64. Size the layout to the canvas the status names. Every change reopens the panels on the next frame.
///
/// ## Permissions
///
/// A Pi 5 reaches the panels through the `gpio` group; a Pi 3 or 4 needs root. The library's refresh thread wants `CAP_SYS_NICE`, without which the status warns of flicker. Setup and every status message: [HUB75 panels](https://moonmodules.org/MoonLight/reference/hardware/hub75-panels.html#raspberry-pi).
///
/// ## PIO is experimental
///
/// `rp1Pio` clocks a Pi 5's panels through the RP1's PIO block in place of RIO. It can stall at an open, after which MoonLight needs a restart, so an installation keeps RIO.
class Hub75PiDriver : public DriverBase {
public:
    /// The library's GPIO mappings for a 64-bit Pi, in its own order; the -pi1 ones fit a Pi 1 the arm64 build never runs on, and compute-module needs a build switch its default build leaves off.
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
    /// The GPIO slowdown: auto takes the library's own choice for the model.
    static constexpr uint8_t kSlowdownCount = 7;
    static constexpr const char* kSlowdownOptions[kSlowdownCount] = {"auto", "0", "1", "2", "3", "4", "5"};

    uint8_t  mappingSel = 0;        ///< --led-gpio-mapping
    uint8_t  panelTypeSel = 0;      ///< --led-panel-type
    char     pixelMapper[48] = "";  ///< --led-pixel-mapper, in the library's syntax: `Rotate:90;U-mapper`
    uint8_t  rows = 32;             ///< --led-rows, of one panel
    uint16_t cols = 64;             ///< --led-cols, of one panel
    uint8_t  chain = 1;             ///< --led-chain
    uint8_t  parallel = 1;          ///< --led-parallel
    uint8_t  multiplexingSel = 0;   ///< --led-multiplexing
    uint8_t  rowAddrSel = 0;        ///< --led-row-addr-type
    uint8_t  scanModeSel = 0;       ///< --led-scan-mode
    uint8_t  pwmBits = 11;          ///< --led-pwm-bits: color depth against refresh
    uint16_t pwmLsbNs = 130;        ///< --led-pwm-lsb-nanoseconds
    uint8_t  ditherBits = 0;        ///< --led-pwm-dither-bits
    uint8_t  slowdownSel = 0;       ///< --led-slowdown-gpio
    bool     rp1Pio = false;        ///< --led-rp1-pio, a Pi 5 alone, and experimental
    uint16_t limitRefresh = 0;      ///< --led-limit-refresh, 0 unlimited

    /// The library corrects to CIE 1931 at up to 11 bits, so the curve stays linear and its control hidden: two curves would darken the dark end twice.
    Hub75PiDriver() { linearCurve_ = true; }

    /// The panels, claimed while open, so a second driver refuses them rather than reopening them under the first.
    LedHwBlock hwBlock() const override { return open_ ? LedHwBlock::RgbMatrix : LedHwBlock::None; }

    /// A panel shows a level only as lit time, so brightness goes to the library rather than scaling values.
    bool dimsByTime() const MM_NONBLOCKING override { return true; }

    /// Bind the rendered frame this driver hands over.
    void setSourceBuffer(Buffer* buf) override { sourceBuffer_ = buf; }

    /// Publish the library's options and the frames shown each second.
    void defineDriverControls() override {
        controls_.addSelect("mapping", mappingSel, kMappingOptions, kMappingCount);
        controls_.addSelect("panelType", panelTypeSel, kPanelTypeOptions, kPanelTypeCount);
        controls_.addText("pixelMapper", pixelMapper, sizeof(pixelMapper));
        controls_.addControl("rows", rows, 8, 64);
        controls_.addControl("cols", cols, 16, 512);
        controls_.addControl("chain", chain, 1, 32);
        controls_.addControl("parallel", parallel, 1, 3);   // 3 outputs at most, a compute module's 6 needing a mapping left out above
        controls_.addSelect("multiplexing", multiplexingSel, kMultiplexingOptions, kMultiplexingCount);
        controls_.addSelect("rowAddrType", rowAddrSel, kRowAddrOptions, kRowAddrCount);
        controls_.addSelect("scanMode", scanModeSel, kScanModeOptions, kScanModeCount);
        controls_.addControl("pwmBits", pwmBits, 1, 11);
        controls_.addControl("pwmLsbNs", pwmLsbNs, 50, 3000);
        controls_.addControl("ditherBits", ditherBits, 0, 2);
        controls_.addSelect("slowdown", slowdownSel, kSlowdownOptions, kSlowdownCount);
        controls_.addControl("rp1Pio", rp1Pio);
        controls_.addControl("limitRefresh", limitRefresh, 0, 1000);
        /// The frames the panels showed in the last second.
        controls_.addReadOnly("shown", shownStr_, sizeof(shownStr_));
    }

    /// Every option of the library's reopens the panels: all but the correction block and the read-only rate.
    bool affectsPrepare(const char* name) const override {
        return !isCorrectionControl(name) && std::strcmp(name, "shown") != 0;
    }

    /// The options as the platform takes them.
    platform::RgbMatrixOptions options() const {
        platform::RgbMatrixOptions o;
        o.mapping = kMappingOptions[mappingSel < kMappingCount ? mappingSel : 0];
        o.panelType = panelTypeSel > 0 && panelTypeSel < kPanelTypeCount ? kPanelTypeOptions[panelTypeSel] : "";
        o.pixelMapper = pixelMapper;
        o.rows = rows;
        o.cols = cols;
        o.chain = chain;
        o.parallel = parallel;
        o.multiplexing = multiplexingSel;
        o.rowAddrType = rowAddrSel;
        o.scanMode = scanModeSel;
        o.pwmBits = pwmBits;
        o.pwmLsbNs = pwmLsbNs;
        o.ditherBits = ditherBits;
        o.slowdown = slowdownSel == 0 || slowdownSel >= kSlowdownCount ? -1 : static_cast<int8_t>(slowdownSel - 1);
        o.rp1Pio = rp1Pio;
        o.limitRefreshHz = limitRefresh;
        return o;
    }

    /// Open the panels and size the frame to the canvas they form.
    void prepare() override {
        release();
        if (siblingClaims(LedHwBlock::RgbMatrix)) {
            setStatus("another driver drives the panels", Severity::Error);
            return;
        }
        if (!platform::rgbMatrixOpen(options())) {
            const char* why = platform::rgbMatrixLastError();
            setStatus(why ? why : "the panels would not open", Severity::Error);
            return;
        }
        open_ = true;
        canvasW_ = platform::rgbMatrixWidth();
        canvasH_ = platform::rgbMatrixHeight();
        ensureWire(static_cast<size_t>(canvasW_) * canvasH_ * 3);
        if (!wire_) {
            release();
            setStatus("not enough memory for the frame", Severity::Error);
            return;
        }
        std::memset(wire_, 0, static_cast<size_t>(canvasW_) * canvasH_ * 3);   // what the layout does not cover stays dark
        if (sizeLightScratch()) publishOpenStatus();   // else its status names the shortage
    }

    /// Stop the refresh, darken the panels and free the frame.
    void release() override {
        if (open_) { platform::rgbMatrixClose(); open_ = false; }
        canvasW_ = canvasH_ = 0;
        shownStr_[0] = '\0';   // a closed panel shows nothing, so no rate either
        lastShown_ = 0xFFFF;
        DriverBase::release();   // frees the correction scratch, clears failBuf_ + configErr_
    }

    /// Correct the frame onto the canvas, where the layout and the panels overlap, and hand it over; a position with no light stays dark.
    void tick() MM_NONBLOCKING override {
        if (!open_ || !sourceBuffer_ || !sourceBuffer_->data() || !layer_ || !wire_) return;
        const uint8_t srcCh = sourceBuffer_->channelsPerLight();
        const uint8_t outCh = correction_.outChannels;
        if (srcCh == 0 || outCh < 3) return;
        const uint16_t layoutW = static_cast<uint16_t>(layer_->physicalWidth());
        const uint16_t layoutH = static_cast<uint16_t>(layer_->physicalHeight());
        const uint16_t w = layoutW < canvasW_ ? layoutW : canvasW_;
        const uint16_t h = layoutH < canvasH_ ? layoutH : canvasH_;
        const nrOfLightsType have = sourceBuffer_->count();
        const uint8_t* src = sourceBuffer_->data();
        if (outCh > 3 && lightScratch_.count() < outCh) return;   // not ready
        uint8_t* scratch = outCh > 3 ? &lightScratch_[0] : nullptr;
        for (uint16_t y = 0; y < h; y++) {
            for (uint16_t x = 0; x < w; x++) {
                const nrOfLightsType i = static_cast<nrOfLightsType>(y) * layoutW + x;
                uint8_t* d = wire_ + (static_cast<size_t>(y) * canvasW_ + x) * 3;
                if (i >= have) {
                    d[0] = d[1] = d[2] = 0;   // a layout with fewer lights than its box
                } else if (outCh == 3) {
                    correction_.apply(src + static_cast<size_t>(i) * srcCh, d, srcCh);
                } else {
                    correctToRgb(src + static_cast<size_t>(i) * srcCh, srcCh, scratch, d);
                }
            }
        }
        platform::rgbMatrixBrightness(effectiveBrightness());
        platform::rgbMatrixPush(wire_);
    }

    /// Size the one-light scratch to a wiring wider than RGB, off the hot path, so a moving-head profile cannot write past it.
    void onCorrectionChanged() override { sizeLightScratch(); }

    /// Publish the frames shown in the last second.
    void tick1s() MM_NONBLOCKING override {
        if (!open_) return;
        const uint16_t fps = platform::rgbMatrixFps();
        if (fps == lastShown_) return;   // never re-serialize an unchanged value on the 1 Hz tick
        lastShown_ = fps;
        mm::formatTo(shownStr_, sizeof(shownStr_), "%u fps", fps);
    }

private:
    /// Say what the open panels drive: a layout that does not match them first, then the seam's warning, else the canvas and depth.
    void publishOpenStatus() {
        const unsigned layoutW = layer_ ? static_cast<unsigned>(layer_->physicalWidth()) : 0;
        const unsigned layoutH = layer_ ? static_cast<unsigned>(layer_->physicalHeight()) : 0;
        if (layoutW != canvasW_ || layoutH != canvasH_) {
            std::snprintf(statusBuf_, sizeof(statusBuf_), "layout %ux%u, panels %ux%u", layoutW, layoutH,
                          static_cast<unsigned>(canvasW_), static_cast<unsigned>(canvasH_));
            setStatus(statusBuf_, Severity::Warning);
        } else if (const char* warn = platform::rgbMatrixWarning()) {
            setStatus(warn, Severity::Warning);
        } else {
            std::snprintf(statusBuf_, sizeof(statusBuf_), "%ux%u, %u-bit%s", static_cast<unsigned>(canvasW_),
                          static_cast<unsigned>(canvasH_), static_cast<unsigned>(pwmBits), rp1Pio ? ", PIO" : "");
            setStatus(statusBuf_, Severity::Status);
        }
    }

    Buffer*  sourceBuffer_ = nullptr;
    bool     open_ = false;
    uint16_t canvasW_ = 0;
    uint16_t canvasH_ = 0;
    uint16_t lastShown_ = 0xFFFF;
    char     shownStr_[16] = "";
    char     statusBuf_[64] = {};
};

}  // namespace mm
