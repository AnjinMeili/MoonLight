#pragma once

#include "light/effects/PianoNotesBase.h"
#include "light/powerfunctions/boids.h"   // the step clock, the pace, the trail plane
#include "core/util/AudioPhrase.h"          // where the phrases start

#include <cstdio>    // TEMPORARY: the phrase capture
#include <cstdlib>   // TEMPORARY: the phrase capture

#if MM_HEAVY_COMPUTE   // only where the SoC declares a hardware FPU, for the reason boids.h carries

namespace mm {

/// The notes played become bodies that swing round a center of gravity for each octave, glowing in that octave's palette, and fade away as the notes die.
///
/// A key struck gives birth to its body beside its octave's center, thrown into orbit. The body glows and grows while the note sounds, cools as it rests, and dies once it is cold. The louder an octave plays, the harder its center pulls. The bodies dance to the beat: gravity draws them in as the beat nears and the beat throws them out, the bar's first beat hardest; until the tempo is known, the kick throws them. Once the phrases are found, the last beat before a phrase draws every body in and the phrase's first beat bursts them out, new bodies born in every octave and every octave on to the next palette.
///
/// In silence the keys are struck now and then at random, so the picture keeps breathing.
///
/// Compare with @ref MoonFlockEffect, the flock with a boid for each piano key.
///
/// @attribution James Hutchinson, Claude, Jason Coon, Daniel Shiffman
///
/// @guide
///
/// ## The octaves
///
/// The eight octaves, A0 to D#7, each have a center of gravity on a ring round the middle of the grid, low octaves first, and the ring turns slowly. Each octave draws from its own palette, warm low down and cold up high, the note's pitch class picking the color within it; under `colors` at `active palette` all octaves share the active palette, each a step along it.
///
/// @technical
///
/// ## After Aurora's Attract
///
/// The gravity is Jason Coon's Attract in Aurora, after Daniel Shiffman's *The Nature of Code*: a body is pulled by G M / d², the distance held between Aurora's 5 and 32, at most 1.5 a step, in Aurora's 32 by 32 space scaled onto the grid. Aurora steps once a frame at 120 frames a second; `speed` 120 keeps that pace on [boids.h](../../../src/light/powerfunctions/boids.h)'s step clock.
///
/// A key's heat is MoonFlock's: a strike adds `growth`, by how loud, and the heat halves every `fade` seconds on the effect's clock.
class AttractEffect : public PianoNotesBase {
public:
    /// Catalog tags: MoonLight origin, and reacting to which notes are playing.
    const char* tags() const override { return "💫🎶"; }
    /// Writes the z=0 slice, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// Where the colors come from: a palette for each octave, or the active palette.
    enum Colors : uint8_t { kColorsOctaves, kColorsActive, kColorsCount };

    /// How many lights wide a body is drawn, where 0 sizes it to the grid.
    uint8_t size = 2;
    /// Steps a second, where 120 is Aurora's pace and 0 holds the picture.
    uint8_t speed = 120;
    /// How much loudness speeds the bodies, up to double `speed`.
    uint8_t audioGain = 100;
    /// How hard the octaves' centers pull, where 128 is Aurora's.
    uint8_t gravity = 128;
    /// How much heat a key gains each time it is played, by how loud.
    uint8_t growth = 160;
    /// Seconds a key's heat takes to halve.
    uint8_t fade = 2;
    /// How hard the beat, or the kick, throws the bodies outward.
    uint8_t kick = 128;
    /// How hard gravity draws the bodies in as the beat nears.
    uint8_t dance = 160;
    /// How big the burst at a phrase's start, where 0 leaves the phrases be.
    uint8_t wow = 200;
    /// The phrase lengths `phrase` picks from, in its option order.
    enum Phrase : uint8_t { kPhraseAuto, kPhrase16, kPhrase32, kPhraseCount };
    /// How long a phrase runs: found from the music, or 16 beats as dance music runs, or 32 as some DJs do.
    uint8_t phrase = kPhraseAuto;
    /// Strike a key now and then while nothing sounds.
    bool idle = true;
    /// A palette for each octave, or the active palette.
    uint8_t colors = kColorsOctaves;

