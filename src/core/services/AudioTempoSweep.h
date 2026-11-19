#pragma once

#include <cstdint>

namespace mm {

/// The tempo tracker's search: a comb over the onset window, scored candidate by candidate, whose winner is the plateau's centre rather than its edge.
/// @attribution troyhacks
///
/// @technical
///
/// ## The comb
///
/// For a candidate period every onset's phase offset within the period is taken modulo the period: on a clean beat train the offsets cluster, and the size of that cluster, as a fraction of the window's onset strength, is the score. The kernel is a box of half-width 70 ms, capped at 14 % of the period so a fast candidate's box cannot cover a bigger share of its beat than a slow one's.
///
/// The box is the exact integral of the 64-bin phase histogram over the requested half-width, with fractional edge bins: rounding the half-width to whole bins put a sawtooth on the score profile, and the sawtooth's period was the candidate spacing.
///
/// ## The rate is the plateau's centre
///
/// The comb's score saturates: on a clean 128 BPM track every candidate from 126.5 to 129.0 scores exactly 1.000, so the argmax of a flat run is its first member and the published rate wanders by half a BPM at random. The rate is the centre of the run of candidates the comb cannot separate, and the prior chooses between plateaus only, never inside one.
///
/// The phase is the plateau's centre too: the box is up to 27 bins wide at 175 BPM, and the argmax's leading edge would anchor the grid up to 70 ms early.
///
/// ## Two searches
///
/// The whole-band search looks at all 341 candidates, 171 on the lattice, spread over four frames; the narrow search looks at the rate the grid believes plus or minus 2 BPM and finishes in one frame. The onset window is snapshotted at the start, so every candidate is scored against the same onsets.
///
/// The background runs on its own clock, looks at the whole band, and nominates a rate and a grid of its own. It does not touch the grid until it has converged, the same rate and the same grid over six searches in a row, and until its evidence is clearly better than the steady rate's on the same window.
///
/// ## The lattice
///
/// The candidates are the multiples of half a BPM, with the whole numbers preferred by 0.01 in score units: the lattice's own resolution is what the narrow window and the near test are measured in.
///
/// ## Chance
///
/// A comb score of 0.35 on random onsets is not evidence: the box alone covers a share of the circle, and the best of 64 boxes reaches further. The chance level is the box's share plus 1.5 standard deviations of the best-box lift, over the strength-weighted onset count; the corrected score is how far the raw score sits above it, scaled to the headroom.
///
/// The pick still runs on the raw score, because the chance level varies with the rate and would tilt the plateaus; the correction applies where a score feeds a decision, the outcome, the background's comparison and the grid's own measurement.
///
/// Noise reads a corrected score of at most 0.135, a kick track at least 0.73, and the least believed score is 0.35, in the middle of the gap.
///
/// ## The level
///
/// The comb cannot tell a rate from its relatives: a 4:3 subharmonic scores 0.35-0.5 on real onsets, and the prior sits almost penalty-free near 125, so a diluted true rate loses to its own relative. The fold can: at the true rate the onsets are on every beat, at the 4:3 relative on every fourth beat of its own grid.
///
/// So the winner is rescored against its in-band relatives, 3:2, 2:3, 4:3 and 3:4, each searched 5 BPM either side because the winner is a plateau's centre, not the exact relative, by quality, the corrected score times the coverage. A relative takes the level only when its quality beats the winner's by 0.10.
///
/// The octave relatives are out of band for any winner here, so they move the level, not the grid: the half and the double are scored the same way, over 40-360 BPM, and the winner's level is the one whose quality wins. The grid stays in the band; the state publishes the grid's rate times the level.
///
/// The double has one more gate: at the doubled rate the onsets on the odd beats must be nearly as strong as those on the even ones. Hats on the off-beats are strong enough to confirm a beat, so the fold alone reads a 120 track with hats as 240; the alternation is what says the odd beats are subdivisions, not beats.
///
/// ## The window
///
/// The window is 12 s, and every onset in it is weighted by its age: its strength decays by e every 5 s, so a tempo change outvotes the old grid in seconds instead of waiting for the old onsets to age out of the window. The weighting applies to every search, acquisition included, and the chance level reads the weighted strengths, so the correction stays honest.
struct AudioTempoSweep {
    /// A finished search's outcome: the plateau's centre, its evidence and its grid, and what the background wants done with the grid.
    struct Outcome {
        /// What the step returned: a search still running, no search active, or a finished foreground or background search.
        enum Kind : uint8_t { kSearching, kIdle, kForeground, kBackground };
        Kind kind = kIdle;          ///< which of the four
        float rate = 0.0f;          ///< the plateau's centre, BPM
        float score = 0.0f;         ///< ... and its evidence, 0..1
        float phase = 0.0f;         ///< ... and its grid, in frames
        float level = 1.0f;         ///< the true rate's level against the grid: 1, or the half or double that won the fold
        float margin = 0.0f;        ///< the winner's corrected score above the best alternative outside its plateau: how much the fold prefers this rate
        bool wide = false;          ///< the search looked at the whole band, so the margin is the global discrimination, not the local peak's
        bool promote = false;       ///< the background's grid should replace the running one
        bool nudgePhase = false;    ///< the background's phase should be taken, the rate kept
    };

