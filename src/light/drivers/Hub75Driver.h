#pragma once

#include "light/drivers/DriverBase.h"
#include "light/drivers/Hub75Fit.h"      // kGeometryMessageMax: the longest status the Pi output writes
#include "light/drivers/Hub75Output.h"   // the strategy that takes the corrected frame to the panel
#include "light/layers/Layer.h"
#include "platform/platform.h"
#include "core/util/format.h"   // formatTo: the output's status into a fixed buffer

#include <algorithm>
#include <cstring>

namespace mm {

class Hub75Driver;
/// The bus output, for LCD_CAM and Parlio; defined in Hub75BusOutput.h, which this header includes after the class.
inline Hub75Output* makeHub75BusOutput();
/// The Pi header's output, whose chain canvas `owner` accounts; defined in Hub75MatrixOutput.h, made only where `hasRgbMatrix` holds.
inline Hub75Output* makeHub75MatrixOutput(Hub75Driver& owner);
/// The Pi 5 PIO output, whose chain canvas `owner` accounts; defined in Hub75PioOutput.h, made only where `hasHub75Pio` holds.
inline Hub75Output* makeHub75PioOutput(Hub75Driver& owner);
/// The Pi 3/4 DMA output, whose chain canvas `owner` accounts; defined in Hub75DmaOutput.h, made only where `hasHub75Dma` holds.
inline Hub75Output* makeHub75DmaOutput(Hub75Driver& owner);

/// Drives HUB75 LED panels straight from the board's own pins, with no receiving card in between.
/// @card Hub75Driver.png
///
/// The sibling of @ref PanelCardDriver, which drives the same panels over Ethernet. A [MoonHub75](https://moonmodules.org/projects/hardware/#moonhub75-pcb) runs a 64x64 1/32-scan panel at 6-bit and 1,508 Hz; larger walls wait on reports.
///
/// @attribution LostHope, Delphium, RobWomp, mrcodetastic, hzeller, ESPHome, Adafruit, Waveshare
///
/// @guide
///
/// ## Which of the two paths
///
/// Above roughly 16,384 pixels a receiving card (@ref PanelCardDriver) earns its place; [HUB75 panels](https://moonmodules.org/MoonLight/reference/hardware/hub75-panels.html#which-of-the-two-paths) has the choice.
///
/// @technical
///
/// **Outputs.** The driver corrects the layout into one RGB frame in light order for a Hub75Output. The bus output reads a row-major raster; the three Pi outputs' canvas is a chain of panels, so each places its lights first (Hub75Canvas.h): the matrix output hands the frame to the library's RGB seam, the PIO output to the RP1 state machine's, the DMA output to the Pi 3/4 engine's.
///
/// **Continuous output.** A HUB75 panel is lit only while clocked, so on an ESP32 the platform re-sends one buffer forever and the bus output writes the next frame into it between scans, while tick() starts no transfer; the Pi outputs hand each frame to their seams, the matrix output's to the library and the PIO and DMA outputs' to their engines' refresh threads. Format: Hub75Slots.h.
///
/// **Brightness.** Binary coded modulation lights plane `p` for 2^p time units. Each plane is stored once, since emitting it 2^p times costs 1,060,800 bytes at 8-bit on a 64x64 panel against 33,280.
///
/// **Pins.** The first definition fills them from the chip's first board; members stay -1, since a blind guess lands on PSRAM, USB, UART0 or a strapping pin. WLED's array ends `...LAT,OE,CLK`: a map written `...CLK,LAT,OE` swaps three lines.
class Hub75Driver : public DriverBase {
public:
    // The bus output's settings and the shared bitDepth live on the driver, which binds them, so a saved config finds them whichever output reads them.

    /// The board's wiring: picking one fills the fourteen pins, Custom keeps yours.
    /// @guide A published map hides the pin rows, which stay bound. A pin on flash or PSRAM is refused. Boards and the R1/G1/B1 naming trap: [HUB75 panels](https://moonmodules.org/MoonLight/reference/hardware/hub75-panels.html#which-board).
    uint8_t boardSel = 0;

    int8_t r1 = -1;   ///< Red, upper half-panel.
    int8_t g1 = -1;   ///< Green, upper half-panel.
    int8_t b1 = -1;   ///< Blue, upper half-panel.
    int8_t r2 = -1;   ///< Red, lower half-panel.
    int8_t g2 = -1;   ///< Green, lower half-panel.
    int8_t b2 = -1;   ///< Blue, lower half-panel.
    int8_t addrA = -1;   ///< Row address bit 0.
    int8_t addrB = -1;   ///< Row address bit 1.
    int8_t addrC = -1;   ///< Row address bit 2.
    int8_t addrD = -1;   ///< Row address bit 3.
    int8_t addrE = -1;   ///< Row address bit 4, on 1/32 panels.
    int8_t clk = -1;   ///< The shift clock.
    int8_t lat = -1;   ///< Latch: moves the shift register to the output drivers.
    int8_t oe = -1;    ///< Output enable, active LOW.

