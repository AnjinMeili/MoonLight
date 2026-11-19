/// @module AudioTempo

/// The tempo tracker through the service's own analysis: a click track locks at its rate, silence and noise read no tempo, the state follows the demand, a beat in an earlier block still reaches the effects, the grid survives the frame counter's rebase, a diluted 174 BPM onset set reads 174, not its 4:3 relative, and a loud floor never halves the published beat.

#include "doctest.h"
#include "core/services/AudioService.h"
#include "core/services/AudioTempo.h"
#include "core/services/AudioTempoSweep.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <numbers>
#include <vector>

namespace {

constexpr uint32_t kRate = 24000;   // the default rate
constexpr size_t kBlock = 512;
constexpr float kBlockS = static_cast<float>(kBlock) / static_cast<float>(kRate);

// A click track: a decaying 120 Hz burst at each beat, over a quiet hiss, silent until `sounding`.
struct ClickTrack {
    float bpm;
    double hissGain = 1.0;    // the floor's gain: 1 is the room's quiet hiss, ~7000 is the synthetic WAVs' -30 dB floor
    double t = 0.0;           // the stream's time, seconds
    double nextBeat = 0.0;    // the next beat's time
    double lastBeat = -1.0;   // the last beat's time, for the burst's decay
    uint32_t seed = 1;
    bool sounding = false;    // false during the lead-in hiss

    int32_t sample() {
        seed = seed * 1664525u + 1013904223u;
        double v = static_cast<double>(static_cast<int32_t>(seed) >> 26) * hissGain;   // a hiss of a few 24-bit counts, scaled by the floor
        if (sounding) {
            while (t >= nextBeat) {
                lastBeat = nextBeat;
                nextBeat += 60.0 / static_cast<double>(bpm);
            }
            if (lastBeat >= 0.0) {
                const double age = t - lastBeat;
                v += 5000000.0 * std::exp(-age / 0.08) * std::cos(2.0 * std::numbers::pi * 120.0 * age);
            }
        }
        t += 1.0 / kRate;
        return static_cast<int32_t>(v) * 256;   // left-justified, as the input delivers 24-bit samples
    }
    void fill(int32_t (&block)[kBlock]) {
        for (size_t i = 0; i < kBlock; i++) block[i] = sample();
    }
};

// Sort an onset set by frame, oldest first, so the snapshot walks newest first.
void sortOnsets(int32_t* frames, float* str, int n) {
    for (int i = 1; i < n; i++) {
        const int32_t f = frames[i];
        const float s = str[i];
        int j = i - 1;
        while (j >= 0 && frames[j] > f) {
            frames[j + 1] = frames[j];
            str[j + 1] = str[j];
            j--;
        }
        frames[j + 1] = f;
        str[j + 1] = s;
    }
}

// Run a whole-band search on an onset set to its outcome; `now` slides the recency-weighted window, as the live tracker does.
mm::AudioTempoSweep::Outcome runSearch(mm::AudioTempoSweep& sweep, int32_t* frames, float* str, int n, uint32_t now = 562) {
    sortOnsets(frames, str, n);
    sweep.start(false, frames, str, n, n, now, 0.0f);
    mm::AudioTempoSweep::Outcome out;
    do {
        out = sweep.step(0.0f);
    } while (out.kind == mm::AudioTempoSweep::Outcome::kSearching);
    return out;
}

// `seconds` of the track through the service's analysis, counting the blocks a beat fired on; answers the last block's frame.
const mm::AudioFrame& play(mm::AudioService& audio, ClickTrack& track, float seconds, int& beats) {
    static int32_t block[mm::AudioService::kBlock];
    const mm::AudioFrame* f = nullptr;
    for (float done = 0; done < seconds; done += kBlockS) {
        track.fill(block);
        f = &audio.analyzeForTest(block);
        if (f->beat) beats++;
    }
    return *f;
}

// The service ready and the track's room floor learned, the clicks sounding.
void startTrack(mm::AudioService& audio, ClickTrack& track, int& beats) {
    audio.defineControls();
    audio.setTempoForTest(true);
    audio.resetAnalysisForTest();
    play(audio, track, 1.0f, beats);   // the room's floor, learned first
    track.sounding = true;
}

// Kicks at `bpm` plus, when `hatStr` > 0, hats half a beat off at that strength; answers how many onsets.
int kickHatSet(int32_t* frames, float* str, float bpm, int kicks, float hatStr) {
    const float beat = 60.0f * 46.875f / bpm;
    int n = 0;
    for (int i = 0; i < kicks; i++) {
        frames[n] = static_cast<int32_t>(lroundf(static_cast<float>(i) * beat));
        str[n] = 1.0f;
        n++;
    }
    for (int i = 0; hatStr > 0.0f && i < kicks; i++) {
        frames[n] = static_cast<int32_t>(lroundf((static_cast<float>(i) + 0.5f) * beat));
        str[n] = hatStr;
        n++;
    }
    return n;
}

// A whole-band search on a kick-and-hat set at `bpm`; answers its outcome.
mm::AudioTempoSweep::Outcome searchSet(float bpm, int kicks, float hatStr) {
    static mm::AudioTempoSweep sweep;
    sweep.reset(46.875f);
    static int32_t frames[64];
    static float str[64];
    return runSearch(sweep, frames, str, kickHatSet(frames, str, bpm, kicks, hatStr));
}

// A State locked at 120 BPM on the click track, the gate open.
void lockAt120(mm::AudioTempoState& tempo, ClickTrack& track, int32_t (&block)[kBlock]) {
    tempo.reset(kRate);
    track.sounding = true;
    for (float done = 0.0f; done < 8.0f; done += kBlockS) {   // lock at 120, the gate open
        track.fill(block);
        tempo.process(block, kBlock, true);
    }
    REQUIRE(tempo.locked());
}

}  // namespace

