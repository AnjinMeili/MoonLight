// hub75_gpio_backend_hw.cpp -- GpioBackend on real Pi 3/4 hardware.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The CPU bit-bang + PWM pulser backend: GPIO, PWM and clock-manager
// registers mapped through /dev/mem, the refresh thread clocking columns
// straight into GPSET0/GPCLR0, the PWM pulser (hub75_gpio_pwm.h) timing
// OE. Written from scratch from the BCM2835/BCM2711 datasheets and the
// published timing numbers; no hzeller code is adapted (every file in
// its lib/ carries a GPL-2.0-only header -- see MOONLIGHT-INTEGRATION.md
// for the license check). The architecture -- a realtime thread
// bit-banging precomputed GPIO words while a hardware PWM pulse times
// OE -- follows the reference design's shape, as the research report
// describes it; the code here is original.
//
// Register map (BCM2835 ARM Peripherals; offsets within each block, the
// SoC's peripheral base added by the model detection):
//
//   GPIO block (base + 0x200000): GPFSEL0-5 0x00-0x14, GPSET0 0x1C,
//     GPCLR0 0x28, GPLEV0 0x34.
//   PWM block (base + 0x20C000): see hub75_gpio_pwm.h.
//   Clock manager (base + 0x101000): see hub75_gpio_pwm.h.
//
// The dump loop, per (plane, row-group) -- the reference design's order:
//
//   1. Clock the group's columns in while the previous OE pulse runs:
//      per column, three GPIO writes (clear the 0-data bits and the
//      clock, set the 1-data bits, raise the clock -- the rising edge
//      clocks the data in; falling-edge panels invert the sequence).
//   2. Wait for the previous OE pulse to finish.
//   3. In the dark: drop the clock, set the row address, strobe.
//   4. Fire the OE pulse for this plane; loop back to 1.
//
// The dark-first setup order (NOTE.md rule 1) is structural here:
// open() writes the dark levels, then makes the pins outputs; start()
// configures the PWM (idle high), then muxes OE to it. The stub backend
// logs the same four steps in the same order, so the engine test pins
// the order on a host with no hardware.

#include "platform/rpi/hub75_gpio_backend.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "platform/rpi/hub75_gpio_pins.h"
#include "platform/rpi/hub75_gpio_pwm.h"

namespace mm::platform {

namespace {

// GPIO block register indices (offset / 4).
constexpr uint32_t kGpioSet0Idx = 0x1C / 4;
constexpr uint32_t kGpioClr0Idx = 0x28 / 4;
constexpr uint32_t kGpioLev0Idx = 0x34 / 4;

// Block offsets from the peripheral base (BCM2835 section 1.2.3).
constexpr uint32_t kGpioBlockOffset = 0x200000;
constexpr uint32_t kPwmBlockOffset = 0x20C000;
constexpr uint32_t kClkBlockOffset = 0x101000;
constexpr size_t kMapSize = 4096;   // one page per block

// Every GPIO the board drives, for the direction setup.
const uint8_t kBoardPins[] = {
    gpio_shared::kOe, gpio_shared::kClk, gpio_shared::kStb,
    gpio_shared::kA, gpio_shared::kB, gpio_shared::kC, gpio_shared::kD, gpio_shared::kE,
    kGpioChainPins[0].r1, kGpioChainPins[0].g1, kGpioChainPins[0].b1,
    kGpioChainPins[0].r2, kGpioChainPins[0].g2, kGpioChainPins[0].b2,
    kGpioChainPins[1].r1, kGpioChainPins[1].g1, kGpioChainPins[1].b1,
    kGpioChainPins[1].r2, kGpioChainPins[1].g2, kGpioChainPins[1].b2,
    kGpioChainPins[2].r1, kGpioChainPins[2].g1, kGpioChainPins[2].b1,
    kGpioChainPins[2].r2, kGpioChainPins[2].g2, kGpioChainPins[2].b2,
};

// Read a small /proc or /sys file into `out` (up to the first NUL for
// device-tree strings); false when the file cannot be read.
bool readSmallFile(const char* path, std::string& out) {
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return false;
    char buf[256];
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    if (n == 0) return false;
    buf[n] = '\0';
    out.assign(buf, strnlen(buf, n));   // the device-tree model is NUL-terminated
    return true;
}

// The Revision value from /proc/cpuinfo, as a hex number; false when
// the file has no Revision line.
bool readCpuinfoRevision(uint32_t& revision) {
    FILE* f = std::fopen("/proc/cpuinfo", "r");
    if (f == nullptr) return false;
    char line[256];
    bool found = false;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (std::strncmp(line, "Revision", 8) == 0) {
            const char* colon = std::strchr(line, ':');
            if (colon != nullptr) {
                revision = static_cast<uint32_t>(std::strtoul(colon + 1, nullptr, 16));
                found = true;
            }
            break;
        }
    }
    std::fclose(f);
    return found;
}

// Whether the onboard analogue audio driver is loaded: it owns the PWM
// the OE pulser needs, so the backend refuses to open while it runs.
bool audioDriverLoaded() {
    FILE* f = std::fopen("/proc/modules", "r");
    if (f == nullptr) return false;
    char line[256];
    bool loaded = false;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (std::strncmp(line, "snd_bcm2835 ", 12) == 0) {
            loaded = true;
            break;
        }
    }
    std::fclose(f);
    return loaded;
}

}  // namespace