    /// The clock edges `clockEdgeSel` picks from, in kEdgeOptions order.
    enum ClockEdge : uint8_t { kEdgeRising, kEdgeFalling, kEdgeCount };
    /// The labels `clockEdgeSel` indexes.
    static constexpr const char* kEdgeOptions[kEdgeCount] = {"rising", "falling"};
    /// The edge the chips sample on; every pixel one column over means the other.
    uint8_t clockEdgeSel = kEdgeRising;
    /// The scan rates `scanSel` picks from, in kScanOptions order.
    static constexpr uint8_t kScanCount = 3;
    /// The labels `scanSel` indexes.
    static constexpr const char* kScanOptions[kScanCount] = {"1/8", "1/16", "1/32"};
    /// The scan rate each label means: 8, 16 or 32.
    static constexpr uint8_t kScanRates[kScanCount] = {8, 16, 32};
    /// The panel's scan rate: 1/8, 1/16 or 1/32, read off the panel.
    /// @guide Two panels of identical dimensions can scan differently, so the size does not give the rate. A 1/32-scan panel needs `e`; on 1/16 panels that pin is free.
    uint8_t scanSel = 1;

    /// Bit planes per frame: color levels against refresh and memory.
    /// @guide Depth costs refresh and memory linearly.
    uint8_t bitDepth = 6;

    /// What drives the panels: LCD_CAM, Parlio, Pi header, Pi PIO or Pi DMA, as the board offers.
    /// @guide A P4 has one LCD_CAM and one Parlio, so a board already driving WS2812 strips from one needs the panel on the other. Only you know which way round.
    uint8_t peripheralSel_ = 0;

    /// A fresh driver takes the output for its first backend, so it always has one.
    Hub75Driver() { ensureOutputMatchesSelection(); }
    /// Free the output; the owner has released the driver first, which closed it.
    ~Hub75Driver() override { delete output_; }

    /// The output the selected backend drives.
    Hub75Output* output() const { return output_; }

    /// The backend the `peripheral` select names, for the output that drives it.
    platform::Hub75Backend selectedBackend() const {
        return backendIndex_[peripheralSel_ < backendOptionCount_ ? peripheralSel_ : 0];
    }

    /// Whether the panel dims by lit time rather than by scaled values, as the output answers.
    bool dimsByTime() const MM_NONBLOCKING override { return output_->dimsByTime(); }
    /// Whether the output applies the brightness curve itself, so the correction bakes Linear.
    bool appliesCurve() const override { return output_->appliesCurve(); }

    /// The hardware block the output holds while the panel runs, so a failed init claims nothing; the sibling claim guard reads it.
    LedHwBlock hwBlock() const override {
        return running_ ? output_->hwBlock() : LedHwBlock::None;
    }

    /// Bind the rendered frame this driver corrects from.
    void setSourceBuffer(Buffer* buf) override { sourceBuffer_ = buf; }

    /// Publish the output's wiring, the peripheral select, then the output's own controls, the output being the one the select names.
    void defineDriverControls() override {
        buildBackendOptions();
        ensureOutputMatchesSelection();
        output_->addWiringControls(controls_);
        controls_.addSelect("peripheral", peripheralSel_, backendOptions_, backendOptionCount_);
        output_->addControls(controls_);
    }

    /// Rebuild the card when the output says a change altered it, such as a board writing its pins; for `peripheral` the scheduler already rebuilt it.
    void onControlChanged(const char* name) override {
        // rebuildControls CLEARS first; a bare defineControls appended a second set of rows.
        if (name && output_->onControlChanged(name)) rebuildControls();
        DriverBase::onControlChanged(name);
    }

