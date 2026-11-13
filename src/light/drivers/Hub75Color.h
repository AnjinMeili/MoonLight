#pragma once

#include "light/drivers/Correction.h"

#include <cstdint>

namespace mm {

/// Who applies the one color map, exactly one of the two: applying it twice darkens the picture as a square.
enum class MapOwner : uint8_t { Library, MoonLight };

/// The curve whose table the library builds itself, at up to 11 bits, is the library's; every other curve is MoonLight's, at 8.
inline MapOwner mapOwnerFor(Correction::Curve curve) {
    return curve == Correction::Curve::Cie ? MapOwner::Library : MapOwner::MoonLight;
}

/// The color depths an output drives, in bit planes, and the one a fresh card starts at; the defaults are the Pi's under MoonLight's map.
struct DepthRange {
    uint8_t min = 1;       ///< the fewest planes
    uint8_t max = 8;       ///< the most planes
    uint8_t initial = 8;   ///< where a card left at the default starts
};

/// The library's `pwm_bits` range for a map owner: a direct map shifts 8-bit values up by 3, so 8 bits are real, while the library's CIE table fills all 11.
inline DepthRange depthRangeFor(MapOwner owner) {
    DepthRange r;
    if (owner == MapOwner::Library) r.max = 11;
    return r;
}

/// A saved depth brought into the owner's range; a value out of it clamps and keeps its place.
inline uint8_t clampDepth(uint8_t depth, MapOwner owner) {
    const DepthRange r = depthRangeFor(owner);
    return depth < r.min ? r.min : (depth > r.max ? r.max : depth);
}

/// The library's dither range (`--led-pwm-dither-bits`), a setting the library owns at every depth.
constexpr uint8_t kDitherMax = 2;

} // namespace mm
