#pragma once

#include "core/util/AudioFrame.h"   // the frame it hears: the beat clock and the bands

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace mm {

/// How long the music's phrases run and where they start: the spectral flux laid out by the beat clock, folded at each candidate length, the length that lines up more than its own divisors wins, and the beat where the mean phrase rises most starts it.
///
/// @attribution James Hutchinson, Claude
///
/// @technical
///
/// ## The flux, by the beat
///
/// MatrixPrism's beat graph (its band-flux.js and beat-fold.js): the sound's spectral flux, each band's rise in log energy summed, laid row by row against the beat clock, so the same moment of every beat lands on the same row. The frame's 16 bands stand in for MatrixPrism's 8; each frame's flux goes to the row of the beat its `beatPhase` falls in, 12 rows a beat, the fixed-duration rows of MatrixPrism's 2026-11-03 tempo study.
///
/// ## The fold
///
/// At each candidate length, 2 to 16 bars, the latest whole phrases are laid side by side; each is correlated (Pearson) with the mean of the other phrases, and the mean less the study's allowance for chance, 2 √(2 / ((rows − 1) phrases)), is the length's clarity. A plain loop is as clear at every multiple of its length, so a length counts by what it adds: the share of what the best shorter length, down to the bar, leaves unexplained (one less its clarity) that this length explains. A length is tried once the history holds three of its phrases. A fill, a crash or a build that recurs once a phrase adds clarity only at the phrase's length, 16 beats in dance music, 32 for some DJs, 48 in a twelve-bar blues.
///
/// The phrase starts on the bar start with the most flux in the mean phrase: the crash and the drop after the fill. A phrase starts a bar, so the fill's busier beat before it is passed over; the bar comes from the clock's `beatInBar`, Link's bar where a session runs.
class AudioPhrase {
public:
    /// Rows a beat.
    static constexpr uint8_t kRows = 12;
    /// Beats in a bar, the step the candidate lengths take.
    static constexpr uint8_t kBar = 4;
    /// The shortest and longest phrase tried, in beats: 2 and 16 bars.
    static constexpr uint8_t kShortest = 2 * kBar, kLongest = 16 * kBar;
    /// The phrases a length needs in the history to be tried: with two, each is compared with the other alone, which chance lines up.
    static constexpr uint8_t kMinPhrases = 3;
    /// The beats remembered: three of the longest phrase, so every length can be tried.
    static constexpr uint16_t kHistory = 3 * kLongest;
    /// The least share of what the shorter lengths leave unexplained a length must explain to set the phrase: the phrase's fills and crashes are a small part of the sound beside the beat, which lines up at every length, so the share, not the clarity added, tells them.
    static constexpr float kGain = 0.3f;
    /// The least clarity a length must have at all: noise lines up nothing.
    static constexpr float kLock = 0.2f;
    /// The least unexplained rest a share is taken of, so a divisor that already lines up almost everything does not magnify chance.
    static constexpr float kRest = 0.05f;
    /// How much more a new length must add than the set one to replace it, so the phrase does not flicker between two near-equal lengths.
    static constexpr float kSwitch = 0.05f;
    /// The clarity a length asked for (`onlyLength`) needs, less `kGain`, to set the phrase.
    static constexpr float kAskedLock = 0.1f;
    /// A tempo change of more than 1 in this many starts over: 1%.
    static constexpr int kRetempo = 100;
    /// The beats the history must hold before a phrase is set: three phrases of 32, so the common lengths are all tried before a shorter one wins for being the only one.
    static constexpr uint32_t kSettle = 3 * 32;
    /// The bands the flux sums: the frame's 16.
    static constexpr uint8_t kBands = 16;

    /// Start over: no beats, no phrase; the length asked for stays.
    void reset() {
        const uint8_t only = only_;
        *this = AudioPhrase{};
        only_ = only;
    }

    /// Look only for phrases of `beats`, or for every length at 0, starting over when that changes.
    void onlyLength(uint8_t beats) {
        if (beats == only_) return;
        only_ = beats;
        reset();
    }

    /// Hear frame `f`, read once: its flux into the row of the beat it falls in, and close the beat when the clock counts the next; each closed bar folds the history again. Returns true when a beat closed.
    bool hear(const AudioFrame& f) {
        if (!f.tempoBpm) {
            if (started_) reset();
            return false;
        }
        // A tempo moved by more than kRetempo is another track or another grid: what was folded no longer lines up.
        if (started_ && std::abs(static_cast<int>(f.tempoBpm) - static_cast<int>(tempo_)) * kRetempo > tempo_) reset();
        tempo_ = f.tempoBpm;
        float flux = 0;
        for (uint8_t b = 0; b < kBands; b++) {
            const float level = std::log(f.bands[b] + 1.0f);
            if (heardBands_) flux += std::fmax(0.0f, level - prevLevel_[b]);
            prevLevel_[b] = level;
        }
        heardBands_ = true;
        if (!started_) {
            started_ = true;
            lastCount_ = f.beatCount;
        }
        bool closed = false;
        if (f.beatCount != lastCount_) {
            lastCount_ = f.beatCount;
            closeBeat();
            closed = true;
        }
        if (f.beatInBar) barStart_[beats_ % kHistory] = f.beatInBar == 1;
        else barStart_[beats_ % kHistory] = true;   // with no bar known, every beat may start a phrase
        const uint8_t row = static_cast<uint8_t>(f.beatPhase * kRows / 256);
        sum_[row] += flux;
        count_[row]++;
        return closed;
    }

    /// The flux rows of the beat closed `back` beats before the latest, `back` under `kHistory`: the beat graph's column for that beat.
    const float* beatRows(uint32_t back = 0) const { return history_[(beats_ + kHistory - 1 - back) % kHistory]; }
    /// Beats closed since the tempo was found.
    uint32_t beats() const { return beats_; }
    /// The phrase's length in beats, or 0 while none is set.
    uint8_t length() const { return length_; }
    /// Beats from the beat now sounding to the next phrase start, 0 on the start itself; meaningful while `length()` is set.
    uint32_t beatsToStart() const { return length_ ? (length_ - (beats_ - anchor_) % length_) % length_ : 0; }
    /// Whether the beat now sounding starts a phrase.
    bool onStart() const { return length_ && beatsToStart() == 0; }
    /// What the set length adds over its divisors at the last fold; the best other length and what it adds.
    float gain() const { return gain_; }
    uint8_t runnerUp() const { return runnerUp_; }
    float runnerUpGain() const { return runnerUpGain_; }

    /// One line for a status: the phrase, the next start and the fold's gains, or the beats heard while listening.
    const char* describe(char* out, size_t n) const {
        if (length_)
            std::snprintf(out, n, "phrase %u beats, next in %u, adds %.2f; %u beats %.2f", length_, static_cast<unsigned>(beatsToStart()),
                          static_cast<double>(gain_), runnerUp_, static_cast<double>(runnerUpGain_));
        else
            std::snprintf(out, n, "listening for phrases, %u beats; best %u beats adds %.2f", static_cast<unsigned>(beats_), runnerUp_,
                          static_cast<double>(runnerUpGain_));
        return out;
    }

private:
    float history_[kHistory][kRows] = {};   ///< the latest beats, each its rows' mean flux, beat b at b % kHistory
    bool barStart_[kHistory] = {};   ///< whether each beat starts a bar, where a phrase may start
    float sum_[kRows] = {};       ///< the beat now sounding, each row's flux summed
    uint8_t count_[kRows] = {};   ///< frames summed into each row
    float prevLevel_[kBands] = {};   ///< each band's log level in the frame before
    bool heardBands_ = false;
    uint16_t lastCount_ = 0;      ///< the clock's beat count the beat started at
    bool started_ = false;
    uint16_t tempo_ = 0;          ///< the clock's tempo the history was laid out at, BPM x100
    uint32_t weakFolds_ = 0;      ///< folds in a row the set length has stayed under the bar
    uint32_t beats_ = 0;          ///< beats closed
    uint8_t length_ = 0;
    uint32_t anchor_ = 0;         ///< a phrase start, as a beat number
    float gain_ = 0;
    uint8_t runnerUp_ = 0;
    float runnerUpGain_ = 0;
    uint8_t only_ = 0;            ///< the one length looked for, or 0 for every length

    /// The beat just ended: store each row's mean, a row no frame fell in taking the row before's, and fold at the end of each bar.
    void closeBeat() {
        float* slot = history_[beats_ % kHistory];
        float last = 0;
        for (uint8_t r = 0; r < kRows; r++) {
            if (count_[r]) last = sum_[r] / count_[r];
            slot[r] = last;
        }
        std::memset(sum_, 0, sizeof(sum_));
        std::memset(count_, 0, sizeof(count_));
        beats_++;
        if (beats_ % kBar == 0) foldAll();
    }

    /// Fold the history at every length, the bar's too, and set the phrase to the length that adds most over its divisors.
    void foldAll() {
        constexpr uint8_t kCount = kLongest / kBar;   // lengths of 1 to 16 bars
        float clarity[kCount + 1];
        uint32_t start[kCount + 1] = {};
        for (uint8_t bars = 1; bars <= kCount; bars++) clarity[bars] = fold(static_cast<uint8_t>(bars * kBar), start[bars]);

        uint8_t best = 0, second = 0;
        float bestGain = -1, secondGain = -1, current = -1;
        for (uint8_t bars = kShortest / kBar; bars <= kCount; bars++) {
            if (clarity[bars] <= -1) continue;
            float shorter = -1;   // the best of every shorter length, down to the bar
            for (uint8_t d = 1; d < bars; d++)
                if (clarity[d] > shorter) shorter = clarity[d];
            // What the length explains of what every shorter length left unexplained; a length asked for needs only to line up.
            const float g = only_ ? clarity[bars] - kAskedLock
                          : clarity[bars] < kLock ? -1 : (clarity[bars] - shorter) / std::fmax(kRest, 1 - shorter);
            const uint8_t L = static_cast<uint8_t>(bars * kBar);
            if (L == length_) current = g;
            if (only_ && L != only_) continue;
            if (g > bestGain) {
                second = best;
                secondGain = bestGain;
                best = L;
                bestGain = g;
            } else if (g > secondGain) {
                second = L;
                secondGain = g;
            }
        }
        if (length_ && best != length_ && bestGain < current + kSwitch) {   // the set length holds against a near-equal challenger
            second = best;
            secondGain = bestGain;
            best = length_;
            bestGain = current;
        }
        runnerUp_ = second;
        runnerUpGain_ = secondGain > -1 ? secondGain : 0;
        if (beats_ < kSettle) return;
        if (best && bestGain >= kGain) {
            length_ = best;
            gain_ = bestGain;
            anchor_ = start[best / kBar];
            weakFolds_ = 0;
        } else if (length_) {
            gain_ = current;   // the fold weakened: the phrase coasts on its count, for a history's worth of folds at most
            if (++weakFolds_ * kBar >= kHistory) {
                length_ = 0;
                weakFolds_ = 0;
            }
        }
    }

    /// The clarity at `L` beats, and in `start` the beat a phrase starts on; -1 while the history holds fewer than `kMinPhrases` phrases or no flux.
    float fold(uint8_t L, uint32_t& start) const {
        const uint32_t held = beats_ < kHistory ? beats_ : kHistory;
        const uint32_t phrases = held / L;
        if (phrases < kMinPhrases) return -1;
        const uint32_t first = beats_ - phrases * L;   // the oldest beat of the whole phrases laid side by side
        const uint16_t rows = static_cast<uint16_t>(L * kRows);
        float mean[kLongest * kRows];
        for (uint16_t i = 0; i < rows; i++) mean[i] = 0;
        for (uint32_t p = 0; p < phrases; p++)
            for (uint16_t i = 0; i < rows; i++) mean[i] += row(first + p * L, i);
        float sum = 0;
        uint32_t counted = 0;
        for (uint32_t p = 0; p < phrases; p++) {
            // This phrase against the mean of the others: a mean that includes it correlates with it by chance.
            float ma = 0, mb = 0;
            for (uint16_t i = 0; i < rows; i++) {
                const float a = row(first + p * L, i);
                ma += a;
                mb += (mean[i] - a) / (phrases - 1);
            }
            ma /= rows;
            mb /= rows;
            float ab = 0, aa = 0, bb = 0;
            for (uint16_t i = 0; i < rows; i++) {
                const float a = row(first + p * L, i) - ma, b = (mean[i] - row(first + p * L, i)) / (phrases - 1) - mb;
                ab += a * b;
                aa += a * a;
                bb += b * b;
            }
            if (aa <= 1e-9f || bb <= 1e-9f) continue;
            sum += ab / std::sqrt(aa * bb);
            counted++;
        }
        if (!counted) return -1;
        const float clarity = sum / counted - 2 * std::sqrt(2.0f / ((rows - 1) * phrases));

        // The bar start with the most flux in the mean phrase: the crash and the drop that open a phrase. A phrase starts a bar, so a fill's busier beat before it is passed over.
        float most = -1;
        uint8_t beat = 0;
        for (uint8_t b = 0; b < L; b++) {
            if (!barStart_[(first + b) % kHistory]) continue;
            float now = 0;
            for (uint8_t r = 0; r < kRows; r++) now += mean[b * kRows + r];
            if (now > most) {
                most = now;
                beat = b;
            }
        }
        start = first + beat;
        return clarity;
    }

    /// Row `i` of the phrase whose first beat is `beat`: beat `beat + i / kRows`, row `i % kRows`.
    float row(uint32_t beat, uint16_t i) const { return history_[(beat + i / kRows) % kHistory][i % kRows]; }
};

}  // namespace mm
