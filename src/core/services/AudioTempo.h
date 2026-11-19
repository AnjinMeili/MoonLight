#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "core/services/AudioTempoFrontEnd.h"
#include "core/services/AudioTempoSweep.h"
#include "platform/platform.h"  // InternalRamObject, so the state lives in internal RAM

namespace mm {

/// Troy's tempo tracker: the beat rate and the beat grid, from the music's onsets, one block at a time.
/// @attribution troyhacks
///
/// @technical
///
/// ## The three parts
///
/// The front end filters the block and picks the onsets; the sweep searches the onset window for the rate; this state runs the grid, the gate and the requests, and publishes the numbers. The front end and the sweep are members, so the whole tracker is one allocation, held only while it is demanded.
///
/// ## The grid
///
/// The grid is a clock and the comb's answer is a measurement: the clock's rate follows the measurement slowly, with an 8 s time constant, and its phase follows the comb's anchor on the grid's own period, 35 % of the way each search. The first lock places the grid at once; a confirmed far rate jumps it.
///
/// A rate a long way off is a different grid, taken only when the rate the grid is at has died and the new one has evidence of its own, or when the far answer has held its course for 116 searches without reversing.
///
/// ## The gate and the hold
///
/// A gate shut for 1 s means the music stopped or changed: the sweep freezes, the evidence is kept, and the gap flag reads true, so a breakdown does not stutter the beat and an effect can tell a gap from a lock.
///
/// The evidence is thrown away after 10 s of silence, or after 1 s when nothing has locked yet, when the evidence is pre-lock noise and not the track's. The tempo goes invalid after 16 s, the grid stopping being published then, though it keeps running inside.
///
/// ## The rebase
///
/// The frame counter is rebased every 65536 frames, so the float arithmetic on the grid keeps its precision: 11:39 at 48 kHz, 23:18 at 24 kHz. The rebase is deferred while a search is running, because the search's snapshot is in the same frame numbers.
///
/// ## What was left behind
///
/// The measured downbeat is gone: its t-score sat on its null in 43 % of sixty windows of real music, chance, so the numbering is the convention or the user's mark.
///
/// The ProLink reference, the serial console, the user's lock and sync, and the drift publication went with it: WLED test instruments with no MoonLight reader. The prior's table is gone too: it was written and never read, the formula is what the sweep uses.
///
/// ## The rate it runs at
///
/// The tracker derives its constants from the opened rate, so the seconds stay the same: the 12 s window, the 1 s gate, the 16 s hold, the search intervals. The picker's frame counts stay as they are, so its latency is 3 frames: 64 ms at 24 kHz, 32 ms at 48 kHz.
///
/// ## The level
///
/// The grid stays in the 90-175 band, and the true rate may be its half or its double: 70 BPM music locks a 140 grid. The sweep reports the level, and the published beat clock runs at the grid's period divided by the level, anchored to the grid's beats, so the published BPM, the beat and the phase all read the true rate.
///
/// A level change applies only after six outcomes in a row agree on it: a single noisy search cannot halve the published beat. The first lock takes its level at once: nothing is published yet to flicker.
///
/// A half-level published beat is an even grid beat; a double-level one is every grid beat and every midpoint.
struct AudioTempoState : platform::InternalRamObject {
    /// The frame counter's rebase interval: the grid's float arithmetic keeps its precision: @xref{the-rebase}
    static constexpr uint32_t kRebaseFrames = 65536;

    /// Bake the coefficients for the opened rate and clear the state: @xref{the-rate-it-runs-at}
    void reset(uint32_t rate);

    /// Whether the opened rate supports the band: false at 8 kHz and below.
    bool runnable() const { return runnable_; }

    /// One block of left-justified 24-bit samples: the front end, the grid, the search, the rebase: @xref{the-three-parts}
    void process(const int32_t* samples, size_t n, bool gateOpen);

    /// The beat rate, BPM: the grid's rate times the level, 0 when there is no usable estimate: @xref{the-grid} @xref{the-level}
    float bpm() const { return valid() ? bpm_ * level_ : 0.0f; }

    /// The progress through the current beat, 0..1: 0 when there is no usable estimate.
    float beatPhase() const;

    /// Whether a beat fired in the last block.
    bool beatFired() const { return valid() && beatFired_; }

    /// Whether the beat that fired in the last block was the bar's first.
    bool downbeatFired() const { return valid() && downbeatFired_; }

    /// The beat in the bar, 1..4: 0 when there is no usable estimate.
    uint8_t beatInBar() const;