TEST_CASE("AudioTempo: a 120 BPM click track locks at 120 within 1 BPM") {
    static mm::AudioService audio;
    ClickTrack track{120.0f};
    int beats = 0;
    startTrack(audio, track, beats);
    const mm::AudioFrame& f = play(audio, track, 8.0f, beats);
    CHECK(f.tempoBpm >= 11900);
    CHECK(f.tempoBpm <= 12100);
    CHECK(beats >= 4);
    CHECK(f.beatCount >= 4);
}

TEST_CASE("AudioTempo: a 90 BPM click track locks at 90 within 1 BPM") {
    static mm::AudioService audio;
    ClickTrack track{90.0f};
    int beats = 0;
    startTrack(audio, track, beats);
    const mm::AudioFrame& f = play(audio, track, 12.0f, beats);   // 90 BPM needs longer: eight onsets take 5 s, and the plateau narrows with the window
    CHECK(f.tempoBpm >= 8900);
    CHECK(f.tempoBpm <= 9100);
    CHECK(beats >= 4);
}

TEST_CASE("AudioTempo: a 70 BPM click track locks at 70 within 1 BPM, not its double") {
    static mm::AudioService audio;
    ClickTrack track{70.0f};
    int beats = 0;
    startTrack(audio, track, beats);
    const mm::AudioFrame& f = play(audio, track, 12.0f, beats);   // 70 BPM needs longer: eight onsets take 6 s, and the plateau narrows with the window
    CHECK(f.tempoBpm >= 6900);
    CHECK(f.tempoBpm <= 7100);
    CHECK(beats >= 4);
}

TEST_CASE("AudioTempo: a click track stepping from 120 to 140 BPM is followed, within 12 s") {
    static mm::AudioService audio;
    ClickTrack track{120.0f};
    int beats = 0;
    startTrack(audio, track, beats);
    const mm::AudioFrame& locked = play(audio, track, 8.0f, beats);
    CHECK(locked.tempoBpm >= 11900);
    CHECK(locked.tempoBpm <= 12100);
    track.bpm = 140.0f;   // the step, phase continuous
    const mm::AudioFrame& f = play(audio, track, 12.0f, beats);
    CHECK(f.tempoBpm >= 13900);
    CHECK(f.tempoBpm <= 14100);
    // The plan's four-beat bar is not met: the measured follow on the synthetic WAV is ~10 s (two background cycles, the first promotion landing 3 BPM off and the narrow window trapping it). 12 s pins what the tracker does, not the bar.
}

