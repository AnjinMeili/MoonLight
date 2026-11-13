/// @module Hub75Color

#include "doctest.h"
#include "light/drivers/Hub75Color.h"

using namespace mm;

// The rules: one color map is ever applied; the Pi renders at 8 bits; 9 to 11 exist only with the curve whose library table supplies them. The library facts (11 planes, the shift by 3, the CIE table) are its source's at the pinned version.

TEST_CASE("Hub75Color: only the CIE 1931 curve is the library's to apply") {
    CHECK(mapOwnerFor(Correction::Curve::Cie) == MapOwner::Library);
    CHECK(mapOwnerFor(Correction::Curve::Gamma22) == MapOwner::MoonLight);
    CHECK(mapOwnerFor(Correction::Curve::Gamma28) == MapOwner::MoonLight);
    CHECK(mapOwnerFor(Correction::Curve::Linear) == MapOwner::MoonLight);
}

TEST_CASE("Hub75Color: the Pi renders at 8 bits whoever owns the map") {
    CHECK(depthRangeFor(MapOwner::Library).initial == 8);
    CHECK(depthRangeFor(MapOwner::MoonLight).initial == 8);
}

TEST_CASE("Hub75Color: 9 to 11 bits belong to the library's table alone") {
    const DepthRange lib = depthRangeFor(MapOwner::Library);
    CHECK(lib.min == 1);
    CHECK(lib.max == 11);
    const DepthRange mine = depthRangeFor(MapOwner::MoonLight);
    CHECK(mine.min == 1);
    CHECK(mine.max == 8);   // planes 0 to 2 stay empty under a direct map
}

TEST_CASE("Hub75Color: a saved depth is clamped into the owner's range, never reset") {
    CHECK(clampDepth(11, MapOwner::MoonLight) == 8);   // the curve changed from CIE 1931
    CHECK(clampDepth(9, MapOwner::MoonLight) == 8);
    CHECK(clampDepth(7, MapOwner::MoonLight) == 7);    // inside the range: kept
    CHECK(clampDepth(8, MapOwner::Library) == 8);      // back to CIE 1931 keeps 8; the user raises it
    CHECK(clampDepth(11, MapOwner::Library) == 11);
    CHECK(clampDepth(0, MapOwner::Library) == 1);
    CHECK(clampDepth(0, MapOwner::MoonLight) == 1);
    CHECK(clampDepth(200, MapOwner::Library) == 11);
}

TEST_CASE("Hub75Color: the dither range is the library's 0 to 2") {
    CHECK(kDitherMax == 2);
}
