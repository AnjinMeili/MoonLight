/// The tempo tracker's own front end; the public surface and the class layout live in AudioTempoFrontEnd.h.
#include "core/services/AudioTempoFrontEnd.h"

#include <cmath>
#include <cstring>
#include <numbers>

namespace mm {

namespace {

// One biquad section, transposed direct form II: y = b0*x + s1, s1 = b1*x - a1*y + s2, s2 = b2*x - a2*y.
float biquadStep(const float* c, float* w, float x) {
    const float y = c[0] * x + w[0];
    w[0] = c[1] * x - c[3] * y + w[1];
    w[1] = c[2] * x - c[4] * y;
    return y;
}

// The RBJ cookbook's shared preamble: the angular frequency, its cosine, and alpha from Q.
void rbjPreamble(float f0, float fs, float q, float& w0, float& cw, float& alpha) {
    w0 = 2.0f * std::numbers::pi_v<float> * f0 / fs;
    cw = cosf(w0);
    alpha = sinf(w0) / (2.0f * q);
}

// A high-pass with its -3 dB point at f0 (RBJ cookbook).
void rbjHpf(float f0, float fs, float q, float* c) {
    float w0 = 0.0f, cw = 0.0f, alpha = 0.0f;
    rbjPreamble(f0, fs, q, w0, cw, alpha);
    const float a0 = 1.0f + alpha;
    c[0] = ((1.0f + cw) * 0.5f) / a0;
    c[1] = (-(1.0f + cw)) / a0;
    c[2] = ((1.0f + cw) * 0.5f) / a0;
    c[3] = (-2.0f * cw) / a0;
    c[4] = (1.0f - alpha) / a0;
}

// A low-pass with its -3 dB point at f0 (RBJ cookbook).
void rbjLpf(float f0, float fs, float q, float* c) {
    float w0 = 0.0f, cw = 0.0f, alpha = 0.0f;
    rbjPreamble(f0, fs, q, w0, cw, alpha);
    const float a0 = 1.0f + alpha;
    c[0] = ((1.0f - cw) * 0.5f) / a0;
    c[1] = (1.0f - cw) / a0;
    c[2] = ((1.0f - cw) * 0.5f) / a0;
    c[3] = (-2.0f * cw) / a0;
    c[4] = (1.0f - alpha) / a0;
}

// A Butterworth low-pass of `order`, as biquad sections: the prototype's poles at exp(j*pi*(2k+N+1)/(2N)), each conjugate pair one section, a real pole a first-order section in the biquad's form.
int butterLowpass(float fc, float fs, int order, float out[][5], int maxSecs) {
    const float k = tanf(std::numbers::pi_v<float> * fc / fs);
    const float k2 = k * k;
    int n = 0;
    for (int i = 0; i < order && n < maxSecs; i++) {
        const float theta = std::numbers::pi_v<float> * (static_cast<float>(order) + 2.0f * static_cast<float>(i) + 1.0f) / (2.0f * static_cast<float>(order));
        const float px = cosf(theta);
        const float py = sinf(theta);
        if (py < -1e-6f) continue;  // the conjugate of a pole already placed
        const float sigma = -px;    // the pole is at -sigma +- j*|py|
        if (fabsf(py) < 1e-6f) {    // a real pole: a first-order section
            const float a = 1.0f + sigma * k;
            out[n][0] = k / a;
            out[n][1] = k / a;
            out[n][2] = 0.0f;
            out[n][3] = (1.0f - sigma * k) / a;
            out[n][4] = 0.0f;
        } else {
            const float a = 1.0f / k2 + 2.0f * sigma / k + 1.0f;
            out[n][0] = 1.0f / a;
            out[n][1] = 2.0f / a;
            out[n][2] = 1.0f / a;
            out[n][3] = (2.0f - 2.0f / k2) / a;
            out[n][4] = (1.0f / k2 - 2.0f * sigma / k + 1.0f) / a;
        }
        n++;
    }
    return n;
}

}  // namespace

void AudioTempoFrontEnd::reset(uint32_t rate) {
    memset(aaW_, 0, sizeof(aaW_));
    memset(bandW_, 0, sizeof(bandW_));
    memset(decBuf_, 0, sizeof(decBuf_));
    memset(hist_, 0, sizeof(hist_));
    memset(peakFrame_, 0, sizeof(peakFrame_));
    memset(peakStr_, 0, sizeof(peakStr_));
    decCount_ = 0;
    aaPhase_ = 0;
    lastDb_ = -120.0f;
    haveLastDb_ = false;
    odfMax_ = 0.0f;
    histIdx_ = 0;
    histFilled_ = 0;
    peakHead_ = 0;
    peakCount_ = 0;
    prevPeakFrame_ = kNoPeak;
    prevPeakStr_ = 0.0f;
    const float frameRate = static_cast<float>(rate) / 512.0f;
    const float decimatedRate = static_cast<float>(rate) / static_cast<float>(kDec);
    maxDecay_ = expf(-1.0f / (kMaxTauS * frameRate));
    runnable_ = decimatedRate > 2.0f * kBandHiHz;
    if (!runnable_) return;
    butterLowpass(kAaHz, static_cast<float>(rate), 2 * kAaSecs, aaC_, kAaSecs);
    rbjHpf(kBandLoHz, decimatedRate, kQ, bandC_[0]);
    rbjHpf(kBandLoHz, decimatedRate, kQ, bandC_[1]);
    rbjLpf(kBandHiHz, decimatedRate, kQ, bandC_[2]);
    rbjLpf(kBandHiHz, decimatedRate, kQ, bandC_[3]);
}

void AudioTempoFrontEnd::process(const int32_t* samples, size_t n, uint32_t frame) {
    if (!runnable_) return;
    filterBlock(samples, n);
    if (decCount_ < kHop) return;
    decCount_ = 0;
    pickPeak(frameOdf(), frame);
}

void AudioTempoFrontEnd::filterBlock(const int32_t* samples, size_t n) {
    float aa[kAaSecs][5];  // the coefficients in locals, so the inner loop reads the stack
    for (int s = 0; s < kAaSecs; s++)
        for (int c = 0; c < 5; c++) aa[s][c] = aaC_[s][c];
    float band[kBandSecs][5];
    for (int s = 0; s < kBandSecs; s++)
        for (int c = 0; c < 5; c++) band[s][c] = bandC_[s][c];
    for (size_t i = 0; i < n; i++) {
        float x = static_cast<float>(samples[i] >> 8) * kSampleScale;
        for (int s = 0; s < kAaSecs; s++) x = biquadStep(aa[s], aaW_[s], x);
        if (++aaPhase_ < kDec) continue;  // one sample in sixteen is kept
        aaPhase_ = 0;
        for (int s = 0; s < kBandSecs; s++) x = biquadStep(band[s], bandW_[s], x);
        decBuf_[decCount_++] = x;
    }
}

float AudioTempoFrontEnd::frameOdf() {
    float p = 0.0f;
    for (int i = 0; i < kHop; i++) p += decBuf_[i] * decBuf_[i];
    p /= static_cast<float>(kHop);
    const float db = 10.0f * log10f(p * kPowNorm + kPowEps);
    float odf = 0.0f;
    if (haveLastDb_) {  // the first frame has no previous level, so it has no rise to report
        odf = db - lastDb_;
        if (odf < 0.0f) odf = 0.0f;  // half-wave rectified: a rise only
    }
    lastDb_ = db;
    haveLastDb_ = true;
    if (odf > odfMax_) odfMax_ = odf;
    else odfMax_ *= maxDecay_;
    return odf;
}

void AudioTempoFrontEnd::hold() {
    peakCount_ = 0;
    peakHead_ = 0;
    prevPeakFrame_ = kNoPeak;
    prevPeakStr_ = 0.0f;
}

void AudioTempoFrontEnd::rebase(int32_t delta) {
    for (uint16_t i = 0; i < peakCount_; i++) {
        const int idx = (peakHead_ + kPeakRing - 1 - i) & (kPeakRing - 1);
        peakFrame_[idx] += delta;
    }
    if (prevPeakFrame_ >= 0) prevPeakFrame_ += delta;  // the sentinel means no onset yet
}

void AudioTempoFrontEnd::pickPeak(float odf, uint32_t frame) {
    hist_[histIdx_] = odf;
    histIdx_ = static_cast<uint8_t>((histIdx_ + 1) & (kHistRing - 1));
    if (histFilled_ < kHistRing) histFilled_++;
    if (histFilled_ < kHistLen + kPeakPost + 1) return;  // the picker needs its history plus the candidate's latency
    const uint8_t head = static_cast<uint8_t>((histIdx_ + kHistRing - kHistLen) & (kHistRing - 1));
    const int base = kHistLen - 1 - kPeakPost;  // 33: the candidate's logical index
    const float oj = hist_[(head + base) & (kHistRing - 1)];
    if (oj <= 0.0f) return;
    const float on = peakStrength(head, base, oj);
    if (on < 0.0f) return;
    acceptPeak(static_cast<int32_t>(frame) - kPeakPost, on);  // the candidate's frame, three frames back
}

float AudioTempoFrontEnd::peakStrength(int head, int base, float oj) const {
    float segMax = oj;
    for (int k = base - kPeakPre; k <= base + kPeakPost; k++) {
        const float v = hist_[(head + k) & (kHistRing - 1)];
        if (v > segMax) segMax = v;
    }
    if (oj < segMax) return -1.0f;  // not a local maximum
    float sum = 0.0f;
    for (int k = base - kPeakMeanHalf; k <= base; k++) sum += hist_[(head + k) & (kHistRing - 1)];
    const float mean = sum / static_cast<float>(kPeakMeanHalf + 1);
    if (oj < (1.0f + kPeakDelta) * mean) return -1.0f;  // not above the local mean
    const float on = (odfMax_ > 0.0f) ? (oj / odfMax_) : 0.0f;
    if (on < kPeakFloor) return -1.0f;  // below the floor
    return on;
}

void AudioTempoFrontEnd::acceptPeak(int32_t frame, float on) {
    if (static_cast<int32_t>(static_cast<uint32_t>(frame) - static_cast<uint32_t>(prevPeakFrame_)) < kPeakMinGap) {
        if (on > prevPeakStr_) {  // too close to the last: keep the stronger of the pair
            prevPeakFrame_ = frame;
            prevPeakStr_ = on;
            const int last = (peakHead_ + kPeakRing - 1) & (kPeakRing - 1);
            peakFrame_[last] = frame;
            peakStr_[last] = on;
        }
        return;
    }
    prevPeakFrame_ = frame;
    prevPeakStr_ = on;
    peakFrame_[peakHead_] = frame;
    peakStr_[peakHead_] = on;
    peakHead_ = static_cast<uint16_t>((peakHead_ + 1) & (kPeakRing - 1));
    if (peakCount_ < kPeakRing) peakCount_++;
}

}  // namespace mm
