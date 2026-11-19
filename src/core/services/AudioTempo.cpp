/// Troy's tempo tracker; the public surface and the class layout live in AudioTempo.h.
#include "core/services/AudioTempo.h"

#include <cmath>

namespace mm {

void AudioTempoState::reset(uint32_t rate) {
    frontEnd_.reset(rate);
    frameRate_ = static_cast<float>(rate) / 512.0f;
    sweep_.reset(frameRate_);
    gateShutFrames_ = static_cast<int>(lroundf(kGateShutS * frameRate_));
    evidenceHoldFrames_ = static_cast<int>(lroundf(kEvidenceHoldS * frameRate_));
    holdFrames_ = static_cast<int>(lroundf(kHoldS * frameRate_));
    searchEvery_ = static_cast<int>(lroundf(static_cast<float>(kSearchEveryRef) * frameRate_ / kRefFrameRate));
    if (searchEvery_ < 1) searchEvery_ = 1;
    bgEvery_ = static_cast<int>(lroundf(static_cast<float>(kBgEveryRef) * frameRate_ / kRefFrameRate));
    if (bgEvery_ < 1) bgEvery_ = 1;
    qualDecay_ = powf(0.5f, static_cast<float>(searchEvery_) / (8.0f * frameRate_));
    runnable_ = frontEnd_.runnable();
    period_ = 0.0f;
    nextBeat_ = 0.0f;
    beatIndex_ = 0;
    level_ = 1.0f;
    levelPending_ = 1.0f;
    levelAgree_ = 0;
    pubNext_ = 0.0f;
    pubLast_ = 0.0f;
    pubIndex_ = 0;
    frameIndex_ = 0;
    bpm_ = 0.0f;
    score_ = 0.0f;
    margin_ = 0.0f;
    lastSlewFrame_ = 0;
    quality_ = 0.0f;
    qualityPeak_ = 0.0f;
    localScore_ = 0.0f;
    localPhase_ = 0.0f;
    locked_ = false;
    confBase_ = 0.0f;
    confPrev_ = 0.0f;
    confCount_ = 0;
    confDir_ = 0;
    downbeatPos_ = 0;
    downbeatKnown_ = false;
    downbeatManual_ = false;
    downbeatApplyAt_ = 0;
    gateShutCount_ = 0;
    lastSweepFrame_ = 0;
    bgLastFrame_ = 0;
    collapseCount_ = 0;
    beatFired_ = false;
    downbeatFired_ = false;
    relockRequested_.store(false, std::memory_order_relaxed);
    downbeatRequest_.store(0, std::memory_order_relaxed);
}

void AudioTempoState::process(const int32_t* samples, size_t n, bool gateOpen) {
    if (!runnable_) return;
    beatFired_ = false;
    downbeatFired_ = false;
    applyRequests();
    applyGate(gateOpen);
    frameIndex_++;
    frontEnd_.process(samples, n, frameIndex_);
    advanceGrid();
    if (gateOpen) {  // no search while the gate is shut; the front end and the grid still run
        scheduleSearch();
        applyOutcome(sweep_.step(bpm_));
    }
    maybeRebase();
}

float AudioTempoState::beatPhase() const {
    if (!valid() || period_ <= 0.0f) return 0.0f;
    const float p = (static_cast<float>(frameIndex_) - pubLast_) / (period_ / level_);
    if (p < 0.0f) return 0.0f;
    if (p > 1.0f) return 1.0f;
    return p;
}

uint8_t AudioTempoState::beatInBar() const {
    if (!valid()) return 0;
    const bool applied = downbeatKnown_ && pubIndex_ >= downbeatApplyAt_;
    const uint32_t base = applied ? (pubIndex_ - downbeatPos_) : pubIndex_;
    return static_cast<uint8_t>((base % 4) + 1);
}

uint32_t AudioTempoState::beatCount() const {
    if (!valid()) return 0;
    const bool applied = downbeatKnown_ && pubIndex_ >= downbeatApplyAt_;
    return applied ? (pubIndex_ - downbeatApplyAt_) : pubIndex_;
}

uint8_t AudioTempoState::energy() const {
    float e = (frontEnd_.bandDb() + 60.0f) * (255.0f / 60.0f);  // -60..0 dBFS onto 0..255: silence reads 0, a full-scale band reads 255
    if (e < 0.0f) e = 0.0f;
    if (e > 255.0f) e = 255.0f;
    return static_cast<uint8_t>(e + 0.5f);
}

void AudioTempoState::setFrameIndexForTest(uint32_t frame) {
    const int32_t delta = static_cast<int32_t>(frame - frameIndex_);
    frameIndex_ = frame;
    nextBeat_ += static_cast<float>(delta);
    pubNext_ += static_cast<float>(delta);
    pubLast_ += static_cast<float>(delta);
    lastSweepFrame_ += static_cast<uint32_t>(delta);
    lastSlewFrame_ += static_cast<uint32_t>(delta);
    bgLastFrame_ += static_cast<uint32_t>(delta);
    frontEnd_.rebase(delta);
}

void AudioTempoState::applyRequests() {
    if (relockRequested_.exchange(false, std::memory_order_relaxed)) {
        locked_ = false;
        beatIndex_ = 0;
        nextBeat_ = 0.0f;
        level_ = 1.0f;
        levelPending_ = 1.0f;
        levelAgree_ = 0;
        pubNext_ = 0.0f;
        pubLast_ = 0.0f;
        pubIndex_ = 0;
        period_ = 0.0f;
        score_ = 0.0f;
        margin_ = 0.0f;
        hold();
        downbeatManual_ = false;  // a mark belongs to the grid that was just thrown away
        downbeatKnown_ = false;
        downbeatApplyAt_ = kDownbeatNone;
    }
    const uint8_t dbr = downbeatRequest_.exchange(0, std::memory_order_relaxed);
    if (dbr == 1) {
        if (locked_) {  // the beat sounding now is beat one, and the mark takes effect at once
            downbeatPos_ = static_cast<uint8_t>(pubIndex_ % 4u);
            downbeatApplyAt_ = pubIndex_;
            downbeatManual_ = true;
            downbeatKnown_ = true;
        }
    } else if (dbr == 2) {
        downbeatManual_ = false;  // back to the convention: the first beat after the lock is beat one
        downbeatKnown_ = false;
        downbeatApplyAt_ = kDownbeatNone;
    }
}

void AudioTempoState::applyGate(bool gateOpen) {
    if (!gateOpen) {
        if (gateShutCount_ < 60000) gateShutCount_++;  // the counter saturates, so it cannot wrap into a short value
        const int clearAt = locked_ ? evidenceHoldFrames_ : gateShutFrames_;  // unlocked, the evidence is pre-lock noise: clear it at 1 s; locked, it is the track's: keep it to 10 s
        if (gateShutCount_ == clearAt) hold();  // once per episode
    } else {
        gateShutCount_ = 0;
    }
}

void AudioTempoState::advanceGrid() {
    if (!locked_) return;
    while (static_cast<float>(frameIndex_) >= nextBeat_) {
        nextBeat_ += period_;
        beatIndex_++;
    }
    const float pubPeriod = period_ / level_;  // the published beat's spacing: the grid's, divided by the level
    while (static_cast<float>(frameIndex_) >= pubNext_) {
        pubLast_ = pubNext_;
        pubNext_ += pubPeriod;
        pubIndex_++;
        beatFired_ = true;
        if (beatInBar() == 1) downbeatFired_ = true;
    }
}

void AudioTempoState::reanchorPublished() {
    if (!locked_ || period_ <= 0.0f) return;
    const float pubPeriod = period_ / level_;
    float anchor = nextBeat_;  // the next grid beat
    if (level_ < 1.0f && (beatIndex_ & 1) != 0) anchor += period_;  // a half-level published beat is an even grid beat; past an odd one, the next is one period on
    pubNext_ = anchor + roundf((pubNext_ - anchor) / pubPeriod) * pubPeriod;  // the nearest published slot to where the clock was
    pubLast_ = pubNext_ - pubPeriod;
}

void AudioTempoState::scheduleSearch() {
    if (sweep_.active()) return;
    if (locked_ && static_cast<int32_t>(frameIndex_ - bgLastFrame_) >= bgEvery_) {
        bgLastFrame_ = frameIndex_;
        sweep_.start(true, frontEnd_.peakFrames(), frontEnd_.peakStrengths(), frontEnd_.peakHead(), frontEnd_.peakCount(), frameIndex_, 0.0f);
        return;
    }
    const bool due = !locked_ || static_cast<int32_t>(frameIndex_ - lastSweepFrame_) >= searchEvery_;
    if (!due) return;
    lastSweepFrame_ = frameIndex_;
    sweep_.start(false, frontEnd_.peakFrames(), frontEnd_.peakStrengths(), frontEnd_.peakHead(), frontEnd_.peakCount(), frameIndex_, locked_ ? bpm_ : 0.0f);
}

void AudioTempoState::applyOutcome(const AudioTempoSweep::Outcome& out) {
    if (out.kind == AudioTempoSweep::Outcome::kForeground) applyForeground(out);
    else if (out.kind == AudioTempoSweep::Outcome::kBackground) applyBackground(out);
}

void AudioTempoState::applyForeground(const AudioTempoSweep::Outcome& out) {
    score_ = out.score;  // published whether or not it is believed: a score that has fallen says the estimate is no longer trustworthy
    if (out.wide) margin_ = out.margin;  // the whole band's discrimination only: a narrow search's margin is the local peak's, not the rate's
    if (out.score < kMinScore) {  // the music it was tracking is not there: the grid holds
        if (collapseCount_ < 255) collapseCount_++;
        return;
    }
    collapseCount_ = 0;
    confirmLevel(out.level);
    if (!locked_) {
        firstLock(out.rate, out.phase, static_cast<float>(sweep_.startedAt()));
        return;
    }
    float ph = 0.0f;  // the grid's own measurement: the comb at the rate the grid is running
    const float local = sweep_.correctedAt(bpm_, &ph);
    localScore_ = local;
    localPhase_ = ph;
    quality_ = local * sweep_.coverage(bpm_, ph);
    qualityPeak_ = fmaxf(quality_, qualityPeak_ * qualDecay_);
    const uint8_t run = confirm(out.rate);  // the run is watched whether the answer is near the clock or far
    const bool near = (fabsf(out.rate - bpm_) <= kNearBpm);
    if (!near) {
        applyFarRate(out.rate, out.phase, out.score, local, run);
        return;
    }
    if (quality_ >= kQualMin) slewRate(out.rate, frameIndex_);  // the clock follows the measurement, slowly
    if (quality_ >= kQualMin && quality_ >= kQualRel * qualityPeak_) adoptPhase(ph);  // ... and its phase follows the comb's anchor
}

void AudioTempoState::confirmLevel(float level) {
    if (level == level_) {  // the outcomes agree with the published level again: the pending change is forgotten
        levelAgree_ = 0;
        return;
    }
    if (level == levelPending_) {  // the fold moved the level: it applies only after agreeing outcomes, so one noisy search cannot halve the published beat
        if (levelAgree_ < 255) levelAgree_++;
    } else {
        levelPending_ = level;
        levelAgree_ = 1;
    }
    if (!locked_ || levelAgree_ >= kLevelConfirmN) {  // the first lock takes its level at once: nothing published yet to flicker
        level_ = level;
        levelAgree_ = 0;
        reanchorPublished();
    }
}

bool AudioTempoState::applyFarRate(float rate, float phase, float score, float local, uint8_t run) {
    if (local < kMinScore && score >= kDeadAccept) {  // the music changed: the grid's rate died and the new one has evidence
        hardAdopt(rate, phase, static_cast<float>(sweep_.startedAt()));
        return true;
    }
    if (run >= kConfirmN && score >= kDeadAccept) {  // the far answer held its course
        hardAdopt(rate, phase, static_cast<float>(sweep_.startedAt()));
        return true;
    }
    return false;  // otherwise the grid holds its rate and its phase
}

void AudioTempoState::applyBackground(const AudioTempoSweep::Outcome& out) {
    margin_ = out.margin;  // the background always looks at the whole band, so its margin is the rate's
    if (out.promote) {
        hardAdopt(out.rate, out.phase, static_cast<float>(sweep_.startedAt()));
        return;
    }
    if (out.nudgePhase) adoptPhase(out.phase);
}

void AudioTempoState::maybeRebase() {
    if (frameIndex_ < kRebaseFrames || sweep_.active()) return;  // deferred while a search runs: the snapshot is in the same frame numbers
    frameIndex_ -= kRebaseFrames;
    nextBeat_ -= static_cast<float>(kRebaseFrames);
    pubNext_ -= static_cast<float>(kRebaseFrames);
    pubLast_ -= static_cast<float>(kRebaseFrames);
    lastSweepFrame_ = (lastSweepFrame_ >= kRebaseFrames) ? lastSweepFrame_ - kRebaseFrames : 0;
    lastSlewFrame_ = (lastSlewFrame_ >= kRebaseFrames) ? lastSlewFrame_ - kRebaseFrames : 0;
    bgLastFrame_ = (bgLastFrame_ >= kRebaseFrames) ? bgLastFrame_ - kRebaseFrames : 0;
    frontEnd_.rebase(-static_cast<int32_t>(kRebaseFrames));
}

void AudioTempoState::firstLock(float bpm, float phase, float now) {
    const float p = 60.0f * frameRate_ / bpm;
    period_ = p;
    nextBeat_ = phase + roundf((now - phase) / p) * p;
    locked_ = true;
    beatIndex_ = 0;  // the bar numbering restarts with the lock
    pubIndex_ = 0;
    pubNext_ = nextBeat_;  // the published clock starts on the grid's anchor: beat 0 is published at every level
    pubLast_ = pubNext_ - p / level_;
    downbeatKnown_ = false;
    downbeatManual_ = false;
    downbeatApplyAt_ = 0;
    bpm_ = bpm;
    lastSlewFrame_ = static_cast<uint32_t>(now);  // the follower starts here: the grid is new
}

void AudioTempoState::hardAdopt(float bpm, float phase, float now) {
    const float p = 60.0f * frameRate_ / bpm;
    period_ = p;
    const float k = roundf((nextBeat_ - phase) / period_);
    const float anchor = phase + k * period_;
    nextBeat_ = anchor;
    bpm_ = 60.0f * frameRate_ / period_;
    lastSlewFrame_ = static_cast<uint32_t>(now);  // the grid is new, and the follower with it
    reanchorPublished();
}

void AudioTempoState::slewRate(float bpm, uint32_t now) {
    const float p = 60.0f * frameRate_ / bpm;
    if (p <= 0.0f) return;
    float dt = static_cast<float>(static_cast<int32_t>(now - lastSlewFrame_)) / frameRate_;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.5f) dt = 0.5f;  // a long gap does not snap the clock
    float alpha = dt / kRateTauS;
    if (alpha > 1.0f) alpha = 1.0f;
    period_ += alpha * (p - period_);
    bpm_ = 60.0f * frameRate_ / period_;
    lastSlewFrame_ = now;
    reanchorPublished();
}