    /// Whether changing `name` forces a rebuild: the peripheral does, and whatever the output says does; prepare() re-initializes for the rest.
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "peripheral") == 0 || output_->affectsPrepare(name);
    }

    /// Stop the panel, release its peripheral, and free the correction scratch.
    void release() override {
        stopScan();
        DriverBase::release();
    }

    /// Refuse a block a sibling holds, then open the output and size the correction scratch to its canvas.
    void prepare() override {
        stopScan();   // a reinit keeps the correction scratch, which release() frees
        if (!layer_) {
            setStatus("no layer", Severity::Warning);
            return;
        }
        // One block per chip, so a sibling holding it means idle with a status rather than fight.
        if (siblingClaims(output_->hwBlock())) {
            setConfigErr(kBlockInUse);
            return;
        }
        if (const char* why = output_->open(static_cast<uint16_t>(layer_->physicalWidth()),
                                            static_cast<uint16_t>(layer_->physicalHeight()))) {
            showStatus(why, Severity::Error);
            return;
        }
        // One whole frame of packed RGB: the output needs the frame, not one light at a time.
        ensureWire(static_cast<size_t>(output_->lights()) * 3);
        if (!wire_) {
            setStatus("not enough memory for the correction buffer", Severity::Error);
            output_->close();
            return;
        }
        if (!sizeLightScratch()) {   // its status names the shortage
            output_->close();
            return;
        }

        running_ = true;
        publishHeapBytes();   // the output's own memory, such as the Pi output's chain canvas, joins the readout
        if (const char* warn = output_->warning()) showStatus(warn, Severity::Warning);
        else showStatus(output_->describe(), Severity::Status);
    }

    /// Correct the frame into one RGB frame and hand it to the output.
    void tick() MM_NONBLOCKING override {
        if (!running_ || !sourceBuffer_ || !sourceBuffer_->data()) return;

        const uint8_t srcCh = sourceBuffer_->channelsPerLight();
        const uint8_t outCh = correction_.outChannels;
        const nrOfLightsType lights = output_->lights();
        if (srcCh == 0 || outCh < 3 || lights == 0) return;
        if (!wire_ || wireCap_ < static_cast<size_t>(lights) * 3) return;   // not ready
        // A layout with fewer lights than the canvas leaves the rest of the panel dark.
        const nrOfLightsType have = sourceBuffer_->count() < lights ? sourceBuffer_->count() : lights;

        // A plane reads (x, r) and the pixel half a panel below in one pass, so correct first.
        correctFrameToRgb(sourceBuffer_->data(), srcCh, wire_, have);
        std::memset(wire_ + static_cast<size_t>(have) * 3, 0, static_cast<size_t>(lights - have) * 3);

        // Read each frame, so the slider applies on the next one.
        output_->show(wire_, effectiveBrightness());
    }

    /// Resize the correction scratch when the wiring changes, once core 1 has finished the frame it writes through it.
    void onCorrectionChanged() override {
        if (!running_) return;
        const size_t frameBytes = static_cast<size_t>(output_->lights()) * 3;
        if (wireCap_ >= frameBytes && !outChannelsChanged()) return;   // a brightness change moves no buffer
        if (parent()) parent()->quiesce();   // a resize frees the block the encode worker may be writing
        ensureWire(frameBytes);
        sizeLightScratch();
    }

    /// Let the output publish its measured readouts; MM_NONBLOCKING stays, or the base's contract widens and the compiler refuses.
    void tick1s() MM_NONBLOCKING override {
        if (running_) output_->tick1s();
    }

protected:
    /// The memory the output holds counts toward the driver's heap readout, beside the correction scratch.
    size_t driverHeapBytes() const override { return DriverBase::driverHeapBytes() + (output_ ? output_->heapBytes() : 0); }

