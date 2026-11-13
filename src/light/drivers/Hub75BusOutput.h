#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "light/drivers/Hub75Driver.h"   // the orchestrator, whose settings this output binds and reads; it includes this header back, after its class
#include "light/drivers/Hub75Output.h"
#include "light/drivers/Hub75Slots.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

namespace mm {

/// Hub75Driver's output on the ESP32's parallel bus: it packs the corrected frame into the panel's bit planes in the platform's buffer, which LCD_CAM or Parlio clocks out.
///
/// It owns the board table, the fourteen pins, the scan rate, the bit depth, the clock edge and the measured refresh; their values live on the driver, which binds them.
class Hub75BusOutput : public Hub75Output {
public:
    /// The board select and the fourteen pin controls, above the peripheral select.
    void addWiringControls(ControlList& controls) override {
        buildBoardOptions();
        // Once only, at the first card, so a re-render keeps an edited pin.
        if (!prefilled_) { prefilled_ = true; applyBoard(); }
        controls.addSelect("board", owner_->boardSel, boardOptions_, boardOptionCount_);

        // Hidden on a published map, and hidden stays BOUND: the values still drive the panel.
        const bool editable = pinsEditable();
        const char* const kPinNames[] = {"r1", "g1", "b1", "r2", "g2", "b2", "a", "b", "c",
                                         "d", "e", "clk", "lat", "oe"};
        Hub75Driver& d = *owner_;
        int8_t* const kPinVars[] = {&d.r1, &d.g1, &d.b1, &d.r2, &d.g2, &d.b2, &d.addrA, &d.addrB, &d.addrC,
                                    &d.addrD, &d.addrE, &d.clk, &d.lat, &d.oe};
        for (size_t i = 0; i < sizeof(kPinNames) / sizeof(kPinNames[0]); i++) {
            controls.addPin(kPinNames[i], *kPinVars[i]);
            controls.setHidden(controls.count() - 1, !editable);
        }
    }

    /// Two to six planes, the depths the bit-plane slots hold, starting at six.
    DepthRange depthRange() const override { return {2, 6, 6}; }

    /// The geometry, the clock edge and the measured refresh, below the peripheral select.
    void addControls(ControlList& controls) override {
        controls.addSelect("scanRate", owner_->scanSel, kScanOptions, kScanCount);
        controls.addControl("bitDepth", owner_->bitDepth, depthRange().min, depthRange().max);
        // The one knob a chip family changes. WLED exposes the same flag as its bus "reversed" box.
        controls.addSelect("clockEdge", owner_->clockEdgeSel, Hub75Driver::kEdgeOptions, Hub75Driver::kEdgeCount);

        /// The measured refresh rate: the number to report when a panel flickers.
        controls.addReadOnly("refresh", refreshStr_, sizeof(refreshStr_));
    }

    /// Picking a board writes its map into the pin controls at once, which the card then shows.
    bool onControlChanged(const char* name) override {
        if (std::strcmp(name, "board") != 0) return false;
        applyBoard();
        return true;
    }

    /// The wire format and the geometry need the bus reopened.
    bool affectsPrepare(const char* name) const override {
        static const char* const kRebuild[] = {
            "r1", "g1", "b1", "r2", "g2", "b2", "a", "b", "c", "d", "e",
            "clk", "lat", "oe", "scanRate", "bitDepth", "board", "clockEdge",
        };
        for (const char* n : kRebuild) {
            if (std::strcmp(name, n) == 0) return true;
        }
        return false;
    }

    /// The bytes the bit planes take for this layout at the selected scan and depth; 0 when they do not pair into rows.
    size_t frameBytes(uint16_t width, uint16_t height) const override {
        const Hub75Geometry probe = geometry(width, height);
        return probe.valid() ? probe.frameBytes() : 0;
    }