    /// The beats counted since the reference point: the lock, or the beat the numbering was shifted to.
    uint32_t beatCount() const;

    /// The comb's score at the last search, 0..1: the measurement, published whether it is believed or not.
    float score() const { return score_; }

    /// The winner's corrected score above the best alternative at the last whole-band search, 0..1: how much the fold prefers this rate. A narrow search leaves it alone: its margin is the local peak's, not the rate's.
    float margin() const { return margin_; }

    /// The tracker's band level this block, -60..0 dBFS mapped to 0..255: the instantaneous loudness of the 90-320 Hz band the onsets come from.
    uint8_t energy() const;

    /// How well the music explains the running grid, 0..1: the alignment times the coverage.
    float quality() const { return quality_; }

    /// Whether the grid is running.
    bool locked() const { return locked_; }

    /// Whether the estimate is usable: the grid running, and the music gone less than 16 s: @xref{the-gate-and-the-hold}
    bool valid() const { return locked_ && gateShutCount_ < holdFrames_; }

    /// Whether the music has stopped but the grid still runs: the gate shut 1 s or more, the estimate still usable: @xref{the-gate-and-the-hold}
    bool gap() const { return locked_ && gateShutCount_ >= gateShutFrames_ && gateShutCount_ < holdFrames_; }

    /// Ask for a re-lock: the grid is thrown away at the top of the next block and the next search locks afresh.
    void requestRelock() { relockRequested_.store(true, std::memory_order_relaxed); }

    /// Ask to mark the beat sounding now as beat one, or to clear the mark and return to the convention: @xref{what-was-left-behind}
    void requestDownbeat(bool set) { downbeatRequest_.store(set ? 1 : 2, std::memory_order_relaxed); }

    /// The frame counter, for the rebase test.
    uint32_t frameIndexForTest() const { return frameIndex_; }

    /// Move the frame counter and every clock on it to a frame, for the rebase test.
    void setFrameIndexForTest(uint32_t frame);

    /// The onsets the front end holds, for the gap test: the evidence the 10 s silence throws away.
    uint16_t onsetCountForTest() const { return frontEnd_.peakCount(); }

private:
    static constexpr float kMinScore = 0.35f;     // the least evidence a rate needs to be believed
    static constexpr float kDeadAccept = 0.50f;   // a far rate needs this much evidence to be taken
    static constexpr float kNearBpm = 1.50f;      // a rate this close is the same grid
    static constexpr float kConfirmMove = 0.10f;  // the run's stillness: a move inside this is the same answer
    static constexpr uint8_t kConfirmN = 116;    // the run's length before a far rate may move the clock
    static constexpr uint8_t kLevelConfirmN = 6;  // the agreeing outcomes before a level change applies: the background's converge count
    static constexpr float kQualMin = 0.20f;      // the quality floor below which the grid holds
    static constexpr float kQualRel = 0.50f;      // ... and the relative floor, against the quality's recent best
    static constexpr float kPllGamma = 0.35f;     // the phase's step toward the comb's anchor
    static constexpr float kRateTauS = 8.0f;      // the rate follower's time constant, seconds
    static constexpr float kGateShutS = 1.0f;     // the gate shut this long means the music changed
    static constexpr float kEvidenceHoldS = 10.0f;  // ... and this long of silence throws the evidence away
    static constexpr float kHoldS = 16.0f;        // ... and this long means the track ended
    static constexpr float kRefFrameRate = 93.75f;  // the frame rate the search intervals are measured in
    static constexpr int kSearchEveryRef = 4;     // the steady search's interval, at the reference rate
    static constexpr int kBgEveryRef = 32;        // the background search's interval, at the reference rate
    static constexpr uint32_t kDownbeatNone = 0xFFFFFFFFu;  // no downbeat mark applies

    /// The re-lock and downbeat requests, applied here because the grid belongs to the analysis task.
    void applyRequests();

    /// The gate's bookkeeping: a shut gate counts up, and at 1 s the evidence goes.
    void applyGate(bool gateOpen);

    /// The grid's advance: fire every beat the frame counter has reached.
    void advanceGrid();

    /// Start a search when one is due: the background's on its own clock, the steady one every few frames, the acquisition's as soon as the last finished.
    void scheduleSearch();

    /// Apply a finished search's outcome to the grid.
    void applyOutcome(const AudioTempoSweep::Outcome& out);

    /// A foreground search's outcome: the steady state, or the first lock.
    void applyForeground(const AudioTempoSweep::Outcome& out);

    /// A level the outcomes agree on: the pending level counts agreeing outcomes, and applies on the sixth, or at once before the first lock.
    void confirmLevel(float level);