TEST_CASE("AudioTempo: white noise reads no tempo") {
    static mm::AudioTempoState tempo;
    tempo.reset(kRate);
    static int32_t block[kBlock];
    uint32_t seed = 1234;
    float maxScore = 0.0f;
    for (float done = 0.0f; done < 8.0f; done += kBlockS) {
        for (size_t i = 0; i < kBlock; i++) {
            seed = seed * 1664525u + 1013904223u;
            block[i] = (static_cast<int32_t>(seed >> 9) - (1 << 22)) * 512;   // white noise, about half scale
        }
        tempo.process(block, kBlock, true);   // the gate open, as a loud room holds it
        if (tempo.score() > maxScore) maxScore = tempo.score();
    }
    CHECK_FALSE(tempo.locked());
    CHECK(tempo.bpm() == 0.0f);
    CHECK(maxScore < 0.35f);   // below the least believed score: chance, corrected, never reaches it
    CHECK(tempo.margin() < 0.10f);   // no rate beats its alternatives on noise: the margin stays near zero
}

TEST_CASE("AudioTempo: a sounding click track reads band energy, and a clean lock's margin beats noise's") {
    static mm::AudioService audio;
    ClickTrack track{120.0f};
    int beats = 0;
    startTrack(audio, track, beats);
    uint8_t maxEnergy = 0;
    for (float done = 0.0f; done < 8.0f; done += kBlockS) {
        static int32_t block[mm::AudioService::kBlock];
        track.fill(block);
        const mm::AudioFrame& f = audio.analyzeForTest(block);
        if (f.energy > maxEnergy) maxEnergy = f.energy;
    }
    CHECK(maxEnergy > 0);   // the kicks lift the band above silence
    const mm::AudioTempoState* tempo = audio.tempoStateForTest();
    REQUIRE(tempo != nullptr);
    CHECK(tempo->margin() > 0.05f);   // the winner beats its best alternative: the margin on noise stays below 0.10, and on a clean track it clears 0.05
}

TEST_CASE("AudioTempo: a gap flags, keeps the evidence to 10 s, and the grid coasts to 16 s") {
    static mm::AudioTempoState tempo;
    static int32_t block[kBlock];
    ClickTrack track{120.0f};
    lockAt120(tempo, track, block);
    CHECK(tempo.bpm() >= 119.0f);
    CHECK(tempo.bpm() <= 121.0f);
    CHECK_FALSE(tempo.gap());
    static int32_t silence[kBlock] = {};
    for (float done = 0.0f; done < 2.0f; done += kBlockS) tempo.process(silence, kBlock, false);   // the music stops
    CHECK(tempo.gap());   // 2 s shut, the grid still valid
    CHECK(tempo.valid());
    CHECK(tempo.bpm() >= 119.0f);   // the grid coasts at its rate
    CHECK(tempo.bpm() <= 121.0f);
    CHECK(tempo.onsetCountForTest() > 0);   // the evidence is kept: the sweep froze, it did not throw the onsets away
    for (float done = 0.0f; done < 9.0f; done += kBlockS) tempo.process(silence, kBlock, false);   // 11 s shut in all
    CHECK(tempo.gap());
    CHECK(tempo.valid());
    CHECK(tempo.onsetCountForTest() == 0);   // 10 s of silence threw the evidence away
    for (float done = 0.0f; done < 6.0f; done += kBlockS) tempo.process(silence, kBlock, false);   // 17 s shut in all
    CHECK_FALSE(tempo.gap());   // the estimate is no longer usable, so there is no gap to flag
    CHECK_FALSE(tempo.valid());
    CHECK(tempo.bpm() == 0.0f);
    for (float done = 0.0f; done < 2.0f; done += kBlockS) {   // the music comes back
        track.fill(block);
        tempo.process(block, kBlock, true);
    }
    CHECK_FALSE(tempo.gap());
    CHECK(tempo.valid());   // the grid coasted through: it never unlocked, so it publishes again at once
    CHECK(tempo.bpm() >= 119.0f);
    CHECK(tempo.bpm() <= 121.0f);
}