    /// Build the geometry, refuse an unusable line, then init the bus and arm the continuous scan.
    const char* open(uint16_t width, uint16_t height) override {
        geo_ = geometry(width, height);
        if (!geo_.valid()) {
            // Name WHICH constraint failed: the scan rate is nearly always the wrong one.
            std::snprintf(status_, sizeof(status_), "%ux%u does not divide into 1/%u scan row pairs",
                          geo_.width, geo_.height, geo_.scanRate);
            return status_;
        }

        // Routing I/O onto a flash or PSRAM pin resets the chip with no panic naming anything.
        {
            const char* role = nullptr;
            const char* why = nullptr;
            const int8_t bad = unusableLine(role, why);
            if (bad >= 0) {
                std::snprintf(status_, sizeof(status_), "%s on GPIO %d %s",
                              role, static_cast<int>(bad), why);
                return status_;
            }
        }

        const Hub75Driver& d = *owner_;
        platform::Hub75Pins pins;
        pins.r1 = toPin(d.r1); pins.g1 = toPin(d.g1); pins.b1 = toPin(d.b1);
        pins.r2 = toPin(d.r2); pins.g2 = toPin(d.g2); pins.b2 = toPin(d.b2);
        pins.a = toPin(d.addrA); pins.b = toPin(d.addrB); pins.c = toPin(d.addrC);
        pins.d = toPin(d.addrD); pins.e = toPin(d.addrE);
        pins.clk = toPin(d.clk); pins.lat = toPin(d.lat); pins.oe = toPin(d.oe);
        pins.clkFalling = d.clockEdgeSel == Hub75Driver::kEdgeFalling;

        if (!platform::hub75Init(hub_, d.selectedBackend(), pins, geo_.width, geo_.height,
                                 geo_.scanRate, geo_.bitDepth)) {
            const char* why = platform::hub75LastError();
            return why ? why : "HUB75 init failed";
        }
        if (!platform::hub75Start(hub_)) {
            const char* why = platform::hub75LastError();
            platform::hub75Deinit(hub_);
            return why ? why : "the panel scan would not start";
        }
        std::snprintf(status_, sizeof(status_), "%ux%u, 1/%u scan, %u-bit (%s)",
                      geo_.width, geo_.height, geo_.scanRate, geo_.bitDepth,
                      platform::hub75Backend(hub_));
        return nullptr;
    }

    /// The geometry and the backend, as open() formatted them.
    const char* describe() const override { return status_; }

    /// The layout's box, one light per panel pixel.
    nrOfLightsType lights() const MM_NONBLOCKING override {
        return static_cast<nrOfLightsType>(geo_.width) * geo_.height;
    }

    /// Pack the frame into the bit planes, brightness being the planes' lit time.
    void show(const uint8_t* rgb, uint8_t brightness) MM_NONBLOCKING override {
        uint8_t* out = platform::hub75Buffer(hub_);
        if (!out) return;
        geo_.brightness = brightness;
        // No double buffer: a one-pass tear beats a second 256 KB frame, invisible at 300+ Hz.
        hub75Encode(rgb, out, geo_);
    }

    /// Publish the measured refresh rate.
    void tick1s() MM_NONBLOCKING override {
        const uint16_t hz = platform::hub75RefreshHz(hub_);
        if (hz == lastRefresh_) return;      // a changed value alone re-serializes on the 1 Hz tick
        lastRefresh_ = hz;
        mm::formatTo(refreshStr_, sizeof(refreshStr_), "%u Hz", hz);
    }

    /// Stop the scan and release the panel's peripheral.
    void close() override { platform::hub75Deinit(hub_); }

    /// The block the selected backend drives: WHICH block, not a guess, since claiming the wrong one frees a live block and holds a free one.
    LedHwBlock hwBlock() const override {
        return owner_->selectedBackend() == platform::Hub75Backend::Parlio ? LedHwBlock::Parlio
                                                                           : LedHwBlock::LcdCam;
    }

    /// A published board's lines for the pin map, since their hidden controls read as free there.
    uint8_t fixedPins(MoonModule::FixedPin* out, uint8_t max) const override {
        // Hidden means "not in use" everywhere else; here the lines are soldered and driven.
        if (!out || pinsEditable()) return 0;
        Line wired[kLineCount];
        uint8_t n = 0;
        for (uint8_t i = 0, count = lines(wired); i < count && n < max; i++) {
            out[n++] = MoonModule::FixedPin{static_cast<uint8_t>(wired[i].gpio), wired[i].role};
        }
        return n;
    }