    /// Publish how big and fast the bodies fly, which notes count, the heat, the gravity and the colors.
    void defineControls() override {
        static constexpr const char* kColorsOptions[] = {"octave palettes", "active palette"};
        controls_.addControl("size", size, 0, boids::kMaxSize);
        controls_.addControl("speed", speed, 0, 255);
        controls_.addControl("audioGain", audioGain, 0, 255);
        addNotesSelect();
        controls_.addControl("gravity", gravity, 0, 255);
        controls_.addControl("growth", growth, 1, 255);
        controls_.addControl("fade", fade, 1, 60);
        controls_.addControl("kick", kick, 0, 255);
        controls_.addControl("dance", dance, 0, 255);
        controls_.addControl("wow", wow, 0, 255);
        static constexpr const char* kPhraseOptions[] = {"auto", "16 beats", "32 beats"};
        controls_.addSelect("phrase", phrase, kPhraseOptions, kPhraseCount);
        controls_.addControl("idle", idle);
        controls_.addSelect("colors", colors, kColorsOptions, kColorsCount);
    }

    /// Hold a body and a heat for every key, size and clear the trail, and build the octaves' palettes.
    void prepare() override {
        PianoNotesBase::prepare();
        tempoDemand_.set(true);
        if (!heat_.resize(kAudioNoteKeys) || !octavePalette_.resize(kOctaves)) {
            setStatus(boids::kNoMemory, Severity::Warning);
            return;
        }
        if (!flock_.prepare(*this, width(), height(), channelsPerLight(), kAudioNoteKeys)) return;
        std::fill_n(heat_.data(), kAudioNoteKeys, 0.0f);
        for (uint8_t o = 0; o < kOctaves; o++) octavePalette_[o] = Palettes::fromBuiltin(builtinNamed(kOctavePalettes[o]));
        rng_ = Random8{boids::kSeed};
        heardSeq_ = AudioService::latestFrame()->seq;
        turn_ = 0;
        steps_ = 0;
        clockStarted_ = false;
        squeeze_ = 1;
        flash_ = 0;
        shift_ = 0;
        phrase_.reset();
        clock_.reset();
    }

    /// Hear the frame, run the steps this tick owes within its time, copy the trail's colors to the layer, and draw the living bodies over them.
    void tick() MM_NONBLOCKING override {
        const std::optional<boids::Plane> trail = flock_.plane(width(), height(), channelsPerLight());
        if (!trail || !heat_ || !octavePalette_) return;   // a resize outran the prepare, or memory ran out: the frame waits for it

        const uint32_t now = elapsed();
        if (!clockStarted_) {
            clockStarted_ = true;
            cooledMs_ = now;
            flashMs_ = now;
            idleMs_ = now;
        }
        listen(now);
        squeeze_ = windUp(*AudioService::latestFrame());

        const float r = boids::radius(size, trail->w(), trail->h());
        boids::runSteps(clock_.steps(now, boids::pace(speed, audioGain)), [&] { step(*trail, r); });

        const boids::Plane layer{canvas()};
        trail->copyColorsTo(layer.cv);
        for (uint8_t k = 0; k < kAudioNoteKeys; k++) {
            if (heat_[k] < kAlive) continue;
            const boids::Boid& b = flock_.boids[k];
            disc(layer, b.x, b.y, colorOf(k, 255), r * (1.0f + kSwell * heat_[k] / 255.0f) * (1.0f + flash_));
        }
    }