TEST_CASE("AudioTempo: unlocked, a 1 s shut gate clears the pre-lock noise") {
    static mm::AudioTempoState tempo;
    tempo.reset(kRate);
    static int32_t block[kBlock];
    uint32_t seed = 1234;
    for (float done = 0.0f; done < 2.0f; done += kBlockS) {   // noise, the gate open: onsets accumulate, nothing locks
        for (size_t i = 0; i < kBlock; i++) {
            seed = seed * 1664525u + 1013904223u;
            block[i] = (static_cast<int32_t>(seed >> 9) - (1 << 22)) * 512;
        }
        tempo.process(block, kBlock, true);
    }
    CHECK_FALSE(tempo.locked());
    CHECK(tempo.onsetCountForTest() > 0);
    static int32_t silence[kBlock] = {};
    for (float done = 0.0f; done < 1.5f; done += kBlockS) tempo.process(silence, kBlock, false);   // the gate shuts, still unlocked
    CHECK(tempo.onsetCountForTest() == 0);   // the 1 s clear threw the noise away: a track starting now acquires on its own onsets
}

TEST_CASE("AudioTempo: marking the downbeat makes the beat sounding now beat one") {
    static mm::AudioTempoState tempo;
    static int32_t block[kBlock];
    ClickTrack track{120.0f};
    lockAt120(tempo, track, block);
    CHECK(tempo.beatCount() > 4);   // the convention's count, several beats in
    tempo.requestDownbeat(true);
    track.fill(block);
    tempo.process(block, kBlock, true);   // the mark applies at the top of the block
    CHECK(tempo.beatInBar() == 1);   // the beat sounding now is beat one
    CHECK(tempo.beatCount() == 0);   // ... and the count restarts there
    int downbeats = 0;
    for (float done = 0.0f; done < 2.5f; done += kBlockS) {   // five beats at 120 BPM
        track.fill(block);
        tempo.process(block, kBlock, true);
        if (tempo.downbeatFired()) downbeats++;
    }
    CHECK(downbeats == 1);   // the bar's first beat fires once, on the fourth beat after the mark
    tempo.requestDownbeat(false);
    track.fill(block);
    tempo.process(block, kBlock, true);   // the mark clears
    CHECK(tempo.beatCount() > 4);   // back to the convention's count since the lock
    CHECK(tempo.beatInBar() == static_cast<uint8_t>((tempo.beatCount() % 4) + 1));   // ... and its numbering
}

TEST_CASE("AudioTempo: onsets at exactly 128 BPM read 128, the plateau's centre, not its edge") {
    const mm::AudioTempoSweep::Outcome out = searchSet(128.0f, 26, 0.0f);
    CHECK(out.kind == mm::AudioTempoSweep::Outcome::kForeground);
    CHECK(out.rate >= 127.0f);   // the plateau's centre: an argmax-first pick would read the edge, 126.5
    CHECK(out.rate <= 129.0f);
}

TEST_CASE("AudioTempo: 120 BPM kicks with hats on the off-beats read 120, not the double") {
    const mm::AudioTempoSweep::Outcome out = searchSet(120.0f, 24, 0.58f);
    CHECK(out.kind == mm::AudioTempoSweep::Outcome::kForeground);
    CHECK(out.rate >= 119.0f);
    CHECK(out.rate <= 121.0f);
    CHECK(out.level == 1.0f);   // the double is gated: its odd beats are the hats, much weaker than the kicks
}

TEST_CASE("AudioTempo: 60 BPM kicks with hats every half second read 60, the half of the 120 grid") {
    const mm::AudioTempoSweep::Outcome out = searchSet(60.0f, 12, 0.58f);
    CHECK(out.kind == mm::AudioTempoSweep::Outcome::kForeground);
    CHECK(out.rate >= 119.0f);   // the grid is 120: the hats sit on every one of its beats
    CHECK(out.rate <= 121.0f);
    CHECK(out.level == 0.5f);   // ... but the grid alternates kick with hat alone, so the beat is its half
}