    /// A panel shows a level only as lit time, so brightness shortens the planes' windows rather than scaling values.
    bool dimsByTime() const MM_NONBLOCKING override { return true; }
    /// The planes carry the corrected values as they are, so the correction applies the curve.
    bool appliesCurve() const override { return false; }

private:
    /// The bit-plane geometry for a layout of `width` by `height` at the selected scan and depth.
    Hub75Geometry geometry(uint16_t width, uint16_t height) const {
        Hub75Geometry g;
        g.width = width;
        g.height = height;
        g.scanRate = kScanRates[owner_->scanSel < kScanCount ? owner_->scanSel : 0];
        g.bitDepth = owner_->bitDepth;
        return g;
    }

    // One board's HUB75 wiring in ribbon order; -1 leaves that line to the user.
    struct BoardPins {
        const char* label;
        int8_t r1, g1, b1, r2, g2, b2;
        int8_t a, b, c, d, e;
        int8_t clk, lat, oe;
        bool (*onThisChip)();   // null = every chip
        bool editable;   // false on a published map: those lines are soldered
    };

    /// Whether this chip is an S3, as the platform names it; likewise for P4 and S31, since inferring the chip from GPIO counts is a guess dressed as a test.
    static bool chipIsS3()  { return platform::isEsp32S3; }
    static bool chipIsP4()  { return platform::isEsp32P4; }
    static bool chipIsS31() { return platform::isEsp32S31; }

    /// The boards this output knows, and the per-chip generic sets, which are editable free pins from gpio-usage.md rather than boards.
    static constexpr BoardPins kBoards[] = {
        // MoonHub75 PCB (Lilygo T7-S3). MOONHUB75/README.md names the upper half R0/G0/B0.
        {"MoonHub75",   1,  5,  6,   7, 13,  9,  16, 48, 47, 21, 38,  18,  8,  4, chipIsS3, false},
        // Adafruit MatrixPortal S3, from the board's own CircuitPython pins.c.
        {"MatrixPortal S3", 42, 41, 40,  38, 39, 37,  45, 36, 48, 35, 21,   2, 47, 14, chipIsS3, false},
        // Waveshare SKU 34422: WLED's array ends LAT,OE,CLK, so reading ...CLK,LAT,OE swaps three.
        {"Waveshare RGB Matrix", 4,  5,  6,   7, 15, 16,  18,  8,  3, 42,  9,  41, 40,  2, chipIsS3, false},
        // Generic sets: the first fourteen of each chip's free GPIOs, in ribbon order.
        {"S3 generic",  4,  5,  6,   7,  8,  9,  10, 11, 12, 13, 14,  15, 16, 17, chipIsS3, true},
        {"P4 generic", 20, 21, 22,  23, 24, 25,  26, 27, 32, 33, 39,  40, 41, 42, chipIsP4, true},
        // The S31's free set is board-specific, so it ships unset rather than invented.
        {"S31 generic", -1, -1, -1,  -1, -1, -1,  -1, -1, -1, -1, -1,  -1, -1, -1, chipIsS31, true},
        // Custom keeps whatever is in the fields: the escape hatch for a hand-wired panel.
        {"Custom",     -1, -1, -1,  -1, -1, -1,  -1, -1, -1, -1, -1,  -1, -1, -1, nullptr, true},
    };
    static constexpr uint8_t kBoardCount = sizeof(kBoards) / sizeof(kBoards[0]);

    /// Whether `board` is offered on this chip; a desktop is none of the three chips and offers every row, silicon sees its own.
    static bool offeredHere(const BoardPins& board) {
        constexpr bool anyKnownChip = platform::isEsp32S3 || platform::isEsp32P4 || platform::isEsp32S31;
        if constexpr (anyKnownChip) return !board.onThisChip || board.onThisChip();
        else return true;   // an else, so MSVC finds no unreachable return on silicon
    }

    /// Offer the boards that make sense on this chip: a P4-generic row on an S3 would be fourteen pins that chip lacks.
    void buildBoardOptions() {
        uint8_t& boardSel = owner_->boardSel;
        const char* current = (boardSel < boardOptionCount_) ? boardOptions_[boardSel] : nullptr;
        boardOptionCount_ = 0;
        for (uint8_t i = 0; i < kBoardCount; i++) {
            if (!offeredHere(kBoards[i])) continue;
            boardOptions_[boardOptionCount_] = kBoards[i].label;
            boardIndex_[boardOptionCount_] = i;
            boardOptionCount_++;
        }
        // Re-point by LABEL, so a filtered list cannot silently select a different board.
        uint8_t sel = 0;
        if (current) {
            for (uint8_t k = 0; k < boardOptionCount_; k++) {
                if (std::strcmp(boardOptions_[k], current) == 0) { sel = k; break; }
            }
        }
        boardSel = sel;
    }