    /// Key `k`'s heat, for tests.
    float heatForTest(uint8_t k) const { return heat_[k]; }
    /// Key `k`'s body, for tests.
    const boids::Boid& bodyForTest(uint8_t k) const { return flock_.boids[k]; }
    /// Key `k`'s color at full brightness, for tests.
    RGB colorForTest(uint8_t k) const { return colorOf(k, 255); }
    /// How far key `k`'s body sits from its octave's center, for tests.
    float reachForTest(uint8_t k) const {
        float cx, cy;
        centerOf(octaveOf(k), cx, cy);
        return std::hypot(flock_.boids[k].x - cx, flock_.boids[k].y - cy);
    }
    /// The octave key `k` falls in, 0 for A0 to B0, for tests.
    static uint8_t octaveOf(uint8_t k) { return static_cast<uint8_t>((kMidiA0 + k) / 12 - 1); }

private:
    /// The octaves the keys span, A0 to D#7.
    static constexpr uint8_t kOctaves = 8;
    /// Key 0's MIDI number.
    static constexpr uint8_t kMidiA0 = 21;
    /// The heat under which a body is dead: not drawn, not moved.
    static constexpr float kAlive = 1.0f;
    /// A strike counts at least this loud, so a soft key is still born.
    static constexpr uint8_t kStrikeFloor = 64;
    /// Aurora's G M, in its 32 by 32 space, and the distance the pull is held between.
    static constexpr float kGM = 5.0f;
    static constexpr float kNear = 5.0f, kFar = 32.0f;
    /// Aurora's top speed, a step.
    static constexpr float kMaxSpeed = 1.5f;
    /// How far from the middle the octaves' centers sit, and how far from its center a body is born.
    static constexpr float kRing = 9.0f, kBirth = 4.0f;
    /// The ring turns once in this many steps, about a minute at 120 a second.
    static constexpr uint32_t kTurnSteps = 7200;
    /// How far a body may wander outside the space before it is held, as Aurora's sketch holds it.
    static constexpr float kMargin = 8.0f;
    /// A beat other than the bar's first throws this much of the downbeat's throw.
    static constexpr float kOffBarThrow = 0.6f;
    /// The wind-up starts this far through the beat, of 255: the last third.
    static constexpr uint8_t kWindUpFrom = 170;
    /// How much a sounding note swells its body, at full heat.
    static constexpr float kSwell = 0.5f;
    /// The burst's flash halves every this many milliseconds.
    static constexpr float kFlashHalfLifeMs = 120.0f;
    /// The burst gives birth to this many bodies in each octave.
    static constexpr uint8_t kBurstPerOctave = 2;
    /// In silence, a key is struck about every this many milliseconds.
    static constexpr uint32_t kIdleEveryMs = 700;
    /// The octaves' palettes, low to high: warm low down, cold up high.
    static constexpr const char* kOctavePalettes[kOctaves] = {"Lava", "Sunset", "Autumn", "Orangery", "Forest", "Aurora", "Ocean", "Fierce Ice"};

    boids::Flock flock_{*this};       ///< a body for every key, and the trail
    ScratchBuffer<float> heat_{*this};   ///< every key's heat, 0 to 255
    ScratchBuffer<Palette> octavePalette_{*this};   ///< each octave's palette, built from the prepare
    boids::StepClock clock_;
    Random8 rng_{boids::kSeed};
    uint16_t heardSeq_ = 0;    ///< the frame whose strikes were counted
    uint32_t cooledMs_ = 0;    ///< when the heat last cooled
    uint32_t flashMs_ = 0;     ///< when the flash last faded
    uint32_t waitShownMs_ = 0; ///< when the status last showed the wait for a tempo
    uint32_t idleMs_ = 0;      ///< when a key last sounded or was struck in silence
    uint32_t steps_ = 0;       ///< steps taken, which turn the ring
    float turn_ = 0;           ///< the ring's angle, in radians
    float throw_ = 0;          ///< the beat's or the kick's throw still to be spent on the next step, 0 to 1
    float squeeze_ = 1;        ///< gravity's swell as the beat nears, 1 off the beat
    float flash_ = 0;          ///< the burst's flash, 1 as a phrase starts, fading
    uint8_t shift_ = 0;        ///< how far the octaves have moved along the palettes, a step a phrase
    AudioPhrase phrase_;       ///< where the phrases start
    char phraseText_[80] = {};   ///< the phrase readout the status shows
    AudioDemand tempoDemand_{*this, AudioFeature::Tempo};   ///< the beat, while the effect is on
    bool clockStarted_ = false;

    /// The built-in palette called `name`, or the first.
    static uint8_t builtinNamed(const char* name) {
        for (uint8_t i = 0; i < palettes::kCount; i++)
            if (std::strcmp(palettes::kBuiltins[i].name, name) == 0) return i;
        return 0;
    }

    /// The fraction of a heat left after `dtMs` with a half-life of `halfLifeMs`.
    static float survives(uint32_t dtMs, float halfLifeMs) { return halfLifeMs > 0 ? std::exp2(-static_cast<float>(dtMs) / halfLifeMs) : 0.0f; }

    /// Key `k`'s color at `brightness`: its pitch class along its octave's palette, or along the active palette a step for each octave.
    RGB colorOf(uint8_t k, uint8_t brightness) const {
        const uint8_t pc = (kMidiA0 + k) % 12;
        const uint8_t along = static_cast<uint8_t>(pc * 256 / 12);
        if (colors == kColorsActive) return colorFromPalette(*Palettes::active(), static_cast<uint8_t>(along + octaveOf(k) * 32), brightness);
        return colorFromPalette(octavePalette_[(octaveOf(k) + shift_) % kOctaves], along, brightness);
    }