Hub75GpioModel hub75GpioDetectModel(const char* deviceTreeModel, const char* cpuinfoRevision) {
    // The device-tree model string names the board outright; it wins.
    if (deviceTreeModel != nullptr) {
        if (std::strstr(deviceTreeModel, "Raspberry Pi 5") != nullptr) return Hub75GpioModel::Pi5;
        if (std::strstr(deviceTreeModel, "Raspberry Pi 4") != nullptr) return Hub75GpioModel::Pi4;
        if (std::strstr(deviceTreeModel, "Raspberry Pi 3") != nullptr) return Hub75GpioModel::Pi3;
        // Compute modules name the number after "Compute Module": a CM4
        // is BCM2711, a CM3/CM3+ is BCM2837.
        if (std::strstr(deviceTreeModel, "Compute Module 4") != nullptr) return Hub75GpioModel::Pi4;
        if (std::strstr(deviceTreeModel, "Compute Module 5") != nullptr) return Hub75GpioModel::Pi5;
        if (std::strstr(deviceTreeModel, "Compute Module 3") != nullptr) return Hub75GpioModel::Pi3;
    }
    // Fallback: the cpuinfo Revision's new-style type field (bits 4..11;
    // bit 23 marks the new scheme). Old-style revisions are Pi 1/2.
    if (cpuinfoRevision != nullptr && cpuinfoRevision[0] != '\0') {
        const uint32_t rev = static_cast<uint32_t>(std::strtoul(cpuinfoRevision, nullptr, 16));
        if ((rev & (1u << 23)) != 0) {
            const uint32_t type = (rev >> 4) & 0xFF;
            switch (type) {
                case 0x08:   // 3B
                case 0x0A:   // CM3
                case 0x0C:   // 3A+
                case 0x0D:   // 3B+
                case 0x0E:   // 3A+
                case 0x10:   // CM3+
                    return Hub75GpioModel::Pi3;
                case 0x11:   // 4B
                case 0x13:   // 400
                case 0x14:   // CM4
                case 0x15:   // CM4S
                    return Hub75GpioModel::Pi4;
                case 0x17:   // 5B
                case 0x18:   // CM5
                case 0x19:   // 500
                    return Hub75GpioModel::Pi5;
                default:
                    break;
            }
        }
    }
    return Hub75GpioModel::Unknown;
}