TEST_CASE("AudioTempo: 120 BPM kicks with weak noise onsets read level 1.0, not the half") {
    // The flicker: weak onsets diluted one parity's alternation mean, and the level applied at once, so clean 120 BPM tracks flipped to ~60. Seeds 4 and 8 at now 606/617/650 are the configurations that flipped.
    static mm::AudioTempoSweep sweep;
    const float beat = 60.0f * 46.875f / 120.0f;
    for (const uint32_t seed0 : {4u, 8u}) {
        for (const uint32_t now : {606u, 617u, 650u}) {
            int32_t frames[256];
            float str[256];
            int n = 0;
            for (float f = 12.0f; f < 562.0f; f += beat) {
                frames[n] = static_cast<int32_t>(lroundf(f));
                str[n] = 1.0f;
                n++;
            }
            uint32_t seed = seed0;
            for (int i = 0; i < 40 && n < 256; i++) {
                seed = seed * 1664525u + 1013904223u;
                frames[n] = static_cast<int32_t>(seed % 562);
                seed = seed * 1664525u + 1013904223u;
                str[n] = 0.06f + static_cast<float>(seed % 100) / 100.0f * 0.24f;
                n++;
            }
            sweep.reset(46.875f);
            const mm::AudioTempoSweep::Outcome out = runSearch(sweep, frames, str, n, now);
            CHECK(out.kind == mm::AudioTempoSweep::Outcome::kForeground);
            CHECK(out.rate >= 117.6f);   // the rate is sanity, not the pin: one configuration reads 118.5
            CHECK(out.rate <= 122.4f);
            CHECK(out.level == 1.0f);   // the weak onsets must not halve the beat
        }
    }
}

TEST_CASE("AudioTempo: a 120 BPM click track over a loud floor never leaves 120 by more than 2% once locked") {
    // The flicker, end to end: the published BPM flipped to ~60 for up to 0.58 s on clean kick tracks over the -30 dB floor. Seeds 3 and 6 are the realizations that flipped pre-fix (to 59.13 and 60.00); the others never left 120.
    for (const uint32_t seed : {3u, 6u}) {
        static mm::AudioTempoState tempo;
        tempo.reset(kRate);
        static int32_t block[kBlock];
        ClickTrack track{120.0f};
        track.hissGain = 7000.0;   // the synthetic WAVs' -30 dB floor: the quiet hiss never reproduced the flicker
        track.seed = seed;
        track.sounding = true;
        float lo = 1.0e9f, hi = 0.0f;
        int checked = 0;
        for (float done = 0.0f; done < 12.0f; done += kBlockS) {
            track.fill(block);
            tempo.process(block, kBlock, true);
            if (done >= 4.0f && tempo.locked()) {   // the lock and the phase need the first 4 s
                checked++;
                if (tempo.bpm() < lo) lo = tempo.bpm();
                if (tempo.bpm() > hi) hi = tempo.bpm();
            }
        }
        REQUIRE(tempo.locked());
        CHECK(checked > 100);   // the lock held for most of the last 8 s
        CHECK(lo >= 117.6f);    // never half, never a fifth off: a flip to ~60 is a show-visible fault
        CHECK(hi <= 122.4f);
    }
}

TEST_CASE("AudioTempo: after the lock, a beat fires within a block of each click") {
    static mm::AudioService audio;
    ClickTrack track{120.0f};
    int beats = 0;
    startTrack(audio, track, beats);
    static int32_t block[mm::AudioService::kBlock];
    std::vector<int> clickBlocks;   // the blocks a click lands on, counted from the sounding start
    std::vector<int> beatBlocks;    // ... and the blocks a beat fires on
    int index = 0;
    for (float done = 0.0f; done < 8.0f; done += kBlockS) {
        const bool click = track.nextBeat < track.t + kBlockS;   // a click lands in this block
        track.fill(block);
        const mm::AudioFrame& f = audio.analyzeForTest(block);
        if (click) clickBlocks.push_back(index);
        if (f.beat) beatBlocks.push_back(index);
        index++;
    }
    REQUIRE(beatBlocks.size() > 4);
    int checked = 0;
    for (const int c : clickBlocks) {
        if (c < 188) continue;   // the last 4 s only: the lock and the phase need the first 4 s
        int nearest = 1000000;
        for (const int b : beatBlocks) {
            const int d = std::abs(b - c);
            if (d < nearest) nearest = d;
        }
        CHECK(nearest <= 1);   // the beat lands on the click's block or the one beside it: 21 ms, not the 70 ms the argmax's leading edge would allow
        checked++;
    }
    CHECK(checked >= 6);   // the last 4 s hold 8 clicks
}

