/// The tempo tracker's search; the public surface and the class layout live in AudioTempoSweep.h.
#include "core/services/AudioTempoSweep.h"

#include <cmath>
#include <cstring>

namespace mm {

void AudioTempoSweep::reset(float frameRate) {
    memset(sweepFrame_, 0, sizeof(sweepFrame_));
    memset(sweepStr_, 0, sizeof(sweepStr_));
    memset(combH_, 0, sizeof(combH_));
    memset(sweepScore_, 0, sizeof(sweepScore_));
    sweepCount_ = 0;
    sweepIndex_ = 0;
    sweepFirst_ = 0;
    sweepN_ = 0;
    sweepPerFrame_ = 0;
    sweepStride_ = 1;
    startedAt_ = 0;
    sweepNow_ = 0.0f;
    sweepTotal_ = 0.0f;
    active_ = false;
    wasNarrow_ = false;
    background_ = false;
    bgBpm_ = 0.0f;
    bgPhase_ = 0.0f;
    bgScore_ = 0.0f;
    bgLocalScore_ = 0.0f;
    bgQual_ = 0.0f;
    bgAgree_ = 0;
    bgHave_ = false;
    bgConverged_ = false;
    frameRate_ = frameRate;
    combFrames_ = static_cast<int>(lroundf(kCombWindowS * frameRate));
}

void AudioTempoSweep::start(bool background, const int32_t* frames, const float* str, int head, int count, uint32_t now, float aroundRate) {
    snapshot(frames, str, head, count, now);
    const bool lattice = (kQuantMode != 0);
    const int stride = lattice ? 2 : 1;
    int first = 0;
    int n = lattice ? ((kCandCount + 1) / 2) : kCandCount;
    bool narrow = false;
    if (!background && aroundRate > 0.0f) {  // locked and steady: look around the rate the grid believes
        if (lattice) narrowWindowLattice(aroundRate, first, n);
        else narrowWindow(aroundRate, first, n);
        narrow = true;
    }
    wasNarrow_ = narrow;
    background_ = background;
    if (background && sweepCount_ < kMinPeaks) {  // nothing to look at: the background says nothing and forgets what it had
        bgHave_ = false;
        bgConverged_ = false;
        bgAgree_ = 0;
    }
    sweepIndex_ = 0;
    sweepFirst_ = static_cast<uint16_t>(first);
    sweepN_ = static_cast<uint16_t>(n);
    sweepStride_ = static_cast<uint8_t>(stride);
    sweepPerFrame_ = static_cast<uint16_t>(n > kCandPerFrame ? kCandPerFrame : n);
    startedAt_ = now;
    sweepNow_ = static_cast<float>(now);
    active_ = (sweepCount_ >= kMinPeaks);
}

AudioTempoSweep::Outcome AudioTempoSweep::step(float currentRate) {
    Outcome out;
    if (!active_) return out;  // no search running
    const int first = sweepFirst_;
    const int total = sweepN_;
    for (int i = 0; i < sweepPerFrame_ && sweepIndex_ < total; i++) {
        const int idx = first + static_cast<int>(sweepStride_) * sweepIndex_++;
        const float bpm = kBpmLo + static_cast<float>(idx) * kCandStep;
        float phase = 0.0f;
        sweepScore_[idx] = combScore(bpm, &phase);  // every candidate's score: the plateau needs the whole profile
    }
    if (sweepIndex_ < total) {
        out.kind = Outcome::kSearching;
        return out;
    }
    Pick pick = plateauPick(sweepFirst_, sweepN_, sweepStride_, !wasNarrow_);
    pick = levelCheck(pick);  // the winner against its relatives, before the outcome is built
    active_ = false;
    out.rate = pick.rate;
    out.score = chanceCorrect(pick.score, 60.0f * frameRate_ / pick.rate);  // the outcome's score is the corrected one: the gates read it
    out.phase = pick.phase;
    out.level = pick.level;
    out.margin = runnerUpMargin(pick);
    out.wide = !wasNarrow_;
    if (background_) {
        out.kind = Outcome::kBackground;
        backgroundResult(pick.rate, out.score, pick.phase, currentRate, out);
        return out;
    }
    out.kind = Outcome::kForeground;
    return out;
}

void AudioTempoSweep::hold() {
    active_ = false;
    background_ = false;
    bgHave_ = false;
    bgConverged_ = false;
    bgAgree_ = 0;
    bgBpm_ = 0.0f;
    bgPhase_ = 0.0f;
    bgScore_ = 0.0f;
    bgLocalScore_ = 0.0f;
}

float AudioTempoSweep::combAt(float bpm, float* phase) {
    return combScore(bpm, phase);
}

float AudioTempoSweep::boxHalf(float per) const {
    return fminf((kTolMs * 0.001f) * frameRate_, kKernelFrac * per);
}

float AudioTempoSweep::coverage(float bpm, float phase) {
    float per = 0.0f, half = 0.0f;
    int kmin = 0, beats = 0;
    if (!spanFor(bpm, phase, per, half, kmin, beats)) return 0.0f;
    const int hits = confirmedHits(per, phase, kmin, beats, half, 0.0f);
    if (hits <= 0) return 0.0f;
    return static_cast<float>(hits) / static_cast<float>(beats);
}

float AudioTempoSweep::coverageStrong(float bpm, float phase) {
    float per = 0.0f, half = 0.0f;
    int kmin = 0, beats = 0;
    if (!spanFor(bpm, phase, per, half, kmin, beats)) return 0.0f;
    float maxStr = 0.0f;
    for (int i = 0; i < sweepCount_; i++) {
        if (sweepStr_[i] > maxStr) maxStr = sweepStr_[i];
    }
    const int hits = confirmedHits(per, phase, kmin, beats, half, kStrongFrac * maxStr);
    if (hits <= 0) return 0.0f;
    return static_cast<float>(hits) / static_cast<float>(beats);
}

bool AudioTempoSweep::spanFor(float bpm, float phase, float& per, float& half, int& kmin, int& beats) const {
    if (sweepCount_ <= 0 || bpm <= 0.0f) return false;
    per = 60.0f * frameRate_ / bpm;
    if (per <= 0.0f) return false;
    half = boxHalf(per);
    beatSpan(per, phase, kmin, beats);
    return true;
}

int AudioTempoSweep::slotIndex(int32_t frame, float phase, float per, float half, int kmin, int beats) const {
    float dev = 0.0f;
    const int k = beatIndex(frame, phase, per, dev);
    if (fabsf(dev) > half) return -1;  // too far off the grid to count
    const int idx = k - kmin;
    if (idx < 0 || idx >= beats) return -1;
    return idx;
}

void AudioTempoSweep::slotMaxima(float phase, float per, float half, int kmin, int beats, float* slotMax) const {
    for (int i = 0; i < beats; i++) slotMax[i] = -1.0f;
    for (int i = 0; i < sweepCount_; i++) {
        const int idx = slotIndex(sweepFrame_[i], phase, per, half, kmin, beats);
        if (idx < 0) continue;
        if (sweepStr_[i] > slotMax[idx]) slotMax[idx] = sweepStr_[i];
    }
}

void AudioTempoSweep::snapshot(const int32_t* frames, const float* str, int head, int count, uint32_t now) {
    const int32_t cutoff = static_cast<int32_t>(now) - combFrames_;
    const float tauFrames = kRecencyTauS * frameRate_;  // the recency weight's time constant, in frames
    int n = 0;
    float total = 0.0f;
    float sumSq = 0.0f;
    for (int i = 0; i < count && n < kPeakRing; i++) {  // the window's onsets, newest first
        const int idx = (head + kPeakRing - 1 - i) & (kPeakRing - 1);
        if (frames[idx] < cutoff) break;  // too old
        const float age = static_cast<float>(static_cast<int32_t>(now) - frames[idx]);
        const float weighted = str[idx] * expf(-age / tauFrames);  // recent onsets count more: a tempo change must outvote the old grid, not wait for it to age out of the window
        sweepFrame_[n] = frames[idx];
        sweepStr_[n] = weighted;
        total += weighted;
        sumSq += weighted * weighted;
        n++;
    }
    sweepCount_ = static_cast<uint16_t>(n);
    sweepTotal_ = total;
    sweepSumSq_ = sumSq;
}

float AudioTempoSweep::combScore(float bpm, float* phase) {
    if (bpm <= 0.0f) return 0.0f;
    const float period = 60.0f * frameRate_ / bpm;
    const float invPeriod = 1.0f / period;
    binPhases(period, invPeriod);
    if (sweepTotal_ <= 0.0f) return 0.0f;
    const float raw = bestBox(period, phase) / sweepTotal_;
    return raw;  // raw: the plateau pick needs the comb's own geometry, and the chance level tilts it toward slow rates; the correction is applied where a score feeds a decision
}

float AudioTempoSweep::correctedAt(float bpm, float* phase) {
    if (bpm <= 0.0f) return 0.0f;
    const float period = 60.0f * frameRate_ / bpm;
    return chanceCorrect(combScore(bpm, phase), period);
}

float AudioTempoSweep::chanceCorrect(float raw, float period) const {
    if (sweepSumSq_ <= 0.0f) return raw;
    int whole = 1;
    float frac = 0.0f;
    kernelHalfWidth(period, whole, frac);
    const float w = (2.0f * (static_cast<float>(whole) + frac) + 1.0f) / static_cast<float>(kCombBins);  // the box's share of the circle
    const float nEff = sweepTotal_ * sweepTotal_ / sweepSumSq_;  // the effective onset count, strength-weighted
    float chance = w + kChanceK * sqrtf(w * (1.0f - w) / nEff);  // the circle share, plus the best-of-64-boxes lift
    if (chance > 0.95f) chance = 0.95f;  // the correction's denominator stays open
    const float corrected = (raw - chance) / (1.0f - chance);
    if (corrected < 0.0f) return 0.0f;
    if (corrected > 1.0f) return 1.0f;
    return corrected;
}

void AudioTempoSweep::binPhases(float period, float invPeriod) {
    const float binScale = static_cast<float>(kCombBins) * invPeriod;
    for (int i = 0; i < kCombBins; i++) combH_[i] = 0.0f;
    for (int i = 0; i < sweepCount_; i++) {
        const float f = static_cast<float>(sweepFrame_[i]);
        const float k = floorf(f * invPeriod);  // the quotient is a floor, not a truncation: the frames can be negative
        float u = f - k * period;
        if (u < 0.0f) u += period;
        else if (u >= period) u -= period;
        int bin = static_cast<int>(u * binScale);
        if (bin < 0) bin = 0;
        else if (bin >= kCombBins) bin = kCombBins - 1;
        combH_[bin] += sweepStr_[i];
    }
}

float AudioTempoSweep::bestBox(float period, float* phase) {
    int w = 1;
    float frac = 0.0f;
    kernelHalfWidth(period, w, frac);
    float win = 0.0f;
    for (int j = -w; j <= w; j++) win += combH_[(j + kCombBins) & (kCombBins - 1)];
    float box[kCombBins];  // the box's value at every bin, so the plateau's extent can be found
    float best = -1.0e30f;
    int bestBin = 0;
    for (int i = 0; i < kCombBins; i++) {
        if (i > 0) {  // the rolling sum slides one bin at a time, so the smoothing is O(bins)
            win += combH_[(i + w) & (kCombBins - 1)] - combH_[(i - w - 1 + kCombBins) & (kCombBins - 1)];
        }
        float v = win;
        if (frac < 0.5f) {  // the exact box is the integer one minus half a bin at each end
            v -= (0.5f - frac) * (combH_[(i - w + kCombBins) & (kCombBins - 1)] + combH_[(i + w) & (kCombBins - 1)]);
        } else {  // ... or plus half a bin of each of the bins just outside it
            v += (frac - 0.5f) * (combH_[(i - w - 1 + kCombBins) & (kCombBins - 1)] + combH_[(i + w + 1) & (kCombBins - 1)]);
        }
        box[i] = v;
        if (v > best) { best = v; bestBin = i; }
    }
    if (phase) *phase = plateauCentre(bestBin, box, best, period);
    return best;
}

float AudioTempoSweep::plateauCentre(int bestBin, const float* box, float best, float period) const {
    const float eps = best * 1e-5f;  // relative: the rolling sum is float32 and the plateau's values drift in the last bits
    int left = 0, right = 0;
    while (left < kCombBins && box[(bestBin - 1 - left + kCombBins) & (kCombBins - 1)] >= best - eps) left++;
    while (right < kCombBins && box[(bestBin + 1 + right) & (kCombBins - 1)] >= best - eps) right++;
    const float centre = static_cast<float>(bestBin) + 0.5f * static_cast<float>(right - left) + 0.5f;
    return centre / static_cast<float>(kCombBins) * period;
}

void AudioTempoSweep::kernelHalfWidth(float period, int& whole, float& frac) const {
    const float tolFrames = (kTolMs * 0.001f) * frameRate_;
    float wf = tolFrames / period * static_cast<float>(kCombBins);
    const float wfrac = kKernelFrac * static_cast<float>(kCombBins);  // the box never covers more than 14 % of the period
    if (wf > wfrac) wf = wfrac;
    const float wmax = static_cast<float>(kCombBins / 2 - 2);  // one bin's clearance at each end
    if (wf < 1.0f) wf = 1.0f;
    if (wf > wmax) wf = wmax;
    const int w = static_cast<int>(wf);  // wf is positive, so this is the floor
    whole = w;
    frac = wf - static_cast<float>(w);
}

float AudioTempoSweep::priorAt(float bpm) const {
    if (bpm <= 0.0f) return 0.0f;
    const float z = logf(bpm / kPriorBpm) / kPriorSigma;
    return kPriorWeight * z * z;
}

void AudioTempoSweep::narrowWindow(float bpm, int& first, int& count) const {
    const int half = static_cast<int>(lroundf(kNarrowBpm / kCandStep));
    const int centre = static_cast<int>(lroundf((bpm - kBpmLo) / kCandStep));
    int f = centre - half;
    int c = 2 * half + 1;
    if (f < 0) { c += f; f = 0; }  // the window ran off the bottom
    if (f + c > kCandCount) c = kCandCount - f;
    if (f < 0) f = 0;
    if (c < 1) c = 1;
    first = f;
    count = c;
}

void AudioTempoSweep::narrowWindowLattice(float bpm, int& first, int& count) const {
    const int half = static_cast<int>(lroundf(kNarrowBpm / (kCandStep * 2.0f)));  // four lattice steps
    const int centre = static_cast<int>(lroundf((bpm - kBpmLo) / (kCandStep * 2.0f))) * 2;  // an even index
    int f = centre - half * 2;
    int c = 2 * half + 1;
    if (f < 0) {  // the window ran off the bottom
        c -= ((-f + 1) / 2);  // ... and lost that many lattice points
        f = 0;
    }
    const int last = f + (c - 1) * 2;
    if (last > kCandCount - 1) c = (kCandCount - 1 - f) / 2 + 1;  // ... or off the top
    if (c < 1) c = 1;
    first = f;
    count = c;
}

AudioTempoSweep::Pick AudioTempoSweep::plateauPick(uint16_t first, uint16_t n, uint8_t stride, bool usePrior) {
    const int st = stride;
    const int f = first;
    const int last = f + static_cast<int>(n - 1) * st;
    float bestVal = -1e18f;
    float bestRate = kBpmLo + static_cast<float>(f) * kCandStep;
    for (int idx = f; idx <= last; idx += st) {
        const float s = sweepScore_[idx];
        if (idx > f && sweepScore_[idx - st] >= s) continue;  // the run's peak is a local maximum; an equal left neighbour is part of the same run, and the earlier member finds it
        if (idx < last && sweepScore_[idx + st] > s) continue;  // the right test is strict: >= on both sides skips every member of a flat-topped run and leaves the plateau invisible (Troy's code had >= here)
        const float centre = plateauExtent(idx, f, last, st, s);
        const float val = plateauValue(s, idx, centre, usePrior);
        if (val > bestVal) {
            bestVal = val;
            bestRate = centre;
        }
    }
    Pick pick;
    pick.rate = bestRate;
    float ph = 0.0f;  // the phase at the plateau's centre rate: the phase drifts with the rate over the snapshot's frames, so a phase measured at the peak's lattice rate can sit half a period off the reported rate's grid
    pick.score = combScore(bestRate, &ph);
    pick.phase = ph;
    return pick;
}

float AudioTempoSweep::plateauExtent(int idx, int first, int last, int stride, float s) const {
    const float tol = fmaxf(1e-9f, s * kPlateauTol);
    int a = idx;
    while (a - stride >= first && sweepScore_[a - stride] >= s - tol) a -= stride;
    int b = idx;
    while (b + stride <= last && sweepScore_[b + stride] >= s - tol) b += stride;
    return 0.5f * ((kBpmLo + static_cast<float>(a) * kCandStep) + (kBpmLo + static_cast<float>(b) * kCandStep));
}

float AudioTempoSweep::plateauValue(float s, int idx, float centre, bool usePrior) const {
    float val = s;
    if (kQuantMode >= 2 && (idx & 3) == 2) val -= kQuantBonus;  // the whole numbers are preferred
    if (usePrior) val -= priorAt(centre);  // the prior chooses between plateaus, never inside one
    return val;
}

float AudioTempoSweep::runnerUpMargin(const Pick& pick) const {
    const int centre = static_cast<int>(lroundf((pick.rate - kBpmLo) / kCandStep));
    float bestRaw = 0.0f;
    float bestRate = 0.0f;
    for (int i = 0; i < sweepN_; i++) {
        const int idx = sweepFirst_ + static_cast<int>(sweepStride_) * i;
        if (idx >= centre - kPlateauExcl && idx <= centre + kPlateauExcl) continue;  // inside the winner's plateau: the same hypothesis
        if (sweepScore_[idx] > bestRaw) {
            bestRaw = sweepScore_[idx];
            bestRate = kBpmLo + static_cast<float>(idx) * kCandStep;
        }
    }
    if (bestRate <= 0.0f) return 0.0f;  // no alternative in the searched range
    const float winCorr = chanceCorrect(pick.score, 60.0f * frameRate_ / pick.rate);
    const float runCorr = chanceCorrect(bestRaw, 60.0f * frameRate_ / bestRate);
    const float m = winCorr - runCorr;
    return m > 0.0f ? m : 0.0f;
}

float AudioTempoSweep::alternation(float bpm, float phase) const {
    float per = 0.0f, half = 0.0f;
    int kmin = 0, beats = 0;
    if (!spanFor(bpm, phase, per, half, kmin, beats)) return 0.0f;
    float slotMax[64];  // each beat slot's strongest onset: a weak onset never dilutes a slot a strong one already fills
    slotMaxima(phase, per, half, kmin, beats, slotMax);
    float evenSum = 0.0f, oddSum = 0.0f;
    int evenN = 0, oddN = 0;
    for (int i = 0; i < beats; i++) {  // the slots that heard anything, split by the beat's parity
        if (slotMax[i] < 0.0f) continue;
        if (((kmin + i) & 1) == 0) {
            evenSum += slotMax[i];
            evenN++;
        } else {
            oddSum += slotMax[i];
            oddN++;
        }
    }
    if (evenN < kMinPeaks || oddN < kMinPeaks) return 0.0f;  // thin evidence: a side with fewer beats than the search's own minimum cannot halve the beat — a missed kick or two would tip it
    const float evenMean = evenSum / static_cast<float>(evenN);
    const float oddMean = oddSum / static_cast<float>(oddN);
    const float hi = fmaxf(evenMean, oddMean);
    if (hi <= 0.0f) return 0.0f;
    return 1.0f - fminf(evenMean, oddMean) / hi;
}

AudioTempoSweep::Pick AudioTempoSweep::levelCheck(const Pick& winner) {
    Pick best = winner;
    const float winPeriod = 60.0f * frameRate_ / winner.rate;
    const float winQual = chanceCorrect(winner.score, winPeriod) * coverageStrong(winner.rate, winner.phase);
    const bool forcedHalf = alternation(winner.rate, winner.phase) > kAlternationMax;  // the winner's grid alternates strong and weak beats: it is a doubled subdivision pattern, and the beat is its half
    static constexpr float kRelatives[4] = {1.5f, 2.0f / 3.0f, 4.0f / 3.0f, 3.0f / 4.0f};
    float bestQual = winQual;
    for (int i = 0; i < 4; i++) {  // the in-band relatives revise the rate
        float qual = 0.0f;
        const Pick rel = bestInWindow(winner.rate * kRelatives[i], kBpmLo, kBpmHi, qual);
        if (qual > bestQual + kLevelMargin) {
            bestQual = qual;
            best = rel;
        }
    }
    static constexpr float kOctaves[2] = {2.0f, 0.5f};
    float bestOctQual = winQual;
    float bestOctLevel = 1.0f;
    for (int i = 0; i < 2; i++) {  // the half and the double move the level, not the grid
        float qual = 0.0f;
        const Pick oct = bestInWindow(winner.rate * kOctaves[i], kLevelLo, kLevelHi, qual);
        if (kOctaves[i] == 2.0f && alternation(oct.rate, oct.phase) > kAlternationMax) continue;  // the odd beats are much weaker than the even ones: subdivisions, not beats, so the double is not the level
        if (qual > bestOctQual + kLevelMargin) {
            bestOctQual = qual;
            bestOctLevel = kOctaves[i];
        }
    }
    if (bestOctQual > bestQual) {  // the octave hypothesis beats the best in-band answer: the level moves, the grid stays
        best = winner;
        best.level = bestOctLevel;
    }
    if (forcedHalf && best.rate == winner.rate) best.level = 0.5f;  // the alternation overrules the quality comparison: a grid that alternates is a doubled pattern, whatever the fold says; only when the grid did not move, since the alternation was measured at the winner's rate
    return best;
}

AudioTempoSweep::Pick AudioTempoSweep::bestInWindow(float centre, float lo, float hi, float& qual) {
    Pick best;
    qual = 0.0f;
    for (int s = -kLevelWindowSteps; s <= kLevelWindowSteps; s++) {
        const float rate = centre + static_cast<float>(s) * kCandStep * 2.0f;
        if (rate < lo || rate > hi) continue;
        const float period = 60.0f * frameRate_ / rate;
        float phase = 0.0f;
        const float raw = combScore(rate, &phase);
        const float q = chanceCorrect(raw, period) * coverageStrong(rate, phase);
        if (q > qual) {
            qual = q;
            best.rate = rate;
            best.score = raw;
            best.phase = phase;
        }
    }
    return best;
}

void AudioTempoSweep::backgroundResult(float rate, float score, float phase, float currentRate, Outcome& out) {
    float ph = 0.0f;  // the steady rate's own evidence, on the same window, corrected like the background's
    bgLocalScore_ = correctedAt(currentRate, &ph);
    if (bgHave_) {
        const float d = fabsf(rate - bgBpm_);
        const float per = 60.0f * frameRate_ / rate;
        float a = fmodf(bgPhase_ / per, 1.0f);
        if (a < 0.0f) a += 1.0f;
        float b = fmodf(phase / per, 1.0f);
        if (b < 0.0f) b += 1.0f;
        float dp = fabsf(a - b);
        if (dp > 0.5f) dp = 1.0f - dp;
        if (d <= kBgAgreeBpm && dp <= kBgPhaseTol) {
            if (bgAgree_ < 255) bgAgree_++;
        } else {
            bgAgree_ = 1;
        }
    } else {
        bgAgree_ = 1;
    }
    bgBpm_ = rate;
    bgPhase_ = phase;
    bgScore_ = score;
    bgQual_ = score * coverage(rate, phase);  // the background's own quality: alignment times coverage
    bgHave_ = true;
    bgConverged_ = (bgAgree_ >= kBgConverge);
    backgroundPromote(currentRate, out);
}

void AudioTempoSweep::backgroundPromote(float currentRate, Outcome& out) const {
    if (!bgHave_ || !bgConverged_) return;
    if (bgScore_ < kMinScore) return;  // it has found nothing
    if (fabsf(bgBpm_ - currentRate) <= kNearBpm) {  // a nudge: the background agrees about where the tempo is
        if (bgQual_ < kQualMin) return;
        out.nudgePhase = true;
        return;
    }
    if (bgLocalScore_ < kMinScore) {  // the rate the grid is at has died
        if (bgScore_ < kDeadAccept) return;
        out.promote = true;
        return;
    }
    if (bgScore_ < bgLocalScore_ * (1.0f + kBgMargin)) return;  // a long way off and the steady rate alive: clearly better, or nothing
    out.promote = true;
}

void AudioTempoSweep::beatSpan(float per, float phase, int& kmin, int& beats) const {
    float fmin = static_cast<float>(sweepFrame_[0]);
    float fmax = static_cast<float>(sweepFrame_[0]);
    for (int i = 1; i < sweepCount_; i++) {
        const float f = static_cast<float>(sweepFrame_[i]);
        if (f < fmin) fmin = f;
        if (f > fmax) fmax = f;
    }
    kmin = static_cast<int>(lroundf((fmin - phase) / per));
    const int kmax = static_cast<int>(lroundf((fmax - phase) / per));
    int b = kmax - kmin + 1;
    if (b < 1) b = 1;
    if (b > 64) b = 64;  // the mask's width
    beats = b;
}

int AudioTempoSweep::beatIndex(int32_t frame, float phase, float per, float& dev) const {
    const float u = static_cast<float>(frame) - phase;
    const int k = static_cast<int>(lroundf(u / per));
    dev = u - static_cast<float>(k) * per;
    return k;
}

int AudioTempoSweep::confirmedHits(float per, float phase, int kmin, int beats, float half, float minStr) const {
    uint64_t mask = 0;  // one bit per beat slot
    for (int i = 0; i < sweepCount_; i++) {
        if (sweepStr_[i] < minStr) continue;  // too weak to confirm a beat
        const int idx = slotIndex(sweepFrame_[i], phase, per, half, kmin, beats);
        if (idx < 0) continue;
        mask |= (static_cast<uint64_t>(1) << idx);
    }
    int hits = 0;
    while (mask) {
        hits += static_cast<int>(mask & 1u);
        mask >>= 1;
    }
    return hits;
}

}  // namespace mm