    /// A plateau pick's answer: the plateau's centre, its evidence and its grid.
    struct Pick {
        float rate = 0.0f;    ///< the plateau's centre, BPM
        float score = 0.0f;   ///< ... and its evidence, 0..1
        float phase = 0.0f;   ///< ... and its grid, in frames
        float level = 1.0f;   ///< the true rate's level against the grid: 1, or the half or double that won the fold
    };

    /// Derive the window from the frame rate and clear the state.
    void reset(float frameRate);

    /// Start a search: the background's looks at the whole band, a foreground search looks around the grid's rate when one is given: @xref{two-searches}
    void start(bool background, const int32_t* frames, const float* str, int head, int count, uint32_t now, float aroundRate);

    /// Whether a search is running.
    bool active() const { return active_; }

    /// Score the next chunk of candidates; a finished search returns its outcome, an unfinished one kSearching.
    Outcome step(float currentRate);

    /// Forget the search and the background's nomination, so the music that comes back is measured afresh.
    void hold();

    /// The comb at a rate, on the last search's snapshot: the grid's own measurement: @xref{the-comb}
    float combAt(float bpm, float* phase);

    /// The comb at a rate, chance-corrected: the score the gates and the published confidence read: @xref{chance}
    float correctedAt(float bpm, float* phase);

    /// The fraction of the beats a rate and phase predict that the last snapshot confirms.
    float coverage(float bpm, float phase);

    /// The same fraction, counting only onsets at least 0.3 of the snapshot's strongest: noise must not confirm a beat the music leaves empty.
    float coverageStrong(float bpm, float phase);

