#pragma once
/// What every HUB75 test file shares: a guard for each process-global test seam, putting it back however a case ends, and the bench a driver is wired on.

#include <cstddef>
#include <cstdint>

#include "doctest.h"
#include "correction_presets.h"
#include "light/drivers/Correction.h"
#include "light/drivers/Hub75Driver.h"
#include "light/layers/Buffer.h"
#include "light/layers/Layer.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/PanelsLayout.h"
#include "platform/platform.h"

/// The RGB seam records for the case, and is left closed and out of recording after it.
struct RecordGuard {
    /// Record from here on.
    RecordGuard() { mm::platform::setTestRgbMatrix(true); }
    /// Close the seam and stop recording.
    ~RecordGuard() { mm::platform::setTestRgbMatrix(false); }
};

/// The PIO seam records for the case, and is left closed and out of recording after it.
struct PioRecordGuard {
    /// Record from here on.
    PioRecordGuard() { mm::platform::setTestHub75Pio(true); }
    /// Close the seam and stop recording.
    ~PioRecordGuard() { mm::platform::setTestHub75Pio(false); }
};

/// The GPIO seam's recorder, on while a test runs: it records the opens and pushes and stands in for the panels, as the PIO recorder does for its engine.
struct GpioRecordGuard {
    /// Record from here on.
    GpioRecordGuard() { mm::platform::setTestHub75Gpio(true); }
    /// Close the seam and stop recording.
    ~GpioRecordGuard() { mm::platform::setTestHub75Gpio(false); }
};

/// The PIO check stands in for a host running `release`, with or without /dev/pio0, then reads the real one again.
struct PioHost {
    /// Stand in for a host on `release`, with /dev/pio0 when `device`.
    PioHost(const char* release, bool device) { mm::platform::setTestPioHost(release, device); }
    /// Read the real host again.
    ~PioHost() { mm::platform::setTestPioHost(nullptr, false); }
};

/// The PIO availability stands forced as `available` for the case, then the host decides again.
struct PioAvailable {
    /// Force the availability as `available`: 0 unavailable, 1 available.
    explicit PioAvailable(int available) { mm::platform::setTestHub75PioAvailable(available); }
    /// Let the host decide again.
    ~PioAvailable() { mm::platform::setTestHub75PioAvailable(-1); }
};

/// The desktop's bus port is open for the case, so LCD_CAM is the first backend offered, as on an ESP32, and closed after it.
struct BusPort {
    /// Open the bus port.
    BusPort() { mm::platform::setTestHub75(true); }
    /// Close the bus port.
    ~BusPort() { mm::platform::setTestHub75(false); }
};

/// The process's real-time capability stands in as `granted`, then is granted again.
struct Realtime {
    /// Stand in for the capability as `granted`.
    explicit Realtime(bool granted) { mm::platform::setTestRgbMatrixRealtime(granted); }
    /// Grant it again.
    ~Realtime() { mm::platform::setTestRgbMatrixRealtime(true); }
};

/// The seam's stand-in answers `n` chains for any mapping, then the three it starts with.
struct ChainLimit {
    /// Answer `n` chains for any mapping.
    explicit ChainLimit(uint8_t n) { mm::platform::setTestRgbMatrixMaxChains(n); }
    /// Answer three again.
    ~ChainLimit() { mm::platform::setTestRgbMatrixMaxChains(3); }
};

/// A Hub75Driver on a layer of the layouts a case adds, wired as production wires it; declared in destruction-safe order, the layouts before what reads them.
struct Hub75Bench {
    mm::Layouts layouts;         ///< the layouts the layer reads
    mm::GridLayout grid;         ///< a raster, for a case that adds it
    mm::PanelsLayout panels;     ///< panels in cabling order, for a case that adds it
    mm::Layer layer;             ///< the layer the driver reads
    mm::Buffer source;           ///< the rendered frame the driver corrects
    mm::Hub75Driver driver;      ///< the driver under test

    /// Size the layer from the layouts added, hold `lights` lights of RGB, and bind the driver to both.
    void wire(mm::nrOfLightsType lights) {
        layouts.applyState();
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.applyState();
        REQUIRE(source.allocate(lights, 3));
        driver.setLayer(&layer);
        driver.setSourceBuffer(&source);
    }

    /// Open the panels with a linear, full-brightness correction, built in place, so a frame's values read straight.
    void open() {
        mm::test::rebuildFromPreset(driver.correctionForTest(), 255, mm::test::PresetOrder::RGB);
        driver.applyState();
        driver.prepare();
    }

    /// Paint light `i` in the rendered frame.
    void paint(mm::nrOfLightsType i, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t* p = source.data() + static_cast<size_t>(i) * 3;
        p[0] = r; p[1] = g; p[2] = b;
    }
};

/// A 32-channel moving head with its color at channels 9 to 11, so correcting one light writes all 32.
struct WideProfile {
    mm::ChannelRole roles[32] = {};   ///< every channel's role, unused but the three
    /// Red, green and blue at 9, 10 and 11.
    WideProfile() {
        roles[9] = mm::ChannelRole::Red;
        roles[10] = mm::ChannelRole::Green;
        roles[11] = mm::ChannelRole::Blue;
    }
};