uint32_t hub75GpioPeripheralBase(Hub75GpioModel model) {
    switch (model) {
        case Hub75GpioModel::Pi3: return 0x3F000000;
        case Hub75GpioModel::Pi4: return 0xFE000000;
        default: return 0;
    }
}

Hub75GpioModel hub75GpioHostModel() {
    // Read once; the model cannot change under a running process.
    static const Hub75GpioModel model = []() {
        std::string dtModel;
        std::string revStr;
        uint32_t rev = 0;
        const bool haveModel = readSmallFile("/proc/device-tree/model", dtModel);
        if (readCpuinfoRevision(rev)) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%x", rev);
            revStr = buf;
        }
        return hub75GpioDetectModel(
            haveModel ? dtModel.c_str() : nullptr,
            revStr.empty() ? nullptr : revStr.c_str());
    }();
    return model;
}

const char* hub75GpioHostRefusal() {
    switch (hub75GpioHostModel()) {
        case Hub75GpioModel::Pi3:
        case Hub75GpioModel::Pi4:
            return nullptr;
        case Hub75GpioModel::Pi5:
            return "this is a Raspberry Pi 5: use the Pi PIO output, not Pi GPIO";
        default:
            return "this host is not a Raspberry Pi 3 or 4";
    }
}

namespace {

class HwBackend : public GpioBackend {
  public:
    bool open(std::string& error) override {
        if (opened_) {
            error = "GPIO backend already open";
            return false;
        }
        // The model decides the peripheral base; a Pi 5 is directed to
        // the PIO output, anything else is refused outright.
        model_ = hub75GpioHostModel();
        if (model_ == Hub75GpioModel::Pi5) {
            error = "this is a Raspberry Pi 5: use the Pi PIO output, not Pi GPIO";
            model_ = Hub75GpioModel::Unknown;
            return false;
        }
        const uint32_t periBase = hub75GpioPeripheralBase(model_);
        if (periBase == 0) {
            error = "this host is not a Raspberry Pi 3 or 4 (no BCM2837/BCM2711 model detected)";
            model_ = Hub75GpioModel::Unknown;
            return false;
        }
        if (audioDriverLoaded()) {
            error = "the onboard audio driver (snd_bcm2835) owns the PWM the OE pulser needs: "
                    "disable onboard audio (dtparam=audio=off) and try again";
            model_ = Hub75GpioModel::Unknown;
            return false;
        }
        memFd_ = ::open("/dev/mem", O_RDWR | O_SYNC);
        if (memFd_ < 0) {
            error = "cannot open /dev/mem (run as root, or grant CAP_SYS_RAWIO)";
            model_ = Hub75GpioModel::Unknown;
            return false;
        }
        gpio_ = mapBlock(periBase + kGpioBlockOffset);
        pwm_ = mapBlock(periBase + kPwmBlockOffset);
        clk_ = mapBlock(periBase + kClkBlockOffset);
        if (gpio_ == nullptr || pwm_ == nullptr || clk_ == nullptr) {
            error = "cannot map the GPIO/PWM registers through /dev/mem";
            unmapAll();
            ::close(memFd_);
            memFd_ = -1;
            model_ = Hub75GpioModel::Unknown;
            return false;
        }

        // Dark first (NOTE.md rule 1): the dark levels go to the output
        // registers BEFORE any pin becomes an output, so the panel
        // never sees a lit row at startup. Writing GPSET/GPCLR sets
        // the output latch even while the pins are still inputs.
        writeDarkLevels();
        for (uint8_t pin : kBoardPins) setGpioFunction(pin, kGpioGpioOutput);

        // The row-address table: address 0..31 as GPIO masks.
        for (int r = 0; r < 32; r++)
            addrTable_[r] = gpioAddressMaskForRow(static_cast<uint8_t>(r));

        opened_ = true;
        return true;
    }

