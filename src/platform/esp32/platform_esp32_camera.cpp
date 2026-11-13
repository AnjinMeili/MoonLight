/// Proof of concept: the XIAO ESP32S3 Sense's DVP camera behind the capture seam, in place of the USB grabber.
///
/// Branch xiao-camera-poc only. The pins are the XIAO's (arduino-esp32 camera_pins.h, CAMERA_MODEL_XIAO_ESP32S3),
/// the sensor streams RGB565 at the smallest size covering the picture, and the decode is a nearest-neighbour
/// shrink into the RGB888 picture. The shape the real change takes is the camera spec's: a source of its own.

#include "platform/platform.h"

#include "sdkconfig.h"

#if defined(CONFIG_MM_DVP_CAMERA) && defined(CONFIG_IDF_TARGET_ESP32S3)

#include "esp_camera.h"
#include "esp_log.h"

#include <atomic>

namespace mm::platform {

namespace {

const char* TAG = "camera";

const char* openFault = nullptr;    ///< the step the last open stopped at
camera_fb_t* pending = nullptr;     ///< the frame AwaitFrame took, until DecodeInto returns it
bool opened = false;
uint16_t streamW = 0, streamH = 0;
std::atomic<uint32_t> decoded{0}, failed{0};

/// The smallest sensor frame size that covers w x h, or the largest offered.
framesize_t frameSizeFor(uint16_t w, uint16_t h, uint16_t& fw, uint16_t& fh) {
    struct Size { framesize_t fs; uint16_t w, h; };
    static constexpr Size kSizes[] = {
        {FRAMESIZE_96X96, 96, 96}, {FRAMESIZE_QQVGA, 160, 120}, {FRAMESIZE_QCIF, 176, 144},
        {FRAMESIZE_HQVGA, 240, 176}, {FRAMESIZE_240X240, 240, 240}, {FRAMESIZE_QVGA, 320, 240},
    };
    for (const Size& s : kSizes)
        if (s.w >= w && s.h >= h) { fw = s.w; fh = s.h; return s.fs; }
    const Size& last = kSizes[sizeof(kSizes) / sizeof(kSizes[0]) - 1];
    fw = last.w; fh = last.h;
    return last.fs;
}

}   // namespace

uint32_t videoCaptureFormatGeneration() MM_NONBLOCKING { return 1; }   // the camera is soldered: one device, never leaving
const char* videoCaptureFault() MM_NONBLOCKING { return openFault; }
bool videoCaptureSettling() MM_NONBLOCKING { return false; }

bool videoCaptureInit(VideoCaptureHandle& h, uint16_t width, uint16_t height, uint8_t fps) {
    (void)fps;   // the sensor runs at its own rate; the newest frame wins
    openFault = nullptr;
    if (opened) { openFault = "camera already open"; return false; }

    camera_config_t c{};
    c.pin_pwdn = -1;
    c.pin_reset = -1;
    c.pin_xclk = 10;
    c.pin_sccb_sda = 40;
    c.pin_sccb_scl = 39;
    c.pin_d7 = 48;   // Y9
    c.pin_d6 = 11;   // Y8
    c.pin_d5 = 12;   // Y7
    c.pin_d4 = 14;   // Y6
    c.pin_d3 = 16;   // Y5
    c.pin_d2 = 18;   // Y4
    c.pin_d1 = 17;   // Y3
    c.pin_d0 = 15;   // Y2
    c.pin_vsync = 38;
    c.pin_href = 47;
    c.pin_pclk = 13;
    c.xclk_freq_hz = 20000000;
    c.ledc_timer = LEDC_TIMER_3;       // the last timer and channel, away from the PWM light driver's first ones
    c.ledc_channel = LEDC_CHANNEL_7;
    c.pixel_format = PIXFORMAT_RGB565;
    c.frame_size = frameSizeFor(width, height, streamW, streamH);
    c.jpeg_quality = 12;
    c.fb_count = 2;
    c.fb_location = CAMERA_FB_IN_PSRAM;
    c.grab_mode = CAMERA_GRAB_LATEST;
    c.sccb_i2c_port = 1;

    const esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        openFault = "camera did not start (esp_camera_init)";
        streamW = streamH = 0;
        return false;
    }
    if (sensor_t* s = esp_camera_sensor_get()) {
        ESP_LOGI(TAG, "sensor PID 0x%02x, %ux%u RGB565", s->id.PID, streamW, streamH);
        // An OV3660 starts upside down and oversaturated: easytarget/esp32-cam-webserver's values (LGPL-2.1, settings only).
        if (s->id.PID == OV3660_PID) {
            s->set_vflip(s, 1);
            s->set_brightness(s, 1);
            s->set_saturation(s, -2);
        }
    }
    opened = true;
    decoded.store(0); failed.store(0);
    h.impl = &opened;
    return true;
}