private:
    /// Show `text` from the output as the status, copied, since the output it lives in can be deleted while the status still shows.
    void showStatus(const char* text, Severity sev) {
        formatTo(statusBuf_, sizeof(statusBuf_), "%s", text);
        setStatus(statusBuf_, sev);
    }

    /// Stop the panel and release its peripheral, the reinit both release() and prepare() begin with.
    void stopScan() {
        output_->close();
        running_ = false;
        publishHeapBytes();
    }

    /// Which output class a backend drives: LCD_CAM and Parlio share the bus output, the three Pi backends each have their own.
    enum class OutputKind : uint8_t { Bus, Matrix, Pio, Dma };
    static OutputKind kindFor(platform::Hub75Backend backend) {
        if (backend == platform::Hub75Backend::RgbMatrix) return OutputKind::Matrix;
        if (backend == platform::Hub75Backend::Pio) return OutputKind::Pio;
        if (backend == platform::Hub75Backend::Dma) return OutputKind::Dma;
        return OutputKind::Bus;
    }

    /// Make the output for `backend`; the Pi's are compiled in only where the capability holds, as NDI is, so an ESP32 image links none of them.
    Hub75Output* makeOutput(platform::Hub75Backend backend) {
        if constexpr (platform::hasRgbMatrix) {
            if (backend == platform::Hub75Backend::RgbMatrix) return makeHub75MatrixOutput(*this);
        }
        if constexpr (platform::hasHub75Pio) {
            if (backend == platform::Hub75Backend::Pio) return makeHub75PioOutput(*this);
        }
        if constexpr (platform::hasHub75Dma) {
            if (backend == platform::Hub75Backend::Dma) return makeHub75DmaOutput(*this);
        }
        return makeHub75BusOutput();
    }

    /// Hold the output the selected backend drives, swapping only when it is another one: LCD_CAM and Parlio share the bus output, which keeps its state across them. The kind, not the hardware block, tells the outputs apart, since the three Pi outputs claim the same header.
    void ensureOutputMatchesSelection() {
        if (output_ && kindFor(selectedBackend()) == outputKind_) return;
        const bool atDefault = !output_ || bitDepth == output_->depthRange().initial;
        const bool replacing = output_ != nullptr;
        if (replacing) {
            MoonModule::notifyQuiesceRender();   // the encode worker reads output_ in tick()
            stopScan();
            delete output_;
        }
        output_ = makeOutput(selectedBackend());
        outputKind_ = kindFor(selectedBackend());
        output_->attach(this);
        // A depth left at the old output's default takes the new one's; a chosen one is clamped into the new range.
        const DepthRange r = output_->depthRange();
        bitDepth = atDefault ? r.initial : std::clamp(bitDepth, r.min, r.max);
        // The new output may own the curve and the dimming the old one left to the correction, so the correction is baked for it again.
        if (replacing) rebuildCorrection(lastGlobalBrightness_);
    }

    /// Offer the backends this silicon has and that can carry the output's frame, re-pointing by label so a dropped backend cannot select another.
    void buildBackendOptions() {
        const char* current = (peripheralSel_ < backendOptionCount_)
                                  ? backendOptions_[peripheralSel_] : nullptr;
        // Zero while the layer is unknown, which asks only whether the silicon exists.
        const size_t bytes = layer_
            ? output_->frameBytes(static_cast<uint16_t>(layer_->physicalWidth()),
                                  static_cast<uint16_t>(layer_->physicalHeight()))
            : 0;

        backendOptionCount_ = 0;
        for (uint8_t i = 0; i < kBackendCount; i++) {
            const auto b = static_cast<platform::Hub75Backend>(i);
            if (!platform::hub75BackendAvailable(b, bytes)) continue;
            backendOptions_[backendOptionCount_] = platform::hub75BackendLabel(b);
            backendIndex_[backendOptionCount_] = b;
            backendOptionCount_++;
        }
        if (backendOptionCount_ == 0) {      // no usable backend (desktop, classic ESP32)
            backendOptions_[0] = "(none)";
            backendIndex_[0] = platform::Hub75Backend::LcdCam;
            backendOptionCount_ = 1;
        }
        uint8_t sel = 0;
        if (current) {
            for (uint8_t k = 0; k < backendOptionCount_; k++) {
                if (std::strcmp(backendOptions_[k], current) == 0) { sel = k; break; }
            }
        }
        peripheralSel_ = sel;
    }

    /// The output's lines for the pin map, since their hidden controls read as free there.
    uint8_t fixedPins(FixedPin* out, uint8_t max) const override {
        return output_->fixedPins(out, max);
    }

    static constexpr uint8_t kBackendCount = 5;
    const char* backendOptions_[kBackendCount] = {};   // "(none)" takes slot 0 when no backend is offered
    platform::Hub75Backend backendIndex_[kBackendCount] = {};
    uint8_t backendOptionCount_ = 0;

    Hub75Output* output_ = nullptr;   // never null once constructed
    OutputKind outputKind_ = OutputKind::Bus;   // which output class output_ is, set beside it in ensureOutputMatchesSelection()
    Buffer* sourceBuffer_ = nullptr;
    /// Whole messages: the Pi outputs' way out of a geometry that does not fit, and their layout warnings, run long; the bus output's refusals do not.
    static constexpr size_t kStatusMax = (platform::hasRgbMatrix || platform::hasHub75Pio || platform::hasHub75Dma) ? kGeometryMessageMax : 96;
    char statusBuf_[kStatusMax] = {};   // the status shown, held here since setStatus keeps the pointer and a backend change deletes the output
    bool running_ = false;
};

}  // namespace mm

// The outputs read the settings declared above, so their headers follow the class and define the make functions it declares; included here so every user of the driver gets all four.
#include "light/drivers/Hub75BusOutput.h"
#include "light/drivers/Hub75MatrixOutput.h"
#include "light/drivers/Hub75PioOutput.h"
#include "light/drivers/Hub75DmaOutput.h"
