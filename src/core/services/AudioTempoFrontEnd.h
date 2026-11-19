#pragma once

#include <cstddef>
#include <cstdint>

namespace mm {

/// The tempo tracker's own front end: a 300 Hz anti-alias filter and a 90 to 320 Hz band feeding an onset picker, reading the analysis block the banks read and disturbing nothing.
/// @attribution troyhacks
///
/// @technical
///
/// ## Why its own front end
///
/// The tracker's onset detector needs the band's rise within 5 to 15 ms, and the third-octave bank's bass bands answer 47 ms late, so the tracker filters the block itself: the anti-alias filter at the full rate, one sample in sixteen kept, then the band at the decimated rate.
///
/// ## The onset detector
///
/// The frame's band power, in dB, minus the previous frame's, half-wave rectified: a rise only. A frame is a peak when it is the maximum of a +-3 frame window, stands 35 % above the local mean of the last 0.35 s, reaches 6 % of the onset function's running maximum, and is at least 5 frames from a stronger peak.
///
/// The picker reads the candidate three frames late, because it cannot see the future: the picker's latency is 3 frames, 64 ms at 24 kHz and 32 ms at 48 kHz.
///
/// ## The onset ring
///
/// The frames are signed and relative to the frame base: a rebase moves the base forward under the onsets already in the ring, so an onset from before the rebase is negative afterwards, which is what it is.
///
/// ## The rate it runs at
///
/// The coefficients are baked for the opened rate, and the decimated rate must stay above twice the band's top, so a rate of 8 kHz or below does not run: reset leaves the front end not runnable and process is a no-op there.
struct AudioTempoFrontEnd {
    /// Bake the coefficients for the opened rate and clear the state.
    void reset(uint32_t rate);

    /// Whether the opened rate supports the band: false at 8 kHz and below: @xref{the-rate-it-runs-at}
    bool runnable() const { return runnable_; }

    /// One block of left-justified 24-bit samples at the frame's index: the filters, the onset function, the picker: @xref{why-its-own-front-end}
    void process(const int32_t* samples, size_t n, uint32_t frame);

    /// Throw the evidence away: the onsets and the last accepted onset, so the music that comes back is measured afresh.
    void hold();

    /// Move the onset ring and the last accepted onset by the rebase's delta.
    void rebase(int32_t delta);

    /// The onset ring's frames, newest at the head's predecessor: @xref{the-onset-ring}
    const int32_t* peakFrames() const { return peakFrame_; }

    /// The onset ring's strengths, in the same order as the frames.
    const float* peakStrengths() const { return peakStr_; }

    /// The ring's write index: the next onset goes here.
    uint16_t peakHead() const { return peakHead_; }

    /// How many onsets the ring holds.
    uint16_t peakCount() const { return peakCount_; }

    /// The current frame's band level, in dB: the energy the tracker publishes.
    float bandDb() const { return lastDb_; }

private:
    static constexpr uint8_t kDec = 16;                 // the decimation: one sample in sixteen is kept
    static constexpr uint8_t kHop = 32;                 // decimated samples per frame (512 / 16)
    static constexpr float kAaHz = 300.0f;              // the anti-alias filter's corner
    static constexpr uint8_t kAaSecs = 2;               // ... in two biquad sections (4th order)
    static constexpr uint8_t kBandSecs = 4;             // the band: two high-pass, two low-pass
    static constexpr float kBandLoHz = 90.0f;           // the band's bottom
    static constexpr float kBandHiHz = 320.0f;          // the band's top
    static constexpr float kQ = 0.70710678f;            // the band's sections are Butterworth
    static constexpr float kPowNorm = 9.31322575e-10f;  // 1 / (32767 * 32767): full scale is 0 dB
    static constexpr float kPowEps = 1e-12f;            // the power floor, -120 dB
    static constexpr float kSampleScale = 1.0f / 256.0f;  // 24-bit left-justified to the +-32767 the filters expect
    static constexpr int kPeakPre = 3;                  // the picker's look-back, frames
    static constexpr int kPeakPost = 3;                 // ... and look-ahead: the picker's latency
    static constexpr int kPeakMeanHalf = 33;            // the local mean's window, frames
    static constexpr float kPeakDelta = 0.35f;          // a peak stands 35 % over the local mean
    static constexpr float kPeakFloor = 0.06f;          // ... and 6 % of the onset function's running maximum
    static constexpr int kPeakMinGap = 5;               // the least separation between onsets, frames
    static constexpr int kHistLen = 37;                 // the onset history the picker reads
    static constexpr int kHistRing = 64;                // ... in a ring twice that, so nothing is overwritten while read
    static constexpr int kPeakRing = 256;               // the onset ring's capacity
    static constexpr float kMaxTauS = 5.0f;             // the onset maximum's decay, seconds
    static constexpr int32_t kNoPeak = -1000;           // the previous-onset sentinel: no onset yet

    /// The picker's decision on one onset value at the frame's index: @xref{the-onset-detector}
    void pickPeak(float odf, uint32_t frame);

    /// The candidate's normalized strength, or -1 when it fails the local-maximum, local-mean or floor test.
    float peakStrength(int head, int base, float oj) const;

    /// Take an onset into the ring, keeping the stronger of any pair too close together.
    void acceptPeak(int32_t frame, float on);

    /// One block through the anti-alias filter, the decimation and the band.
    void filterBlock(const int32_t* samples, size_t n);

    /// The decimated frame's onset value: the band's dB rise, half-wave rectified.
    float frameOdf();

    float aaC_[kAaSecs][5];       // the anti-alias filter's coefficients
    float aaW_[kAaSecs][2];       // ... and its delay lines
    float bandC_[kBandSecs][5];   // the band's coefficients
    float bandW_[kBandSecs][2];   // ... and its delay lines
    float decBuf_[kHop];          // the decimated frame's samples
    uint8_t decCount_ = 0;        // how many of them there are
    uint8_t aaPhase_ = 0;         // the decimation's phase
    float lastDb_ = -120.0f;      // the previous frame's level, dB
    bool haveLastDb_ = false;     // ... and whether there IS one
    float odfMax_ = 0.0f;         // the onset function's running maximum
    float maxDecay_ = 1.0f;       // ... its per-frame decay, from the opened rate
    float hist_[kHistRing];       // the recent onset values
    uint8_t histIdx_ = 0;         // where the next one goes
    uint16_t histFilled_ = 0;     // how many have been written
    int32_t peakFrame_[kPeakRing];  // the onset ring: each onset's frame
    float peakStr_[kPeakRing];    // ... and its strength
    uint16_t peakHead_ = 0;       // the ring's write index
    uint16_t peakCount_ = 0;      // how many are in it
    int32_t prevPeakFrame_ = kNoPeak;  // the last accepted onset (the gap)
    float prevPeakStr_ = 0.0f;    // ... and its strength
    bool runnable_ = false;       // the opened rate supports the band
};

}  // namespace mm