    bool start(const GpioSubmitConfig& config, std::string& error) override {
        if (!opened_) {
            error = "GPIO backend not open";
            return false;
        }
        if (config.chainWidth == 0 || config.scanRows == 0 || config.pairs == 0) {
            error = "GPIO submit config has a zero dimension";
            return false;
        }
        if (config.bitDepth < 2 || config.bitDepth > 6) {
            error = "GPIO bitDepth must be 2..6";
            return false;
        }
        if (config.lsbNs < kGpioPwmLsbMinNs || config.lsbNs > kGpioPwmLsbMaxNs) {
            error = "GPIO LSB time must be 50..3000 ns";
            return false;
        }
        if (config.slowdown < -1 || config.slowdown > 4) {
            error = "GPIO slowdown must be -1 (auto) or 0..4";
            return false;
        }
        config_ = config;
        slowdown_ = hub75GpioResolveSlowdown(config.slowdown, model_);
        groupWords_ = static_cast<size_t>(config.chainWidth) * config.pairs;
        groups_ = static_cast<size_t>(config.bitDepth) * config.scanRows;

        // Configure the PWM (idle high = dark), THEN mux OE to it: the
        // pin is high throughout, GPIO-driven then PWM-driven.
        if (!pulser_.configure(pwm_, clk_, config.lsbNs, error)) {
            return false;
        }
        setGpioFunction(gpio_shared::kOe, kGpioGpioAlt5);
        started_ = true;
        return true;
    }

    int submitFrame(const uint32_t* words, size_t wordCount) override {
        if (!started_ || words == nullptr) return -1;
        const size_t expect = groups_ * groupWords_;
        const size_t total = wordCount < expect ? wordCount : expect;
        volatile uint32_t* gpio = gpio_;
        const uint32_t colourMask = kGpioColourMask;
        const uint32_t clkBit = kGpioClkBit;
        const int slowdown = slowdown_;
        const bool falling = config_.clockFallingEdge;

        for (size_t g = 0; g < groups_; g++) {
            const size_t base = g * groupWords_;
            if (base >= total) break;
            const size_t n = groupWords_ < total - base ? groupWords_ : total - base;
            // 1. Clock this group's columns in while the previous OE
            //    pulse runs. Three GPIO writes per column.
            if (!falling) {
                for (size_t i = 0; i < n; i++) {
                    const uint32_t data = words[base + i] & colourMask;
                    gpio[kGpioClr0Idx] = (~data & colourMask) | clkBit;
                    gpio[kGpioSet0Idx] = data;
                    for (int s = 0; s < slowdown; s++) (void)gpio[kGpioLev0Idx];
                    gpio[kGpioSet0Idx] = clkBit;   // rising edge clocks the data in
                }
            } else {
                for (size_t i = 0; i < n; i++) {
                    const uint32_t data = words[base + i] & colourMask;
                    gpio[kGpioSet0Idx] = data | clkBit;
                    gpio[kGpioClr0Idx] = ~data & colourMask;
                    for (int s = 0; s < slowdown; s++) (void)gpio[kGpioLev0Idx];
                    gpio[kGpioClr0Idx] = clkBit;   // falling edge clocks the data in
                }
            }
            // 2. The previous row's OE must be finished before the latch.
            if (!pulser_.waitPulseFinished()) pulseTimeouts_++;
            // 3. In the dark: drop the clock, set the row address, strobe.
            gpio[kGpioClr0Idx] = clkBit;
            const uint32_t addr = addrTable_[g % config_.scanRows];
            gpio[kGpioClr0Idx] = kGpioAddressMask & ~addr;
            gpio[kGpioSet0Idx] = addr;
            gpio[kGpioSet0Idx] = kGpioStbBit;
            (void)gpio[kGpioLev0Idx];   // the latch's setup time
            gpio[kGpioClr0Idx] = kGpioStbBit;
            // 4. Fire the OE pulse for this plane; the next group's
            //    clock-in runs while it is lit.
            pulser_.sendPulse(static_cast<uint8_t>(g / config_.scanRows));
        }
        return 0;
    }