    /// The frame the last search started at: the grid anchors to the measurement's frame, not the frame it finished at.
    uint32_t startedAt() const { return startedAt_; }

private:
    static constexpr float kBpmLo = 90.0f;       // the band's bottom
    static constexpr float kBpmHi = 175.0f;      // ... and top
    static constexpr float kCandStep = 0.25f;    // the candidate grid's step, BPM
    static constexpr int kCandCount = 341;       // ... and its size
    static constexpr float kQuantBonus = 0.01f;  // the half integers' penalty, in score units
    static constexpr int kCombBins = 64;         // the phase histogram's bins
    static constexpr int kCandPerFrame = 44;     // candidates scored in one frame
    static constexpr float kNarrowBpm = 2.0f;    // the narrow window's half-width, BPM
    static constexpr float kBgAgreeBpm = 0.60f;  // the background agrees within this rate
    static constexpr float kBgPhaseTol = 0.50f;  // ... and this phase, in beats
    static constexpr float kNearBpm = 1.50f;     // a rate this close is the same grid
    static constexpr float kDeadAccept = 0.50f;  // a far rate needs this much evidence to be taken
    static constexpr float kPlateauTol = 1e-4f;  // the score's flat-top tolerance
    static constexpr float kQualMin = 0.20f;     // the quality floor below which a phase is not worth taking
    static constexpr float kBgMargin = 0.70f;    // the background must beat the steady rate by this much
    static constexpr float kCombWindowS = 12.0f;  // the onset window, seconds
    static constexpr float kPriorBpm = 125.0f;   // the prior's centre
    static constexpr float kPriorSigma = 0.35f;  // ... and its width, in log BPM
    static constexpr float kPriorWeight = 0.3f;  // ... and its weight: a gentle preference, not a bias (hook 1)
    static constexpr float kMinScore = 0.35f;    // the least evidence a rate needs to be believed
    static constexpr float kLevelMargin = 0.10f;  // a relative must beat the winner's quality by this much to take the level (hook 3)
    static constexpr int kLevelWindowSteps = 10;  // ... searched 10 lattice steps (5 BPM) either side of each relative: the winner is a plateau's centre, not the exact relative
    static constexpr float kLevelLo = 40.0f;      // the half and double hypotheses' range: the half of 90, less the window (hook 4)
    static constexpr float kLevelHi = 360.0f;     // ... and the double of 175, plus the window
    static constexpr float kStrongFrac = 0.3f;    // an onset confirms a beat for the level check at this share of the snapshot's strongest
    static constexpr float kAlternationMax = 0.25f;  // the double is rejected when its odd beats are this much weaker than its even ones: subdivisions, not beats
    static constexpr float kRecencyTauS = 5.0f;   // the recency weight's time constant: an onset's strength decays by e every 5 s (hook 5)
    static constexpr int kPlateauExcl = 10;       // the runner-up search skips this many lattice steps either side of the winner: the widest plateau seen is 2.5 BPM
    static constexpr float kChanceK = 1.5f;      // the chance level's max-statistic factor, in standard deviations (hook 1)
    static constexpr float kKernelFrac = 0.14f;  // the kernel's widest share of the period
    static constexpr float kTolMs = 70.0f;       // the comb's tolerance, ms
    static constexpr int kMinPeaks = 8;          // the least onsets a search needs
    static constexpr uint8_t kBgConverge = 6;    // the background's agreeing searches before it may move the grid
    static constexpr int kPeakRing = 256;        // the snapshot's capacity, as the onset ring's
    static constexpr uint8_t kQuantMode = 2;     // the lattice with the whole numbers preferred

    /// The window's onsets, newest first, from the onset ring, each weighted by its age: @xref{the-window}
    void snapshot(const int32_t* frames, const float* str, int head, int count, uint32_t now);

    /// The comb's score at one candidate rate, on the snapshot; phaseOut gets the best phase, in frames.
    float combScore(float bpm, float* phase);

    /// A raw comb score corrected for chance: how far above the level random onsets reach, scaled to the headroom.
    float chanceCorrect(float raw, float period) const;

    /// The onset phases binned at one candidate's period.
    void binPhases(float period, float invPeriod);

    /// The box's best value over the binned phases, and its plateau's centre as the phase.
    float bestBox(float period, float* phase);

    /// The plateau's centre in bins, from the box's values around the argmax.
    float plateauCentre(int bestBin, const float* box, float best, float period) const;

    /// The kernel's half-width in bins, split into its whole and fractional parts.
    void kernelHalfWidth(float period, int& whole, float& frac) const;

    /// The prior's penalty at a rate: the log-normal pull toward 125 BPM.
    float priorAt(float bpm) const;

    /// The narrow window's candidate indices around a rate.
    void narrowWindow(float bpm, int& first, int& count) const;

    /// The narrow window on the lattice: the same span, half the candidates: @xref{the-lattice}
    void narrowWindowLattice(float bpm, int& first, int& count) const;

    /// The best plateau's centre, score and phase among the searched candidates: @xref{the-rate-is-the-plateaus-centre}
    Pick plateauPick(uint16_t first, uint16_t n, uint8_t stride, bool usePrior);

    /// The winner against its relatives: the fold, not the prior, picks the level: @xref{the-level}
    Pick levelCheck(const Pick& winner);

    /// How much weaker the odd beats are than the even ones, 0..1: hats on the off-beats alternate, beats do not. Each slot counts its strongest onset; a side with fewer beats than the search's minimum reads 0.
    float alternation(float bpm, float phase) const;

