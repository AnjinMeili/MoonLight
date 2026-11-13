/// The Raspberry Pi platform layer behind the RGB seam (platform.h): a recorder for the tests on every host, and the checks a Pi output runs before it opens.
///
/// A build that lacks rpi-rgb-led-matrix refuses every open with that reason, so a Pi output says why its panels stay dark.

#include "platform/platform.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(__linux__)
    #include <unistd.h>
#endif

namespace mm::platform {

namespace {

std::atomic<uint16_t> width_{0};
std::atomic<uint16_t> height_{0};
std::atomic<uint16_t> fps_{0};
std::atomic<uint8_t>  brightness_{255};
std::atomic<bool>     cie1931_{true};   // the library's own default: its table on
std::string lastError_;
std::string warning_;          // what degrades the running panels, joined so each one shows
bool onPio_ = false;           // whether the open panels run on PIO, as the open decided
bool testRealtime_ = true;     // the recorder's stand-in for the process's real-time priority

// The triple buffer, lock-free with one producer, the driver: ready_ holds a slot index in its low bits and kFresh when the producer has filled it since the consumer last took it.
constexpr uint8_t kFresh = 4;
std::atomic<uint8_t> ready_{0};
uint8_t writeSlot_ = 1;

// The frame slots the consumer takes from, on the heap for the process's life, so a consumer on another thread never reads freed slots at exit.
struct Shared {
    std::vector<uint8_t> slots[3];
};
Shared* const shared_ = new Shared;

bool recording_ = false;
uint8_t testMaxChains_ = 3;   // what the recording stand-in answers for any mapping
RgbMatrixOptions testOptions_;
std::string testMapping_, testPanelType_;   // the recorded options' strings, owned past the caller's
size_t testFrames_ = 0;
uint8_t testReadSlot_ = 2;   // the test's consumer of the triple buffer

size_t frameBytes() MM_NONBLOCKING { return static_cast<size_t>(width_.load()) * height_.load() * 3; }

bool fail(const std::string& why) {
    lastError_ = why;
    warning_.clear();   // a refused open runs no panel for a warning to describe
    onPio_ = false;
    return false;
}

// A kernel release's first three numbers, compared in order.
struct KernelVersion {
    unsigned major = 0, minor = 0, patch = 0;
    bool operator<(const KernelVersion& o) const {
        if (major != o.major) return major < o.major;
        if (minor != o.minor) return minor < o.minor;
        return patch < o.patch;
    }
};

// The kernels whose RP1 PIO driver stalls the library's next PIO open: 6.18.39 dropped the wait a teardown with DMA in flight relied on, and 6.18.60, the next package after 6.18.50, runs it again.
constexpr KernelVersion kPioStallFrom{6, 18, 39};
constexpr KernelVersion kPioStallUntil{6, 18, 60};

std::string testKernel_;
bool testPioDevice_ = false;
bool testPioHost_ = false;
bool pioChecked_ = false;
std::string pioRefusal_;

// What the PIO check reads: the kernel release and whether /dev/pio0 exists, from the test's stand-in or the host.
void readPioHost(std::string& release, bool& device) {
    if (testPioHost_) {
        release = testKernel_;
        device = testPioDevice_;
        return;
    }
#if defined(__linux__)
    // The release alone, as `uname -r` prints it: a whole utsname would push the caller's frame past the build's limit.
    char line[64] = {};
    if (FILE* f = std::fopen("/proc/sys/kernel/osrelease", "r")) {
        if (std::fgets(line, sizeof(line), f)) line[std::strcspn(line, "\n")] = 0;
        std::fclose(f);
    }
    release = line;
    device = access("/dev/pio0", F_OK) == 0;
#else
    release.clear();
    device = false;
#endif
}

// The reason PIO cannot serve here, or empty: a missing device first, since a Pi 3 or 4 has no RP1 whatever its kernel.
std::string pioRefusalFor(const std::string& release, bool device) {
    if (!device) return "no /dev/pio0 on this system";
    KernelVersion v;
    if (std::sscanf(release.c_str(), "%u.%u.%u", &v.major, &v.minor, &v.patch) != 3) return {};
    if (v < kPioStallFrom || !(v < kPioStallUntil)) return {};
    char why[64];
    std::snprintf(why, sizeof(why), "kernel %u.%u.%u stalls a PIO reopen: RIO only", v.major, v.minor, v.patch);
    return why;
}

// Keep the options, on PIO when `pio`, and stand in for panels of the size they wire, so a test reads what a driver opened and pushed.
bool recordOpen(const RgbMatrixOptions& o, bool pio) {
    testOptions_ = o;
    testOptions_.rp1Pio = pio;
    testMapping_ = o.mapping ? o.mapping : "";
    testPanelType_ = o.panelType ? o.panelType : "";
    testOptions_.mapping = testMapping_.c_str();
    testOptions_.panelType = testPanelType_.c_str();
    width_ = static_cast<uint16_t>(o.cols * o.chain);
    height_ = static_cast<uint16_t>(o.rows * o.parallel);
    if (frameBytes() == 0) {   // a failed open leaves no canvas, as on a Pi
        width_ = 0;
        height_ = 0;
        return fail("the panels form no canvas");
    }
    // The triple buffer a consumer takes from, so a test runs the whole handover.
    for (auto& s : shared_->slots) s.assign(frameBytes(), 0);
    ready_.store(0, std::memory_order_release);
    writeSlot_ = 1;
    testReadSlot_ = 2;
    testFrames_ = 0;
    return true;
}

// Whether the refresh thread gets real-time priority: the stand-in while recording, granted otherwise.
bool realtimeGranted() {
    if (recording_) return testRealtime_;
    return true;
}

// Add one warning clause, after any already there.
void addWarning(const char* clause) {
    if (!warning_.empty()) warning_ += "; ";
    warning_ += clause;
}

}  // namespace

bool rgbMatrixAvailable() {
    return recording_;
}

bool rgbMatrixOpen(const RgbMatrixOptions& o) {
    rgbMatrixClose();
    lastError_.clear();
    warning_.clear();
    // PIO where the PIO check refuses it opens on RIO, and the warning says why; a missing real-time priority follows it.
    bool pio = o.rp1Pio;
    if (pio) {
        if (const char* why = rgbMatrixPioRefusal()) {
            pio = false;
            addWarning(why);
        }
    }
    onPio_ = pio;
    if (!realtimeGranted()) addWarning("no real-time priority, so colors may flicker: grant CAP_SYS_NICE and reserve a core with isolcpus");
    if (recording_) return recordOpen(o, pio);
    return fail("needs a Raspberry Pi build with rpi-rgb-led-matrix");
}

uint8_t rgbMatrixMaxChains(const char* mapping) {
    if (recording_) return testMaxChains_;
    (void)mapping;
    return 0;
}

const char* rgbMatrixLastError() { return lastError_.empty() ? nullptr : lastError_.c_str(); }

const char* rgbMatrixWarning() { return warning_.empty() ? nullptr : warning_.c_str(); }

uint16_t rgbMatrixWidth() MM_NONBLOCKING { return width_.load(); }

uint16_t rgbMatrixHeight() MM_NONBLOCKING { return height_.load(); }

void rgbMatrixPush(const uint8_t* rgb) MM_NONBLOCKING {
    const size_t bytes = frameBytes();
    if (!rgb || bytes == 0 || shared_->slots[writeSlot_].size() < bytes) return;   // a canvas with no slots yet, such as a late open's
    std::memcpy(shared_->slots[writeSlot_].data(), rgb, bytes);
    writeSlot_ = ready_.exchange(static_cast<uint8_t>(writeSlot_ | kFresh), std::memory_order_acq_rel) & 3;
    if (recording_) testFrames_++;
}

void rgbMatrixCorrection(uint8_t brightness, bool cie1931) MM_NONBLOCKING {
    brightness_.store(brightness, std::memory_order_relaxed);
    cie1931_.store(cie1931, std::memory_order_relaxed);
}

const char* rgbMatrixPioRefusal() {
    if (!pioChecked_) {
        std::string release;
        bool device = false;
        readPioHost(release, device);
        pioRefusal_ = pioRefusalFor(release, device);
        pioChecked_ = true;
    }
    return pioRefusal_.empty() ? nullptr : pioRefusal_.c_str();
}

uint16_t rgbMatrixFps() MM_NONBLOCKING { return fps_.load(std::memory_order_relaxed); }

// A closed seam holds no memory: the frames and the strings go back, the recorded options to their literal defaults.
void rgbMatrixClose() {
    width_ = 0;
    height_ = 0;
    fps_ = 0;
    ready_.store(0, std::memory_order_release);   // the last push stays unshown: its slot goes below
    for (auto& s : shared_->slots) std::vector<uint8_t>().swap(s);
    testFrames_ = 0;
    testOptions_ = RgbMatrixOptions{};
    std::string().swap(testMapping_);
    std::string().swap(testPanelType_);
    std::string().swap(warning_);
    onPio_ = false;
}

bool rgbMatrixOnPio() { return onPio_; }

void setTestRgbMatrix(bool record) {
    rgbMatrixClose();
    recording_ = record;
    testMaxChains_ = 3;   // the Pi header's, until a case sets another
    testRealtime_ = true;   // granted, until a case takes it away
}

void setTestRgbMatrixMaxChains(uint8_t chains) { testMaxChains_ = chains; }

const RgbMatrixOptions& rgbMatrixTestOptions() { return testOptions_; }

size_t rgbMatrixTestFrameCount() { return testFrames_; }

// The consumer's side of the triple buffer: the newest frame pushed, always whole.
const uint8_t* rgbMatrixTestFrame() {
    if (!testFrames_) return nullptr;
    if (ready_.load(std::memory_order_acquire) & kFresh)
        testReadSlot_ = ready_.exchange(testReadSlot_, std::memory_order_acq_rel) & 3;
    return shared_->slots[testReadSlot_].data();
}

uint8_t rgbMatrixTestBrightness() { return brightness_.load(); }

bool rgbMatrixTestCie1931() { return cie1931_.load(); }

void setTestRgbMatrixRealtime(bool granted) { testRealtime_ = granted; }

void setTestPioHost(const char* kernelRelease, bool pioDevice) {
    testPioHost_ = kernelRelease != nullptr;
    testKernel_ = kernelRelease ? kernelRelease : "";
    testPioDevice_ = pioDevice;
    pioChecked_ = false;   // the next check reads the stand-in, or the host again
}

}  // namespace mm::platform