    bool blankAndStop(const uint32_t* blankWords, size_t blankWordCount,
                      std::string& /*error*/) override {
        // Blank first (NOTE.md rule 2): dump the blank frame, wait for
        // it to drain, then stop. The blank frame's colour bits are
        // all clear, so its OE pulses light nothing.
        if (started_ && blankWords != nullptr) {
            (void)submitFrame(blankWords, blankWordCount);
            if (!pulser_.waitPulseFinished()) pulseTimeouts_++;
        }
        started_ = false;
        if (gpio_ != nullptr) {
            // OE back to a GPIO output driving high (dark) before the
            // PWM stops, so the pin's level is defined throughout.
            setGpioFunction(gpio_shared::kOe, kGpioGpioOutput);
            writeDarkLevels();
        }
        pulser_.stop();
        return true;
    }

    void close() override {
        if (gpio_ != nullptr) {
            // Blank before stopping, even on the error path: OE back
            // to an output driving high, the rest dark, then release.
            setGpioFunction(gpio_shared::kOe, kGpioGpioOutput);
            writeDarkLevels();
        }
        pulser_.stop();
        started_ = false;
        opened_ = false;
        unmapAll();
        if (memFd_ >= 0) {
            ::close(memFd_);
            memFd_ = -1;
        }
        model_ = Hub75GpioModel::Unknown;
        slowdown_ = 1;
    }

    const char* name() const override { return "GPIO+PWM (Pi 3/4)"; }

    Hub75GpioModel model() const override { return model_; }
    int resolvedSlowdown() const override { return slowdown_; }
    uint64_t pulseTimeouts() const override { return pulseTimeouts_; }

  private:
    volatile uint32_t* mapBlock(uint32_t physAddr) {
        void* map = ::mmap(nullptr, kMapSize, PROT_READ | PROT_WRITE, MAP_SHARED,
                           memFd_, static_cast<off_t>(physAddr));
        if (map == MAP_FAILED) return nullptr;
        return static_cast<volatile uint32_t*>(map);
    }

    void unmapAll() {
        if (gpio_ != nullptr) {
            ::munmap(const_cast<uint32_t*>(gpio_), kMapSize);
            gpio_ = nullptr;
        }
        if (pwm_ != nullptr) {
            ::munmap(const_cast<uint32_t*>(pwm_), kMapSize);
            pwm_ = nullptr;
        }
        if (clk_ != nullptr) {
            ::munmap(const_cast<uint32_t*>(clk_), kMapSize);
            clk_ = nullptr;
        }
    }

    // The dark levels: OE high (blanked), colour and address 0, latch
    // and clock low. Written to the output registers; safe to call
    // while the pins are inputs (it sets the latch) or outputs.
    void writeDarkLevels() {
        gpio_[kGpioSet0Idx] = kGpioOeBit;
        gpio_[kGpioClr0Idx] = kGpioColourMask | kGpioAddressMask | kGpioStbBit | kGpioClkBit;
    }

    void setGpioFunction(uint8_t pin, uint32_t mode) {
        const int reg = gpioGpfselReg(pin);
        const int shift = gpioGpfselShift(pin);
        gpio_[reg] = (gpio_[reg] & ~(7u << shift)) | ((mode & 7u) << shift);
    }

    int memFd_ = -1;
    volatile uint32_t* gpio_ = nullptr;
    volatile uint32_t* pwm_ = nullptr;
    volatile uint32_t* clk_ = nullptr;
    GpioPwmPulser pulser_;
    Hub75GpioModel model_ = Hub75GpioModel::Unknown;
    GpioSubmitConfig config_{};
    uint32_t addrTable_[32] = {};
    size_t groupWords_ = 0;
    size_t groups_ = 0;
    int slowdown_ = 1;
    bool opened_ = false;
    bool started_ = false;
    uint64_t pulseTimeouts_ = 0;
};

}  // namespace

GpioBackend* createGpioHwBackend() { return new HwBackend(); }

}  // namespace mm::platform