    /// The winner's corrected score above the best alternative outside its plateau, over the searched range.
    float runnerUpMargin(const Pick& pick) const;

    /// The best-quality rate in a window around a centre, by corrected score times coverage; qual answers its quality, 0 when the window holds no candidate.
    Pick bestInWindow(float centre, float lo, float hi, float& qual);

    /// One plateau's extent around its peak, and its centre's rate.
    float plateauExtent(int idx, int first, int last, int stride, float s) const;

    /// One plateau's value: its evidence, less the lattice's preference and the prior's penalty.
    float plateauValue(float s, int idx, float centre, bool usePrior) const;

    /// The background's search, finished: record the nomination, and let it try to move the grid.
    void backgroundResult(float rate, float score, float phase, float currentRate, Outcome& out);

    /// The background's verdict: whether the grid moves, and how far.
    void backgroundPromote(float currentRate, Outcome& out) const;

    /// The beats the snapshot spans at a rate and phase, as the first beat's index and the count.
    void beatSpan(float per, float phase, int& kmin, int& beats) const;

    /// The period, the box half-width and the beat span at a rate and phase: false when there is nothing to measure.
    bool spanFor(float bpm, float phase, float& per, float& half, int& kmin, int& beats) const;

    /// The beat slot an onset falls in, or -1 when it sits too far off the grid to count.
    int slotIndex(int32_t frame, float phase, float per, float half, int kmin, int beats) const;

    /// Each beat slot's strongest onset, -1 where a slot heard nothing.
    void slotMaxima(float phase, float per, float half, int kmin, int beats, float* slotMax) const;

    /// The beats the snapshot confirms at a rate and phase, as a count of hit slots; only onsets at or above minStr confirm one.
    int confirmedHits(float per, float phase, int kmin, int beats, float half, float minStr) const;

    /// An onset's beat index at a rate and phase, and how far off the beat it sits, in frames.
    int beatIndex(int32_t frame, float phase, float per, float& dev) const;

    /// The half-width of the box that counts an onset on the grid, at a period; the tolerance, capped at 14% of the period.
    float boxHalf(float per) const;

    int32_t sweepFrame_[kPeakRing];  // the window's onsets, snapshotted
    float sweepStr_[kPeakRing];      // ... and their strengths
    uint16_t sweepCount_ = 0;        // how many are in the snapshot
    uint16_t sweepIndex_ = 0;        // how many candidates have been scored
    uint16_t sweepFirst_ = 0;        // the first candidate's index
    uint16_t sweepN_ = 0;            // how many candidates this search has
    uint16_t sweepPerFrame_ = 0;     // how many are scored in one frame
    uint8_t sweepStride_ = 1;        // 1 = every candidate, 2 = the lattice
    uint32_t startedAt_ = 0;         // the frame the search started at
    float sweepNow_ = 0.0f;          // ... as a float, for the grid's anchor
    float sweepTotal_ = 0.0f;        // the window's onset strength, summed
    float sweepSumSq_ = 0.0f;        // ... and squared, for the chance level's effective onset count
    float combH_[kCombBins];         // the phase histogram
    float sweepScore_[kCandCount];   // every candidate's score: the plateau needs the whole profile
    bool active_ = false;            // a search is running
    bool wasNarrow_ = false;         // this search looked around a rate
    bool background_ = false;        // ... and this one is the background's
    float bgBpm_ = 0.0f;             // the background's nominated rate
    float bgPhase_ = 0.0f;           // ... and grid
    float bgScore_ = 0.0f;           // ... and evidence
    float bgLocalScore_ = 0.0f;      // the steady rate's evidence, same window
    float bgQual_ = 0.0f;            // the background's own quality
    uint8_t bgAgree_ = 0;            // how many searches in a row agreed
    bool bgHave_ = false;            // it has said something
    bool bgConverged_ = false;       // ... and it has agreed with itself
    float frameRate_ = 0.0f;         // frames per second, from the opened rate
    int combFrames_ = 0;             // the onset window, in frames
};

}  // namespace mm