    /// Octave `o`'s center of gravity, on the ring round the middle of the space.
    void centerOf(uint8_t o, float& x, float& y) const {
        const float a = turn_ + o * (4 * boids::kQuarterTurn / kOctaves);
        x = boids::kSize / 2 + kRing * std::cos(a);
        y = boids::kSize / 2 + kRing * std::sin(a);
    }

    /// Cool every key, take this frame's strikes once, take the kick, and strike a key now and then in silence.
    void listen(uint32_t now) {
        const float left = survives(now - cooledMs_, fade * 1000.0f);
        cooledMs_ = now;
        for (uint8_t k = 0; k < kAudioNoteKeys; k++) heat_[k] *= left;
        flash_ *= survives(now - flashMs_, kFlashHalfLifeMs);
        flashMs_ = now;

        const AudioFrame* f = AudioService::latestFrame();
        if (f->seq != heardSeq_) {
            heardSeq_ = f->seq;
            if (f->tempoBpm) {
                if (f->beat) throw_ = std::max(throw_, kick / 255.0f * (f->downbeat ? 1.0f : kOffBarThrow));
            } else if (f->onsetLow) {
                throw_ = std::max(throw_, f->onsetLow * kick / (255.0f * 255.0f));
            }
            phrase_.onlyLength(phrase == kPhrase16 ? 16 : phrase == kPhrase32 ? 32 : 0);
            if (phrase_.hear(*f)) {
                captureBeat(*f);   // TEMPORARY: the phrase fold's capture for tuning, removed before commit
                setStatus(phrase_.describe(phraseText_, sizeof(phraseText_)));
                if (wow && phrase_.onStart()) burst();
            } else if (!f->tempoBpm && now - waitShownMs_ >= 1000) {   // while there is no tempo, what the tracker hears, once a second
                waitShownMs_ = now;
                std::snprintf(phraseText_, sizeof(phraseText_), "waiting for a tempo: confidence %u, energy %u", f->tempoConfidence, f->energy);
                setStatus(phraseText_);
            }
            keep(*f, now);
            for (uint8_t k = 0; k < kAudioNoteKeys; k++)
                if (hitAt(struck_, k) && shown_.has(k)) { strike(k, std::max(level(*f, k), kStrikeFloor)); idleMs_ = now; }
        }
        if (idle && now - idleMs_ >= kIdleEveryMs) {
            idleMs_ = now;
            strike(static_cast<uint8_t>(rng_.next8() % kAudioNoteKeys), 255);
        }
    }

    /// Strike key `k` at `loudness`: a cold key's body is born beside its octave's center, thrown into orbit, and the key heats.
    void strike(uint8_t k, uint8_t loudness) {
        if (heat_[k] < kAlive) {
            float cx, cy;
            centerOf(octaveOf(k), cx, cy);
            const float a = boids::uniform(rng_) * 4 * boids::kQuarterTurn;
            boids::Boid& b = flock_.boids[k];
            b = boids::Boid{};
            b.x = cx + kBirth * std::cos(a);
            b.y = cy + kBirth * std::sin(a);
            // Thrown across the line to its center at a speed near a circular orbit's, sqrt(G M / r), so it swings round rather than falls in.
            const float v = std::sqrt(pull() * kGM / kNear) * (0.6f + 0.4f * boids::uniform(rng_));
            b.vx = -std::sin(a) * v;
            b.vy = std::cos(a) * v;
        }
        heat_[k] = std::min(255.0f, heat_[k] + growth * loudness / 255.0f);
    }

    /// Gravity's swell in frame `f`: 1 off the beat, rising with the square of the beat's progress over its last third to 1 + `dance` / 128 as the beat lands, and 1 while the tempo is unknown. On the last beat before a phrase starts it rises over the whole beat, to 1 + `wow` / 32.
    float windUp(const AudioFrame& f) const {
        if (!f.tempoBpm) return 1;
        if (wow && phrase_.length() && phrase_.beatsToStart() == 1) {
            const float x = f.beatPhase / 255.0f;
            return 1 + wow / 32.0f * x * x;
        }
        if (f.beatPhase < kWindUpFrom) return 1;
        const float x = static_cast<float>(f.beatPhase - kWindUpFrom) / (255 - kWindUpFrom);
        return 1 + dance / 128.0f * x * x;
    }