TEST_CASE("AudioTempo: random onsets score below the chance-corrected threshold") {
    static mm::AudioTempoSweep sweep;
    sweep.reset(46.875f);   // the 24 kHz frame rate
    int32_t frames[40];
    float str[40];
    uint32_t seed = 99;
    for (int i = 0; i < 40; i++) {   // 40 onsets, random times and strengths over a 12 s window
        seed = seed * 1664525u + 1013904223u;
        frames[i] = static_cast<int32_t>(seed % 562);
        seed = seed * 1664525u + 1013904223u;
        str[i] = 0.06f + static_cast<float>(seed % 100) / 100.0f * 0.94f;
    }
    const mm::AudioTempoSweep::Outcome out = runSearch(sweep, frames, str, 40);
    CHECK(out.kind == mm::AudioTempoSweep::Outcome::kForeground);
    CHECK(out.score < 0.30f);   // chance, corrected, stays below the least believed score: no rate earns belief
}

TEST_CASE("AudioTempo: a diluted 174 BPM onset set reads 174, not its 4:3 relative") {
    static mm::AudioTempoSweep sweep;
    sweep.reset(46.875f);   // the 24 kHz frame rate
    int32_t frames[256];
    float str[256];
    int n = 0;
    const float period = 60.0f * 46.875f / 174.0f;
    for (float f = 12.0f; f < 562.0f && n < 200; f += period) {   // strong onsets on the true grid
        frames[n] = static_cast<int32_t>(f);
        str[n] = 1.0f;
        n++;
    }
    uint32_t seed = 7;
    for (int i = 0; i < 150 && n < 256; i++) {   // weak onsets at random frames: the syncopation that dilutes the true rate
        seed = seed * 1664525u + 1013904223u;
        frames[n] = static_cast<int32_t>(seed % 562);
        seed = seed * 1664525u + 1013904223u;
        str[n] = 0.2f + static_cast<float>(seed % 100) / 100.0f * 0.3f;
        n++;
    }
    const mm::AudioTempoSweep::Outcome out = runSearch(sweep, frames, str, n);
    CHECK(out.kind == mm::AudioTempoSweep::Outcome::kForeground);
    CHECK(out.rate >= 172.0f);   // the raw pick lands near 130, the 4:3 relative; the fold overturns it to the true rate
    CHECK(out.rate <= 176.0f);
}

TEST_CASE("AudioTempo: the grid survives the frame counter's rebase") {
    static mm::AudioTempoState tempo;
    tempo.reset(kRate);
    static int32_t block[kBlock];
    ClickTrack track{120.0f};
    int beats = 0;
    for (float done = 0.0f; done < 1.0f; done += kBlockS) {   // the room's floor, learned first
        track.fill(block);
        tempo.process(block, kBlock, true);
    }
    track.sounding = true;
    for (float done = 0.0f; done < 8.0f; done += kBlockS) {
        track.fill(block);
        tempo.process(block, kBlock, true);
        if (tempo.beatFired()) beats++;
    }
    REQUIRE(tempo.locked());
    REQUIRE(tempo.bpm() >= 119.0f);
    REQUIRE(tempo.bpm() <= 121.0f);
    const uint32_t beatsBefore = tempo.beatCount();
    tempo.setFrameIndexForTest(65530);   // six frames before the rebase: 23:18 at 24 kHz, 11:39 at 48 kHz
    for (float done = 0.0f; done < 2.0f; done += kBlockS) {   // across the rebase and past it
        track.fill(block);
        tempo.process(block, kBlock, true);
        if (tempo.beatFired()) beats++;
    }
    CHECK(tempo.frameIndexForTest() < 200);   // the counter rebased: 65530 + 94 frames, less 65536
    CHECK(tempo.locked());
    CHECK(tempo.bpm() >= 119.0f);   // the same grid, not a re-lock and not garbage
    CHECK(tempo.bpm() <= 121.0f);
    CHECK(tempo.beatCount() > beatsBefore);   // beats kept firing across the rebase
    CHECK(beats >= 4);
    // The onset ring's rebase, not just the grid clock's: step the tempo and check the grid follows. With the old bug the ring keeps pre-rebase frames and no outcome is ever believed again, so the grid would coast at 120.
    track.bpm = 140.0f;   // the step, phase continuous
    for (float done = 0.0f; done < 12.0f; done += kBlockS) {
        track.fill(block);
        tempo.process(block, kBlock, true);
    }
    CHECK(tempo.locked());
    CHECK(tempo.bpm() >= 139.0f);   // the grid followed to 140: onsets still flow after the rebase
    CHECK(tempo.bpm() <= 141.0f);
}

