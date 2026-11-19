/// @module AttractEffect

#include "doctest.h"
#include "golden_frame.h"
#include "light/effects/AttractEffect.h"

#include <cstring>

namespace {

/// An Attract on Aurora's 32 by 32 grid unless asked, held across ticks, with the audio source whose frames it hears; idle off unless asked, so only the keys played live.
struct AttractRig {
    mm::golden::ScopedTestClock clock{1000};
    mm::AudioService audio;
    mm::golden::LayerFixture fx;
    mm::Layer& layer = fx.layer;
    mm::AttractEffect attract;
    uint32_t now = 1000;

    explicit AttractRig(bool idle = false, mm::lengthType w = 32, mm::lengthType h = 32, uint8_t cpl = 3) : fx(w, h, 1, cpl) {
        audio.defineControls();
        audio.setup();
        audio.tick();   // claims the seat latestFrame reads
        attract.idle = idle;
        attract.notes = mm::AudioNotesBase::kNotesAll;   // every key heard, so a strike is a birth
        fx.add(attract);
    }
    ~AttractRig() { audio.release(); }

    /// Strike key `k` loud on a new frame, as the core reports a kept note's attack, and tick once to hear it.
    void strike(uint8_t k) {
        mm::AudioFrame& f = audio.frameForTest();
        std::memset(f.notesHits, 0, sizeof(f.notesHits));
        std::memset(f.notesKeptHits, 0, sizeof(f.notesKeptHits));
        f.notesCount = mm::kAudioNoteKeys;
        f.notes[k] = 255;
        f.notesKept[k >> 3] |= static_cast<uint8_t>(1u << (k & 7));
        f.notesKeptHits[k >> 3] |= static_cast<uint8_t>(1u << (k & 7));
        f.notesHits[k >> 3] |= static_cast<uint8_t>(1u << (k & 7));
        f.seq++;
        run(1, 20);
    }

    /// A kick of strength `low` on a new frame, ticked once to hear it.
    void kick(uint8_t low) {
        mm::AudioFrame& f = audio.frameForTest();
        std::memset(f.notesHits, 0, sizeof(f.notesHits));
        std::memset(f.notesKeptHits, 0, sizeof(f.notesKeptHits));
        f.onsetLow = low;
        f.seq++;
        run(1, 20);
        f.onsetLow = 0;
    }

    /// A frame from the tempo tracker at 120 BPM, `phase` through the beat, the beat and the bar's first beat landing on it when asked, with a kick of strength `low`, ticked once to hear it.
    void tempo(uint8_t phase, bool beat = false, bool downbeat = false, uint8_t low = 0) {
        mm::AudioFrame& f = audio.frameForTest();
        std::memset(f.notesHits, 0, sizeof(f.notesHits));
        std::memset(f.notesKeptHits, 0, sizeof(f.notesKeptHits));
        f.tempoBpm = 12000;
        f.beatPhase = phase;
        f.beat = beat;
        f.downbeat = downbeat;
        f.onsetLow = low;
        f.seq++;
        run(1, 20);
        f.beat = f.downbeat = false;
        f.onsetLow = 0;
    }

    /// Tick `frames` times `stepMs` apart.
    void run(uint32_t frames, uint32_t stepMs) {
        for (uint32_t i = 0; i < frames; i++) {
            now += stepMs;
            mm::platform::setTestNowMs(now);
            layer.tick();
        }
    }

    /// How many lights carry any color.
    size_t lit() {
        auto& buf = layer.buffer();
        const uint8_t cpl = layer.channelsPerLight();
        size_t n = 0;
        for (size_t i = 0; i + cpl <= buf.bytes(); i += cpl) {
            bool on = false;
            for (uint8_t c = 0; c < cpl; c++) on = on || buf.data()[i + c] != 0;
            n += on;
        }
        return n;
    }
};

constexpr uint8_t kA0 = 0, kB0 = 2, kC1 = 3, kC4 = 39, kC7 = 75, kDs7 = 78;

}  // namespace

TEST_CASE("AttractEffect counts the octaves from C, A0 to B0 being the first") {
    CHECK(mm::AttractEffect::octaveOf(kA0) == 0);
    CHECK(mm::AttractEffect::octaveOf(kB0) == 0);
    CHECK(mm::AttractEffect::octaveOf(kC1) == 1);
    CHECK(mm::AttractEffect::octaveOf(kC4) == 4);
    CHECK(mm::AttractEffect::octaveOf(kDs7) == 7);
}

TEST_CASE("AttractEffect holds a body and a heat for every key, the trail and the octaves' palettes, and none once disabled") {
    AttractRig rig;
    CHECK(rig.attract.dynamicBytes() == 32 * 32 * 3 + mm::kAudioNoteKeys * sizeof(mm::boids::Boid) + mm::kAudioNoteKeys * sizeof(float) + 8 * sizeof(mm::Palette));
    rig.attract.setEnabled(false);
    rig.layer.applyState();
    CHECK(rig.attract.dynamicBytes() == 0);
}