void AudioTempoState::adoptPhase(float phase) {
    const float k = roundf((nextBeat_ - phase) / period_);
    const float anchor = phase + k * period_;
    nextBeat_ += kPllGamma * (anchor - nextBeat_);
    reanchorPublished();
}

uint8_t AudioTempoState::confirm(float rate) {
    if (confCount_ == 0) {
        confBase_ = rate;
        confPrev_ = rate;
        confCount_ = 1;
        confDir_ = 0;
        return 1;
    }
    const float move = rate - confBase_;  // the run is measured against where it started, not the last answer
    if (fabsf(move) <= kConfirmMove) {  // it has not really moved: the same answer again
        if (confCount_ < 255) confCount_++;
        confPrev_ = rate;
        return confCount_;
    }
    const int8_t dir = (move > 0.0f) ? 1 : -1;
    if (confDir_ != 0 && dir != confDir_) {  // it came back: confusion, not a tempo
        confBase_ = rate;
        confCount_ = 1;
        confDir_ = 0;
    } else {
        confDir_ = dir;
        if (confCount_ < 255) confCount_++;
    }
    confPrev_ = rate;
    return confCount_;
}

void AudioTempoState::hold() {
    frontEnd_.hold();
    sweep_.hold();
    collapseCount_ = 0;
}

}  // namespace mm