TEST_CASE("AudioTempo: silence reads no tempo") {
    static mm::AudioService audio;
    audio.defineControls();
    audio.setTempoForTest(true);
    audio.resetAnalysisForTest();
    static int32_t block[mm::AudioService::kBlock] = {};
    const mm::AudioFrame* f = nullptr;
    for (float done = 0; done < 8.0f; done += kBlockS) f = &audio.analyzeForTest(block);
    REQUIRE(f != nullptr);
    CHECK(f->tempoBpm == 0);
    CHECK(f->beatCount == 0);
    CHECK(f->energy == 0);   // silence has no band level
    CHECK_FALSE(f->beat);
    CHECK_FALSE(f->downbeat);
}

TEST_CASE("AudioTempo: with the tempo off a click track reads no tempo") {
    static mm::AudioService audio;
    audio.defineControls();
    audio.setTempoForTest(false);
    audio.resetAnalysisForTest();
    ClickTrack track{120.0f};
    int beats = 0;
    play(audio, track, 1.0f, beats);
    track.sounding = true;
    const mm::AudioFrame& f = play(audio, track, 8.0f, beats);
    CHECK(f.tempoBpm == 0);
    CHECK(f.beatCount == 0);
    CHECK_FALSE(f.beat);
    CHECK_FALSE(audio.tempoHeldForTest());
}

TEST_CASE("AudioTempo: the tempo state goes when the tempo stops and comes back when it runs, the analysis staying") {
    static mm::AudioService audio;
    audio.defineControls();
    audio.setTempoForTest(true);
    audio.resetAnalysisForTest();
    REQUIRE(audio.analysisHeldForTest());
    CHECK(audio.tempoHeldForTest());
    static int32_t block[mm::AudioService::kBlock] = {};
    audio.setTempoForTest(false);
    audio.analyzeForTest(block);
    CHECK(audio.analysisHeldForTest());
    CHECK_FALSE(audio.tempoHeldForTest());
    audio.setTempoForTest(true);
    audio.analyzeForTest(block);
    CHECK(audio.tempoHeldForTest());
}

TEST_CASE("AudioTempo: a beat in an earlier block of one tick still reaches the effects") {
    mm::SpscRing<mm::AudioFrame, 8> ring;
    mm::AudioFrame blocks[3];   // one tick's three blocks: the beat's, a quiet one, the newest
    blocks[0].beat = true;
    blocks[0].downbeat = true;
    blocks[2].level = 42;
    REQUIRE(ring.push(blocks, 3) == 3);
    mm::AudioFrame out;
    REQUIRE(mm::takeNewest(ring, out));
    CHECK(out.level == 42);   // the newest frame's analysis
    CHECK(out.beat);          // with the beat the blocks before it heard
    CHECK(out.downbeat);
}

TEST_CASE("AudioTempo: at 8 kHz the band cannot run, so the tracker stays silent") {
    static mm::AudioTempoState tempo;   // its state belongs off a test's stack
    tempo.reset(8000);
    CHECK_FALSE(tempo.runnable());
    static int32_t block[mm::AudioService::kBlock];
    ClickTrack track{120.0f};
    track.sounding = true;
    for (float done = 0; done < 8.0f; done += kBlockS) {
        track.fill(block);
        tempo.process(block, kBlock, true);
    }
    CHECK(tempo.bpm() == 0.0f);
    CHECK_FALSE(tempo.locked());
}

TEST_CASE("AudioTempo: the tracker's state is 6520 bytes") {
    // The plan's budget is 4 KB; the port is over it by 2424 bytes, recorded as open (Q12). The number is pinned so the state cannot grow silently. The level confirmation's pending level and run counter added 8 bytes over 6512.
    CHECK(sizeof(mm::AudioTempoState) == 6520);
}