bool videoCaptureAwaitFrame(VideoCaptureHandle& h, uint32_t timeoutMs) {
    (void)timeoutMs;   // esp_camera_fb_get waits on the driver's own timeout
    if (!h.impl || pending) return pending != nullptr;
    pending = esp_camera_fb_get();
    return pending != nullptr;
}

bool videoCaptureDecodeInto(VideoCaptureHandle& h, uint8_t* dst, uint16_t maxWidth, uint16_t maxHeight,
                            uint16_t& outWidth, uint16_t& outHeight, uint8_t floor) {
    if (!h.impl || !pending || !maxWidth || !maxHeight) return false;
    camera_fb_t* fb = pending;
    if (fb->format != PIXFORMAT_RGB565 || fb->len < static_cast<size_t>(fb->width) * fb->height * 2) {
        esp_camera_fb_return(fb); pending = nullptr; failed++;
        return false;
    }
    // A whole-number step that fits the frame inside the picture, keeping its aspect.
    const uint32_t sx = (fb->width + maxWidth - 1) / maxWidth, sy = (fb->height + maxHeight - 1) / maxHeight;
    const uint32_t step = sx > sy ? sx : sy;
    outWidth = static_cast<uint16_t>(fb->width / step);
    outHeight = static_cast<uint16_t>(fb->height / step);
    uint8_t* out = dst;
    for (uint32_t y = 0; y < outHeight; y++) {
        const uint8_t* row = fb->buf + (y * step) * fb->width * 2;
        for (uint32_t x = 0; x < outWidth; x++) {
            const uint8_t* p = row + (x * step) * 2;
            const uint16_t v = static_cast<uint16_t>((p[0] << 8) | p[1]);   // the sensor sends RGB565 high byte first
            uint8_t r = static_cast<uint8_t>(((v >> 11) & 0x1F) << 3);
            uint8_t g = static_cast<uint8_t>(((v >> 5) & 0x3F) << 2);
            uint8_t b = static_cast<uint8_t>((v & 0x1F) << 3);
            r |= r >> 5; g |= g >> 6; b |= b >> 5;
            *out++ = r <= floor ? 0 : r;
            *out++ = g <= floor ? 0 : g;
            *out++ = b <= floor ? 0 : b;
        }
    }
    esp_camera_fb_return(fb);
    pending = nullptr;
    decoded++;
    return true;
}

void videoCaptureDeinit(VideoCaptureHandle& h) {
    if (pending) { esp_camera_fb_return(pending); pending = nullptr; }
    if (opened) esp_camera_deinit();
    opened = false;
    streamW = streamH = 0;
    h.impl = nullptr;
}

void videoCaptureShutdown() {}

VideoCaptureStats videoCaptureStats() MM_NONBLOCKING {
    VideoCaptureStats s;
    s.decoded = decoded.load();
    s.failed = failed.load();
    s.stream.width = streamW;
    s.stream.height = streamH;
    return s;
}

}   // namespace mm::platform

#endif