    /// A far rate's outcome: taken when the grid's rate has died, or when the run has held its course.
    bool applyFarRate(float rate, float phase, float score, float local, uint8_t run);

    /// A background search's outcome: the grid moves, or its phase is taken.
    void applyBackground(const AudioTempoSweep::Outcome& out);

    /// Rebase the frame counter and every clock on it, unless a search is running.
    void maybeRebase();

    /// The first lock: place the grid at the comb's phase, immediately.
    void firstLock(float bpm, float phase, float now);

    /// A confirmed far rate: jump the grid's rate and phase at once.
    void hardAdopt(float bpm, float phase, float now);

    /// The rate follower: the clock's rate moves toward the measurement, slowly.
    void slewRate(float bpm, uint32_t now);

    /// The phase follower: the beat moves toward the comb's anchor, the rate untouched.
    void adoptPhase(float phase);

    /// The confirmation run: how long the answer has kept going one way, or kept still.
    uint8_t confirm(float rate);

    /// Snap the published beat clock to the grid's phase and period, so a slew, a phase adoption or a level change does not strand it.
    void reanchorPublished();

    /// Throw the evidence away and keep the grid.
    void hold();

    AudioTempoFrontEnd frontEnd_;  // the filters, the onset function, the picker
    AudioTempoSweep sweep_;        // the search, and the background's second estimator
    float period_ = 0.0f;          // the beat period, in frames
    float nextBeat_ = 0.0f;        // the frame of the next beat
    uint32_t beatIndex_ = 0;       // the monotonic grid-beat counter
    float level_ = 1.0f;           // the true rate's level against the grid: 1, or the half or double that won the fold
    float levelPending_ = 1.0f;    // the level the outcomes have been agreeing on
    uint8_t levelAgree_ = 0;       // how many in a row
    float pubNext_ = 0.0f;         // the frame of the next published beat
    float pubLast_ = 0.0f;         // ... and of the last one
    uint32_t pubIndex_ = 0;        // the monotonic published-beat counter
    uint32_t frameIndex_ = 0;      // the frame counter, rebased
    float bpm_ = 0.0f;             // the grid's rate
    float score_ = 0.0f;           // the comb score, 0..1
    float margin_ = 0.0f;          // the winner's corrected score above the best alternative, 0..1
    uint32_t lastSlewFrame_ = 0;   // when the clock's rate last followed the measurement
    float quality_ = 0.0f;         // the alignment times the coverage, 0..1
    float qualityPeak_ = 0.0f;     // the quality's recent best, decaying
    float localScore_ = 0.0f;      // the comb's score at the grid's own rate
    float localPhase_ = 0.0f;      // ... and phase
    bool locked_ = false;          // the grid is running
    float confBase_ = 0.0f;        // the answer where the run started
    float confPrev_ = 0.0f;        // the answer before it
    uint8_t confCount_ = 0;        // how long the run has lasted
    int8_t confDir_ = 0;           // which way it has been going: -1, 0, +1
    uint8_t downbeatPos_ = 0;      // the marked downbeat, 0..3
    bool downbeatKnown_ = false;   // the numbering is on a mark, not the convention
    bool downbeatManual_ = false;  // ... and the user set it
    uint32_t downbeatApplyAt_ = 0;  // the beat the numbering shifts at
    uint16_t gateShutCount_ = 0;   // how long the gate has been shut, frames
    uint32_t lastSweepFrame_ = 0;  // when the last steady search started
    uint32_t bgLastFrame_ = 0;     // when the background last looked
    uint8_t collapseCount_ = 0;    // searches below the floor, in a row
    bool beatFired_ = false;       // a beat fired in the last block
    bool downbeatFired_ = false;   // ... and it was the bar's first
    std::atomic<bool> relockRequested_{false};  // a re-lock is a request: the grid belongs to the analysis task
    std::atomic<uint8_t> downbeatRequest_{0};   // 0 = nothing, 1 = mark the downbeat, 2 = clear it
    float frameRate_ = 0.0f;       // frames per second, from the opened rate
    int gateShutFrames_ = 0;       // the gate's 1 s, in frames
    int evidenceHoldFrames_ = 0;   // the evidence's 10 s, in frames
    int holdFrames_ = 0;           // the grid's 16 s, in frames
    int searchEvery_ = 0;          // the steady search's interval, in frames
    int bgEvery_ = 0;              // the background search's interval, in frames
    float qualDecay_ = 1.0f;       // the quality peak's per-search decay
    bool runnable_ = false;        // the opened rate supports the band
};

}  // namespace mm
