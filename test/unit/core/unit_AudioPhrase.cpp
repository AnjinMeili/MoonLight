/// @module AudioPhrase

#include "doctest.h"
#include "core/util/AudioPhrase.h"

#include <cstdint>
#include <vector>

namespace {

/// Frames a beat at about 124 BPM, a block every 21 ms.
constexpr int kFramesPerBeat = 23;

/// A track: a kick on every beat and a hat between, and, when `phrase` is set, a crash on each phrase's first beat and a snare fill through its last; `noise` replaces it all with random bands.
struct Song {
    int phrase = 0;
    bool noise = false;

    /// Frame `i` of beat `beat`.
    mm::AudioFrame frame(int beat, int i) const {
        mm::AudioFrame f;
        f.tempoBpm = 12400;
        f.beatCount = static_cast<uint16_t>(beat);
        f.beatPhase = static_cast<uint8_t>(i * 256 / kFramesPerBeat);
        f.beatInBar = static_cast<uint8_t>(beat % 4 + 1);
        if (noise) {
            for (uint8_t b = 0; b < 16; b++) {   // a hash of the beat, the frame and the band (the murmur3 finalizer), so no two draw alike
                uint32_t x = static_cast<uint32_t>((beat * kFramesPerBeat + i) * 16 + b + 1);
                x ^= x >> 16; x *= 0x85ebca6bu; x ^= x >> 13; x *= 0xc2b2ae35u; x ^= x >> 16;
                f.bands[b] = static_cast<uint8_t>(40 + x % 200);
            }
            return f;
        }
        for (uint8_t b = 0; b < 16; b++) f.bands[b] = 60;
        if (i == 0) for (uint8_t b = 0; b < 3; b++) f.bands[b] = 250;     // the kick
        if (i == 11) for (uint8_t b = 12; b < 16; b++) f.bands[b] = 200;  // the hat
        if (phrase) {
            const int at = beat % phrase;
            if (at == 0 && i == 0) for (uint8_t b = 0; b < 16; b++) f.bands[b] = 250;            // the crash
            if (at == phrase - 1 && i % 3 == 0) for (uint8_t b = 5; b < 9; b++) f.bands[b] = 220;  // the fill
        }
        return f;
    }
};

/// Hear `beats` beats of `song`; returns the beats on which a phrase was counted to start.
std::vector<uint32_t> listen(mm::AudioPhrase& p, const Song& song, int beats) {
    std::vector<uint32_t> starts;
    for (int b = 0; b < beats; b++)
        for (int i = 0; i < kFramesPerBeat; i++)
            if (p.hear(song.frame(b, i)) && p.onStart()) starts.push_back(p.beats());
    return starts;
}

}  // namespace

TEST_CASE("AudioPhrase finds no phrase in a loop that never changes") {
    mm::AudioPhrase p;
    listen(p, Song{}, 300);
    CHECK(p.length() == 0);
}

TEST_CASE("AudioPhrase finds no phrase in noise") {
    mm::AudioPhrase p;
    listen(p, Song{0, true}, 300);
    CHECK(p.length() == 0);
}

TEST_CASE("AudioPhrase finds dance music's 16-beat phrases and counts the next starts on the crash") {
    mm::AudioPhrase p;
    const std::vector<uint32_t> starts = listen(p, Song{16}, 200);
    CHECK(p.length() == 16);
    REQUIRE(starts.size() >= 3);
    for (uint32_t s : starts) CHECK(s % 16 == 0);
}

TEST_CASE("AudioPhrase takes 32-beat phrases as 32, not 16") {
    mm::AudioPhrase p;
    const std::vector<uint32_t> starts = listen(p, Song{32}, 300);
    CHECK(p.length() == 32);
    REQUIRE(!starts.empty());
    for (uint32_t s : starts) CHECK(s % 32 == 0);
}

TEST_CASE("AudioPhrase finds a twelve-bar blues' 48 beats") {
    mm::AudioPhrase p;
    listen(p, Song{48}, 400);
    CHECK(p.length() == 48);
}

TEST_CASE("AudioPhrase asked for 32-beat phrases takes 32 where the music lines up at 16") {
    mm::AudioPhrase p;
    p.onlyLength(32);
    listen(p, Song{16}, 300);
    CHECK(p.length() == 32);
}

TEST_CASE("AudioPhrase starts over when the tempo is lost") {
    mm::AudioPhrase p;
    listen(p, Song{16}, 200);
    REQUIRE(p.length() == 16);
    mm::AudioFrame lost;
    p.hear(lost);
    CHECK(p.length() == 0);
    CHECK(p.beats() == 0);
}

TEST_CASE("AudioPhrase counts the beats to the next start down to 0 on the start") {
    mm::AudioPhrase p;
    listen(p, Song{16}, 150);
    REQUIRE(p.length() == 16);
    CHECK(p.beatsToStart() == (16 - p.beats() % 16) % 16);
}

TEST_CASE("AudioPhrase sets no phrase before it has heard three phrases of 32 beats") {
    mm::AudioPhrase p;
    listen(p, Song{16}, 90);
    CHECK(p.length() == 0);
    listen(p, Song{16}, 20);   // beats 0-19 again: the count runs on from where it was, only the clock's numbers repeat
    CHECK(p.beats() >= 96);
}

TEST_CASE("AudioPhrase starts over when the tempo moves by more than 1%") {
    mm::AudioPhrase p;
    listen(p, Song{16}, 200);
    REQUIRE(p.length() == 16);
    mm::AudioFrame f = Song{16}.frame(200, 0);
    f.tempoBpm = 13000;
    p.hear(f);
    CHECK(p.length() == 0);
}

TEST_CASE("AudioPhrase lets a phrase go once the music has stopped lining up for a whole history") {
    mm::AudioPhrase p;
    listen(p, Song{16}, 200);
    REQUIRE(p.length() == 16);
    for (int b = 200; b < 200 + 2 * mm::AudioPhrase::kHistory; b++)
        for (int i = 0; i < kFramesPerBeat; i++) p.hear(Song{0, true}.frame(b, i));
    CHECK(p.length() == 0);
}