    // One HUB75 line as wired: the GPIO and the ribbon name it carries.
    struct Line { int8_t gpio; const char* role; };
    static constexpr uint8_t kLineCount = 14;

    /// The set lines in ribbon order: the one home for which GPIOs this output holds.
    uint8_t lines(Line* out) const {
        const Hub75Driver& d = *owner_;
        const Line all[kLineCount] = {
            {d.r1, "r1"}, {d.g1, "g1"}, {d.b1, "b1"}, {d.r2, "r2"}, {d.g2, "g2"}, {d.b2, "b2"},
            {d.addrA, "a"}, {d.addrB, "b"}, {d.addrC, "c"}, {d.addrD, "d"}, {d.addrE, "e"},
            {d.clk, "clk"}, {d.lat, "lat"}, {d.oe, "oe"}};
        uint8_t n = 0;
        for (const Line& l : all) if (l.gpio >= 0) out[n++] = l;
        return n;
    }

    /// Are the pin controls the user's to edit, for the selected board?
    bool pinsEditable() const {
        const uint8_t boardSel = owner_->boardSel;
        if (boardSel >= boardOptionCount_) return true;   // no board resolved: show every pin
        return kBoards[boardIndex_[boardSel]].editable;
    }

    /// The first line on a pin the chip refuses, with its role and why, or -1 when all are usable.
    int8_t unusableLine(const char*& role, const char*& why) const {
        Line wired[kLineCount];
        for (uint8_t i = 0, count = lines(wired); i < count; i++) {
            const uint8_t gpio = static_cast<uint8_t>(wired[i].gpio);
            const char* refusal = platform::gpioRefusal(gpio);
            // Every line is driven, so an input-only pad is as unusable as a reserved one.
            if (!refusal && !platform::gpioCapability(gpio).outputCapable)
                refusal = "is an input-only pin on this chip";
            if (!refusal) continue;
            role = wired[i].role;
            why = refusal;
            return wired[i].gpio;
        }
        return -1;
    }

    /// Write the chosen board's map into the pin controls; Custom leaves the pins as they are, so switching to it keeps an edit.
    void applyBoard() {
        Hub75Driver& d = *owner_;
        if (d.boardSel >= boardOptionCount_) return;
        const BoardPins& b = kBoards[boardIndex_[d.boardSel]];
        if (std::strcmp(b.label, "Custom") == 0) return;
        d.r1 = b.r1; d.g1 = b.g1; d.b1 = b.b1;
        d.r2 = b.r2; d.g2 = b.g2; d.b2 = b.b2;
        d.addrA = b.a; d.addrB = b.b; d.addrC = b.c; d.addrD = b.d; d.addrE = b.e;
        d.clk = b.clk; d.lat = b.lat; d.oe = b.oe;
    }

    const char* boardOptions_[kBoardCount] = {};
    uint8_t boardIndex_[kBoardCount] = {};
    uint8_t boardOptionCount_ = 0;
    bool prefilled_ = false;   // the board map is written once, not on every re-render

    static constexpr uint8_t kScanCount = 3;
    static constexpr const char* kScanOptions[kScanCount] = {"1/8", "1/16", "1/32"};
    static constexpr uint8_t kScanRates[kScanCount] = {8, 16, 32};

    /// Convert a Pin control's value to the platform seam's pin type; -1 for unset becomes 0xFFFF, since a P4 GPIO number can exceed int8_t's range.
    static uint16_t toPin(int8_t p) {
        return p < 0 ? 0xFFFF : static_cast<uint16_t>(p);
    }

    platform::Hub75Handle hub_{};
    Hub75Geometry geo_{};
    uint16_t lastRefresh_ = 0xFFFF;      // not 0: the first real 0 must still publish
    char status_[64] = {};
    char refreshStr_[16] = {};
};

/// Make the bus output, the one both bus backends use.
inline Hub75Output* makeHub75BusOutput() { return new Hub75BusOutput(); }

} // namespace mm