TEST_CASE("AttractEffect in silence with idle off shows nothing") {
    AttractRig rig;
    rig.run(100, 20);
    CHECK(rig.lit() == 0);
}

TEST_CASE("AttractEffect gives birth to a struck key's body beside its octave's center") {
    AttractRig rig;
    rig.strike(kC4);
    CHECK(rig.attract.heatForTest(kC4) > 100);
    CHECK(rig.attract.reachForTest(kC4) < 6);
    CHECK(rig.lit() > 0);
    CHECK(rig.attract.heatForTest(kC1) == 0);
}

TEST_CASE("AttractEffect cools a key's heat by half every fade seconds, and the body dies cold") {
    AttractRig rig;
    rig.attract.fade = 1;
    rig.strike(kC4);
    const float hot = rig.attract.heatForTest(kC4);
    rig.run(50, 20);   // one second
    CHECK(rig.attract.heatForTest(kC4) == doctest::Approx(hot / 2).epsilon(0.05));
    rig.run(500, 20);   // ten more: under the threshold
    CHECK(rig.attract.heatForTest(kC4) < 1);
    rig.run(100, 20);   // the trail fades out behind it
    CHECK(rig.lit() == 0);
}

TEST_CASE("AttractEffect keeps a body swinging round its center rather than falling in or flying off") {
    AttractRig rig;
    rig.attract.fade = 60;
    rig.strike(kC4);
    float least = 100, most = 0;
    for (int i = 0; i < 300; i++) {
        rig.run(1, 20);
        least = std::min(least, rig.attract.reachForTest(kC4));
        most = std::max(most, rig.attract.reachForTest(kC4));
    }
    CHECK(least > 0.2f);
    CHECK(most < 32);
}

TEST_CASE("AttractEffect colors the same pitch class from a different palette in each octave") {
    AttractRig rig;
    const mm::RGB low = rig.attract.colorForTest(kC1), high = rig.attract.colorForTest(kC7);
    CHECK((low.r != high.r || low.g != high.g || low.b != high.b));
}

TEST_CASE("AttractEffect throws its bodies outward on a kick") {
    // One rig at a time: each claims the seat latestFrame reads, so two alive at once hear the same frames.
    auto reachAfter = [](uint8_t low) {
        AttractRig rig;
        rig.attract.fade = 60;
        rig.strike(kC4);
        if (low) rig.kick(low);
        else rig.run(1, 20);
        rig.run(5, 20);
        return rig.attract.reachForTest(kC4);
    };
    CHECK(reachAfter(255) > reachAfter(0));
}

TEST_CASE("AttractEffect in silence with idle on strikes keys now and then") {
    AttractRig rig(true);
    rig.run(100, 20);   // two seconds
    uint8_t alive = 0;
    for (uint8_t k = 0; k < mm::kAudioNoteKeys; k++) alive += rig.attract.heatForTest(k) >= 1;
    CHECK(alive >= 2);
    CHECK(rig.lit() > 0);
}

TEST_CASE("AttractEffect survives degenerate grids") {
    for (mm::lengthType w : {1, 2, 5}) {
        AttractRig rig(true, w, 1);
        rig.run(100, 20);
        CHECK(true);
    }
}

TEST_CASE("AttractEffect throws its bodies on the tracked beat, and leaves the kick alone once the tempo is known") {
    // One rig at a time, as above. `then` plays the frame that throws or does not, at the start of a beat, where gravity is at rest.
    auto reachAfter = [](auto then) {
        AttractRig rig;
        rig.attract.fade = 60;
        rig.strike(kC4);
        then(rig);
        rig.run(5, 20);
        return rig.attract.reachForTest(kC4);
    };
    const float still = reachAfter([](AttractRig& r) { r.tempo(0); });
    CHECK(reachAfter([](AttractRig& r) { r.tempo(0, true); }) > still);
    CHECK(reachAfter([](AttractRig& r) { r.tempo(0, true, true); }) > reachAfter([](AttractRig& r) { r.tempo(0, true); }));
    CHECK(reachAfter([](AttractRig& r) { r.tempo(0, false, false, 255); }) == doctest::Approx(still));
}

TEST_CASE("AttractEffect draws its bodies in harder as the beat nears") {
    auto reachAt = [](uint8_t phase) {
        AttractRig rig;
        rig.attract.fade = 60;
        rig.strike(kC4);
        rig.run(10, 20);
        for (int i = 0; i < 10; i++) rig.tempo(phase);
        return rig.attract.reachForTest(kC4);
    };
    CHECK(reachAt(250) < reachAt(0));
}
