#pragma once

#include <cstddef>
#include <cstdint>
#include "core/module/Control.h"          // ControlList: an output appends its own controls into the driver's list
#include "core/module/MoonModule.h"       // MoonModule::FixedPin: the lines an output reports to the pin map
#include "light/drivers/Hub75Color.h"     // DepthRange: the bit planes an output drives
#include "light/drivers/LedPeripheral.h"  // LedHwBlock: the sibling claim guard's vocabulary
#include "light/util/light_types.h"       // nrOfLightsType
#include "platform/nonblocking.h"        // MM_NONBLOCKING

namespace mm {

class Hub75Driver;   // the orchestrator; an output reads the settings it binds through this back-pointer

/// The last step of a HUB75 frame, behind a runtime strategy interface, as LedPeripheral is for ParallelLedDriver.
///
/// Hub75Driver owns the shared controls, the lifecycle and the correction into one RGB frame in light order. The output owns what happens to that frame: its controls, canvas, hardware claim, and the call that puts it on the panel.
///
/// Not a hot-path boundary: show() is called once per frame, not per light.
class Hub75Output {
public:
    /// Virtual, since the driver owns its output through this interface.
    virtual ~Hub75Output() = default;

    /// Bind the output to its driver, which it reads the settings it binds through.
    void attach(Hub75Driver* owner) { owner_ = owner; }

    /// Append the controls shown above the driver's `peripheral` select, though they follow it, so the ESP32 card keeps main's order: board and pins first.
    virtual void addWiringControls(ControlList& /*controls*/) {}
    /// The range the driver's one `bitDepth` takes on this output.
    virtual DepthRange depthRange() const = 0;
    /// Append the controls this output shows below the `peripheral` select.
    virtual void addControls(ControlList& /*controls*/) {}
    /// Apply a change to control `name`, answering whether the card must be rebuilt.
    virtual bool onControlChanged(const char* /*name*/) { return false; }
    /// Whether a change to control `name` needs the output reopened.
    virtual bool affectsPrepare(const char* /*name*/) const { return false; }

    /// The panel geometry an output drives: rows and columns of one panel, and panels daisy-chained on one output. Only the Pi outputs have one; the bus output takes its canvas from the layout.
    struct PanelGeometry {
        uint8_t rows = 0;
        uint16_t cols = 0;
        uint8_t chain = 0;
    };
    /// Read this output's panel geometry, false when it has none.
    virtual bool panelGeometry(PanelGeometry& /*geometry*/) const { return false; }
    /// Write a panel geometry carried across an output swap; a no-op for an output with none.
    virtual void setPanelGeometry(const PanelGeometry& /*geometry*/) {}

    /// The bytes a frame for a layout of `width` by `height` takes in the platform's buffer, which decides the backends offered; 0 when unbounded or not valid.
    virtual size_t frameBytes(uint16_t /*width*/, uint16_t /*height*/) const { return 0; }
    /// Bring the panel up for a layout of `width` by `height`; null when it runs, else why not, which the driver shows as an error.
    virtual const char* open(uint16_t width, uint16_t height) = 0;
    /// The status line while the panel runs.
    virtual const char* describe() const = 0;
    /// What degrades the running panel or the layout's fit to it, in full, shown as a warning in place of describe(), or null.
    virtual const char* warning() const { return nullptr; }
    /// The lights on the canvas while it runs: the length of the RGB frame show() takes.
    virtual nrOfLightsType lights() const MM_NONBLOCKING = 0;
    /// Put one corrected RGB frame of lights() lights on the panel, at `brightness` for an output that dims by time.
    virtual void show(const uint8_t* rgb, uint8_t brightness) MM_NONBLOCKING = 0;
    /// Refresh the output's measured readouts, once a second while it runs.
    virtual void tick1s() MM_NONBLOCKING {}
    /// Stop the panel and release its hardware; harmless when it is not running.
    virtual void close() = 0;

    /// The hardware block this output drives, which the sibling claim guard refuses to double up.
    virtual LedHwBlock hwBlock() const = 0;
    /// The bytes the output holds beside the driver's frame, which the driver's memory readout adds.
    virtual size_t heapBytes() const { return 0; }
    /// Lines the output holds that have no visible pin control, for the pin map.
    virtual uint8_t fixedPins(MoonModule::FixedPin* /*out*/, uint8_t /*max*/) const { return 0; }
    /// Whether the panel shows brightness as lit time, so the correction keeps full values.
    virtual bool dimsByTime() const MM_NONBLOCKING = 0;
    /// Whether the output applies the brightness curve itself, so the correction bakes Linear.
    virtual bool appliesCurve() const = 0;

protected:
    Hub75Driver* owner_ = nullptr;
};

} // namespace mm
