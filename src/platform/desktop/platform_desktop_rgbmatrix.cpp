/// rpi-rgb-led-matrix behind the RGB seam (platform.h): real on a Raspberry Pi whose build linked it, a recorder for the tests on every host.
///
/// @attribution hzeller

#include "platform/platform.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(MM_HAVE_RGBMATRIX)
    #include "hardware-mapping.h"   // the library's mapping table: what each mapping drives
    #include "led-matrix.h"

    #include <chrono>
    #include <cstdio>
    #include <unistd.h>
#endif

namespace mm::platform {

namespace {

std::atomic<uint16_t> width_{0};
std::atomic<uint16_t> height_{0};
std::atomic<uint16_t> fps_{0};
std::atomic<uint8_t>  brightness_{255};
std::string lastError_;
const char* warning_ = nullptr;

// The triple buffer, lock-free with one producer, the driver: ready_ holds a slot index in its low bits and kFresh when the producer has filled it since the consumer last took it.
constexpr uint8_t kFresh = 4;
std::atomic<uint8_t> ready_{0};
uint8_t writeSlot_ = 1;

// What the library thread reads, on the heap and never freed: a stuck thread may still read it after exit stops waiting, when static destructors run.
struct Shared {
    std::vector<uint8_t> slots[3];
#if defined(MM_HAVE_RGBMATRIX)
    rgb_matrix::RGBMatrix::Options options;
    rgb_matrix::RuntimeOptions runtime;
    std::string mapping, panelType, pixelMapper;   // the library keeps the options' pointers for the matrix's life
#endif
};
Shared* const shared_ = new Shared;

bool recording_ = false;
RgbMatrixOptions testOptions_;
std::string testMapping_, testPanelType_, testPixelMapper_;   // the recorded options' strings, owned past the caller's
size_t testFrames_ = 0;
uint8_t testReadSlot_ = 2;   // the test's consumer, standing in for the library thread

size_t frameBytes() MM_NONBLOCKING { return static_cast<size_t>(width_.load()) * height_.load() * 3; }

bool fail(const std::string& why) {
    lastError_ = why;
    return false;
}

#if defined(MM_HAVE_RGBMATRIX)

rgb_matrix::RGBMatrix* matrix_ = nullptr;   // the library thread's alone, from creation to deletion
WorkerTask library_;   // one thread for the process: the library's state outlives a matrix
std::atomic<bool> stopping_{false};
enum : uint8_t { kNone, kOpen, kClose };
std::atomic<uint8_t> request_{kNone};
enum : uint8_t { kClosed, kOpened, kRefused };
std::atomic<uint8_t> state_{kClosed};
// Set when the library thread stops answering: it is never joined or reused, and every later open says so at once.
std::atomic<bool> stuck_{false};

static_assert(sizeof(rgb_matrix::Color) == 3, "SetPixels reads the slot as packed RGB triples");

// Show the newest slot at the brightness last handed over, and swap it in at the next vsync.
rgb_matrix::FrameCanvas* showFrame(rgb_matrix::FrameCanvas* canvas, uint8_t& readSlot) {
    readSlot = ready_.exchange(readSlot, std::memory_order_acq_rel) & 3;
    const uint8_t level = brightness_.load(std::memory_order_relaxed);
    if (level == 0) {
        canvas->Clear();
    } else {
        // Only on a change: setting it rebuilds the canvas's brightness tables.
        const uint8_t percent = static_cast<uint8_t>((level * 100 + 254) / 255);   // 1 to 100, never 0
        if (canvas->brightness() != percent) canvas->SetBrightness(percent);
        canvas->SetPixels(0, 0, canvas->width(), canvas->height(), reinterpret_cast<rgb_matrix::Color*>(shared_->slots[readSlot].data()));
    }
    return matrix_->SwapOnVSync(canvas);
}

// Show frames until close is requested, counting those shown each second.
void showFrames(rgb_matrix::FrameCanvas* canvas) {
    using Clock = std::chrono::steady_clock;
    uint8_t readSlot = 2;
    uint16_t shown = 0;
    Clock::time_point window = Clock::now();
    while (request_.load(std::memory_order_acquire) == kOpen && !stopping_.load(std::memory_order_acquire)) {
        if (ready_.load(std::memory_order_acquire) & kFresh) {
            canvas = showFrame(canvas, readSlot);
            shown++;
        } else {
            sleepUs(library_, 1000);
        }
        if (Clock::now() - window >= std::chrono::seconds(1)) {
            fps_.store(shown, std::memory_order_relaxed);
            shown = 0;
            window = Clock::now();
        }
    }
}

// The one thread that touches the library, since piolib keeps its PIO instance per thread and SwapOnVSync blocks until the next refresh: it opens the panels on request, shows frames, and deletes them on close.
void libraryThread(void*) {
    while (!stopping_.load(std::memory_order_acquire)) {
        if (request_.load(std::memory_order_acquire) != kOpen) {
            sleepUs(library_, 1000);
            continue;
        }
        matrix_ = rgb_matrix::RGBMatrix::CreateFromOptions(shared_->options, shared_->runtime);
        if (!matrix_) {
            request_.store(kNone, std::memory_order_release);
            state_.store(kRefused, std::memory_order_release);
            continue;
        }
        matrix_->StartRefresh();
        rgb_matrix::FrameCanvas* canvas = matrix_->CreateFrameCanvas();
        width_ = static_cast<uint16_t>(matrix_->width());
        height_ = static_cast<uint16_t>(matrix_->height());
        state_.store(kOpened, std::memory_order_release);
        showFrames(canvas);
        delete matrix_;   // stops the refresh and clears the panels
        matrix_ = nullptr;
        width_ = 0;   // a late open that ended here leaves no canvas behind
        height_ = 0;
        request_.store(kNone, std::memory_order_release);
        state_.store(kClosed, std::memory_order_release);
    }
}

// Wait on the caller's thread, the render thread at a frame boundary, for the library thread to leave `from`, up to `ms`; false when it did not.
bool awaitState(uint8_t from, int ms) {
    for (int waited = 0; state_.load(std::memory_order_acquire) == from; waited++) {
        if (waited >= ms) return false;
        delayMs(1);
    }
    return true;
}

// At exit a stuck library is left to the kernel: joining it would hold the process until a service manager kills it.
void stopLibrary() {
    rgbMatrixClose();
    stopping_.store(true, std::memory_order_release);
    if (library_.impl && !stuck_.load(std::memory_order_acquire)) stopPinnedTask(library_);
}

// The library's own entry for a mapping name, or null.
const HardwareMapping* findMapping(const char* name) {
    for (const HardwareMapping* m = matrix_hardware_mappings; m->name; ++m)
        if (std::strcmp(m->name, name) == 0) return m;
    return nullptr;
}

// The parallel chains a mapping drives, by the library's rule: its table entry, or where that is 0, the chains whose color pins it defines.
int chainsOf(const HardwareMapping& h) {
    if (h.max_parallel_chains > 0) return h.max_parallel_chains;
    const gpio_bits_t chains[] = {h.p0_r1 | h.p0_g1 | h.p0_r2 | h.p0_g2, h.p1_r1 | h.p1_g1 | h.p1_r2 | h.p1_g2,
                                  h.p2_r1 | h.p2_g1 | h.p2_r2 | h.p2_g2, h.p3_r1 | h.p3_g1 | h.p3_r2 | h.p3_g2,
                                  h.p4_r1 | h.p4_g1 | h.p4_r2 | h.p4_g2, h.p5_r1 | h.p5_g1 | h.p5_r2 | h.p5_g2};
    int n = 0;
    for (gpio_bits_t c : chains) n += c != 0;
    return n;
}

// Whether the Pi's onboard sound is loaded, which the library's hardware pulser on a Pi 3 or 4 exits on.
bool onboardSoundLoaded() {
    FILE* f = std::fopen("/proc/modules", "r");
    if (!f) return false;
    char line[128];
    bool loaded = false;
    while (!loaded && std::fgets(line, sizeof(line), f)) loaded = std::strncmp(line, "snd_bcm2835 ", 12) == 0;
    std::fclose(f);
    return loaded;
}

// stdio rather than ifstream: these small reads keep open's stack frame under the build's limit.
bool isRaspberryPi() {
    char model[80] = {};
    FILE* f = std::fopen("/proc/device-tree/model", "r");
    if (!f) return false;
    const size_t n = std::fread(model, 1, sizeof(model) - 1, f);
    std::fclose(f);
    model[n] = 0;
    return std::strstr(model, "Raspberry Pi") != nullptr;
}

// CAP_SYS_NICE is bit 23 of the effective set; root has it, and without it the library's refresh thread runs unprioritized.
bool hasRealtime() {
    if (geteuid() == 0) return true;
    FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return false;
    char line[128];
    bool nice = false;
    while (std::fgets(line, sizeof(line), f))
        if (std::strncmp(line, "CapEff:", 7) == 0) { nice = (std::strtoull(line + 7, nullptr, 16) >> 23) & 1; break; }
    std::fclose(f);
    return nice;
}

// The seam's options as the library's, its strings owned here since the library keeps their pointers.
void setLibraryOptions(const RgbMatrixOptions& o) {
    shared_->mapping = o.mapping ? o.mapping : "regular";
    shared_->panelType = o.panelType ? o.panelType : "";
    shared_->pixelMapper = o.pixelMapper ? o.pixelMapper : "";
    rgb_matrix::RGBMatrix::Options& mo = shared_->options;
    mo = rgb_matrix::RGBMatrix::Options();
    mo.hardware_mapping = shared_->mapping.c_str();
    mo.panel_type = shared_->panelType.empty() ? nullptr : shared_->panelType.c_str();
    mo.pixel_mapper_config = shared_->pixelMapper.empty() ? nullptr : shared_->pixelMapper.c_str();
    mo.rows = o.rows;
    mo.cols = o.cols;
    mo.chain_length = o.chain;
    mo.parallel = o.parallel;
    mo.multiplexing = o.multiplexing;
    mo.row_address_type = o.rowAddrType;
    mo.scan_mode = o.scanMode;
    mo.pwm_bits = o.pwmBits;
    mo.pwm_lsb_nanoseconds = o.pwmLsbNs;
    mo.pwm_dither_bits = o.ditherBits;
    mo.limit_refresh_rate_hz = o.limitRefreshHz;
    shared_->runtime = rgb_matrix::RuntimeOptions();
    shared_->runtime.daemon = -1;            // never fork: the library thread starts the refresh
    shared_->runtime.drop_privileges = -1;   // never change MoonLight's user
    shared_->runtime.rp1_pio = o.rp1Pio ? 1 : 0;
    if (o.slowdown >= 0) shared_->runtime.gpio_slowdown = o.slowdown;
}

// Why this Pi cannot reach the panels on the chosen backend, or empty when it can.
std::string deviceRefusal(bool pio) {
    if (!isRaspberryPi()) return "not a Raspberry Pi";
    // A Pi 5 maps RP1 through its own devices, which the gpio group reaches; earlier models need /dev/mem.
    const bool rp1 = access("/dev/gpiomem0", F_OK) == 0;
    const char* device = rp1 ? (pio ? "/dev/pio0" : "/dev/gpiomem0") : "/dev/mem";
    if (access(device, F_OK) != 0) return std::string("no ") + device + " on this system";
    if (access(device, R_OK | W_OK) != 0)
        return std::string("no access to ") + device + (rp1 ? ": add the user to the gpio group" : ": run as root");
    if (!rp1 && onboardSoundLoaded()) return "the Pi's onboard sound is loaded, which the panels cannot share: set dtparam=audio=off";
    return {};
}

// Why the library would refuse the options set, or end the process on them through its `...OrDie` init, or empty when it would not.
std::string refusal(uint8_t parallel, bool pio) {
    std::string why;
    if (!shared_->options.Validate(&why)) return why;
    const HardwareMapping* hw = findMapping(shared_->mapping.c_str());
    if (!hw) return "no GPIO mapping named " + shared_->mapping;
    const int chains = chainsOf(*hw);
    if (parallel > chains)
        return std::string("the ") + hw->name + " mapping drives at most " + std::to_string(chains) + " parallel chain" + (chains > 1 ? "s" : "");
    return deviceRefusal(pio);
}

// Ask the library thread to open the panels and wait for its answer, up to 10 s when the library stalls.
bool openOnLibraryThread() {
    if (!library_.impl) {
        if (!spawnPinnedTask(library_, "rgbmatrix", libraryThread, nullptr, 8192, 5, -1)) return fail("no thread for the panels");
        std::atexit(stopLibrary);   // a running thread at exit terminates
    }
    // Nothing is fresh until the driver pushes, which it can only do once open has sized the slots.
    ready_.store(0, std::memory_order_release);
    writeSlot_ = 1;
    state_.store(kClosed, std::memory_order_release);
    request_.store(kOpen, std::memory_order_release);
    if (awaitState(kClosed, 10000) && state_.load(std::memory_order_acquire) == kOpened) return true;
    const bool refused = state_.load(std::memory_order_acquire) == kRefused;
    if (!refused) {
        request_.store(kClose, std::memory_order_release);   // a late open closes itself
        stuck_.store(true, std::memory_order_release);
    }
    state_.store(kClosed, std::memory_order_release);
    width_ = 0;
    height_ = 0;
    return fail(refused ? "the library refused the panels; its reason is in the log" : "the panels did not open in time");
}

#endif

// Keep the options and stand in for panels of the size they wire, so a test reads what a driver opened and pushed.
bool recordOpen(const RgbMatrixOptions& o) {
    testOptions_ = o;
    testMapping_ = o.mapping ? o.mapping : "";
    testPanelType_ = o.panelType ? o.panelType : "";
    testPixelMapper_ = o.pixelMapper ? o.pixelMapper : "";
    testOptions_.mapping = testMapping_.c_str();
    testOptions_.panelType = testPanelType_.c_str();
    testOptions_.pixelMapper = testPixelMapper_.c_str();
    // The panels as wired; a pixel mapper that reshapes the canvas is the library's to apply, on a Pi.
    width_ = static_cast<uint16_t>(o.cols * o.chain);
    height_ = static_cast<uint16_t>(o.rows * o.parallel);
    // The same triple buffer the library thread takes from, so a test runs the handover a Pi runs.
    for (auto& s : shared_->slots) s.assign(frameBytes(), 0);
    ready_.store(0, std::memory_order_release);
    writeSlot_ = 1;
    testReadSlot_ = 2;
    testFrames_ = 0;
    return frameBytes() > 0 || fail("the panels form no canvas");
}

}  // namespace

bool rgbMatrixOpen(const RgbMatrixOptions& o) {
    rgbMatrixClose();
    lastError_.clear();
    warning_ = nullptr;
    if (recording_) return recordOpen(o);
#if !defined(MM_HAVE_RGBMATRIX)
    (void)o;
    return fail("needs a Raspberry Pi build with rpi-rgb-led-matrix");
#else
    // First, before any option the stuck thread may still be reading is rewritten.
    if (stuck_.load(std::memory_order_acquire)) return fail("the panels' library is stuck in a transfer: restart MoonLight");
    setLibraryOptions(o);
    const std::string why = refusal(o.parallel, o.rp1Pio);
    if (!why.empty()) return fail(why);
    if (!openOnLibraryThread()) return false;
    if (!hasRealtime()) warning_ = "no real-time priority, so colors may flicker: grant CAP_SYS_NICE";
    for (auto& s : shared_->slots) s.assign(frameBytes(), 0);
    return true;
#endif
}

const char* rgbMatrixLastError() { return lastError_.empty() ? nullptr : lastError_.c_str(); }

const char* rgbMatrixWarning() { return warning_; }

uint16_t rgbMatrixWidth() MM_NONBLOCKING { return width_.load(); }

uint16_t rgbMatrixHeight() MM_NONBLOCKING { return height_.load(); }

void rgbMatrixPush(const uint8_t* rgb) MM_NONBLOCKING {
    const size_t bytes = frameBytes();
    if (!rgb || bytes == 0 || shared_->slots[writeSlot_].size() < bytes) return;   // a canvas with no slots yet, such as a late open's
    std::memcpy(shared_->slots[writeSlot_].data(), rgb, bytes);
    writeSlot_ = ready_.exchange(static_cast<uint8_t>(writeSlot_ | kFresh), std::memory_order_acq_rel) & 3;
    if (recording_) testFrames_++;
}

void rgbMatrixBrightness(uint8_t level) MM_NONBLOCKING { brightness_.store(level, std::memory_order_relaxed); }

uint16_t rgbMatrixFps() MM_NONBLOCKING { return fps_.load(std::memory_order_relaxed); }

// A closed seam holds nothing: the frames and the strings go back, the recorded options to their literal defaults.
void rgbMatrixClose() {
    width_ = 0;
    height_ = 0;
    fps_ = 0;
#if defined(MM_HAVE_RGBMATRIX)
    // A library stuck in a transfer keeps its frames and strings, which it may still read, and nothing answers, so close waits only the first time.
    if (stuck_.load(std::memory_order_acquire)) return;
    if (state_.load(std::memory_order_acquire) == kOpened) {
        request_.store(kClose, std::memory_order_release);
        if (!awaitState(kOpened, 3000)) {
            stuck_.store(true, std::memory_order_release);
            return;
        }
    }
    std::string().swap(shared_->mapping);
    std::string().swap(shared_->panelType);
    std::string().swap(shared_->pixelMapper);
#endif
    ready_.store(0, std::memory_order_release);   // the last push stays unshown: its slot goes below
    for (auto& s : shared_->slots) std::vector<uint8_t>().swap(s);
    testFrames_ = 0;
    testOptions_ = RgbMatrixOptions{};
    std::string().swap(testMapping_);
    std::string().swap(testPanelType_);
    std::string().swap(testPixelMapper_);
}

void setTestRgbMatrix(bool record) {
    rgbMatrixClose();
    recording_ = record;
}

const RgbMatrixOptions& rgbMatrixTestOptions() { return testOptions_; }

size_t rgbMatrixTestFrameCount() { return testFrames_; }

// The consumer's side of the triple buffer, taken as the library thread takes it: the newest frame pushed, never one half written.
const uint8_t* rgbMatrixTestFrame() {
    if (!testFrames_) return nullptr;
    if (ready_.load(std::memory_order_acquire) & kFresh)
        testReadSlot_ = ready_.exchange(testReadSlot_, std::memory_order_acq_rel) & 3;
    return shared_->slots[testReadSlot_].data();
}

uint8_t rgbMatrixTestBrightness() { return brightness_.load(); }

}  // namespace mm::platform