    // TEMPORARY, removed before commit: each closed beat's flux rows to the file MM_PHRASE_DUMP names, on a desktop host, to tune the phrase fold on real music.
#if defined(MM_HAS_LINK) && defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfunction-effects"
#endif
    void captureBeat(const AudioFrame& f) {
#ifdef MM_HAS_LINK
        static FILE* out = [] { const char* p = std::getenv("MM_PHRASE_DUMP"); return p ? std::fopen(p, "a") : nullptr; }();
        if (!out) return;
        const float* rows = phrase_.beatRows();
        std::fprintf(out, "%u %u %u", static_cast<unsigned>(f.beatCount), f.beatInBar, f.tempoBpm);
        for (uint8_t r = 0; r < AudioPhrase::kRows; r++) std::fprintf(out, " %.4f", static_cast<double>(rows[r]));
        std::fprintf(out, "\n");
        std::fflush(out);
#else
        (void)f;
#endif
    }
#if defined(MM_HAS_LINK) && defined(__clang__)
#pragma clang diagnostic pop
#endif

    /// A phrase starts: every body is thrown out at full speed, bodies are born in every octave, the heads flash, and every octave moves on to the next palette.
    void burst() {
        throw_ = 1;
        flash_ = wow / 255.0f;
        shift_ = static_cast<uint8_t>((shift_ + 1) % kOctaves);
        for (uint8_t o = 0; o < kOctaves; o++) {
            const int first = 12 * (o + 1) - kMidiA0;   // the octave's C, as a key; A0's octave starts below the keyboard
            for (uint8_t i = 0; i < kBurstPerOctave; i++) {
                const int k = std::clamp(first + rng_.next8() % 12, 0, kAudioNoteKeys - 1);
                strike(static_cast<uint8_t>(k), 255);
            }
        }
    }

    /// A hard-edged disc at (x, y) in the flock's space: every light within `radius` takes `c` whole; the one light it stands on when it is that small.
    static void disc(const boids::Plane& p, float x, float y, RGB c, float radius) {
        if (radius < 0.75f) {
            p.head(x, y, c, radius);
            return;
        }
        p.each(p.lightX(x), p.lightY(y), radius, [&](int32_t px, int32_t py, float dx, float dy) {
            if (dx * dx + dy * dy <= radius * radius) p.blend(px, py, c, 1.0f);
        });
    }

    /// How hard the centers pull, from `gravity`, Aurora's at 128.
    float pull() const { return gravity / 128.0f; }

    /// One step: the ring turns, each living body is pulled to its octave's center, harder the louder that octave plays and as the beat nears, the beat or the kick throws them out, and the trail fades and each leaves its streak.
    void step(const boids::Plane& trail, float r) {
        steps_++;
        turn_ = (steps_ % kTurnSteps) * (4 * boids::kQuarterTurn / kTurnSteps);

        float loud[kOctaves] = {};
        for (uint8_t k = 0; k < kAudioNoteKeys; k++) loud[octaveOf(k)] += heat_[k] / 255.0f;

        // Aurora's breathing trail: it keeps between 150 and 230 of 255 a frame, swinging twice a minute.
        const float swing = (std::sin(steps_ * (4 * boids::kQuarterTurn) / (60.0f * 120.0f / 2.0f)) + 1) / 2;
        draw::fade(trail.cv, static_cast<uint8_t>(255 - (150 + 80 * swing)));

        const float out = throw_;
        throw_ = 0;
        for (uint8_t k = 0; k < kAudioNoteKeys; k++) {
            if (heat_[k] < kAlive) continue;
            boids::Boid& b = flock_.boids[k];
            float cx, cy;
            centerOf(octaveOf(k), cx, cy);
            float dx = cx - b.x, dy = cy - b.y;
            const float d = std::clamp(std::sqrt(dx * dx + dy * dy), kNear, kFar);
            const float mass = pull() * squeeze_ * (0.5f + std::min(loud[octaveOf(k)], 2.0f));
            boids::setLength(dx, dy, mass * kGM / (d * d));
            b.vx += dx;
            b.vy += dy;
            if (out > 0) {   // the throw: straight out from its center
                float ox = b.x - cx, oy = b.y - cy;
                boids::setLength(ox, oy, out * kMaxSpeed);
                b.vx += ox;
                b.vy += oy;
            }
            boids::clampLength(b.vx, b.vy, kMaxSpeed);
            const float x0 = b.x, y0 = b.y;
            b.x = std::clamp(b.x + b.vx, -kMargin, boids::kSize + kMargin);
            b.y = std::clamp(b.y + b.vy, -kMargin, boids::kSize + kMargin);
            trail.trace(x0, y0, b.x, b.y, colorOf(k, static_cast<uint8_t>(64 + heat_[k] * 191 / 255)), r);
        }
    }
};

}  // namespace mm

#endif  // MM_HEAVY_COMPUTE
