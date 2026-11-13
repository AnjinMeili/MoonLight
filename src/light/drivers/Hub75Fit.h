#pragma once

#include "light/drivers/Hub75Canvas.h"

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace mm {

/// Whether a layout's panels cable into chains of the card's `chain` on a mapping that drives `maxChains` outputs.
enum class GeometryFit : uint8_t { Ok, Empty, Indivisible, TooManyChains };

/// How `panels` cable into chains of `chain` on `maxChains` outputs: none, unevenly, past the outputs, or fine.
inline GeometryFit checkGeometry(uint32_t panels, uint16_t chain, uint8_t maxChains) {
    if (panels == 0 || chain == 0) return GeometryFit::Empty;
    if (panels % chain != 0) return GeometryFit::Indivisible;
    if (panels / chain > maxChains) return GeometryFit::TooManyChains;
    return GeometryFit::Ok;
}

/// A status buffer this size holds every `geometryMessage` whole for a mapping of six chains at most; the unnamed-mapping fallback's longest is cut safely.
constexpr size_t kGeometryMessageMax = 512;

/// A card's geometry as the messages need it: one panel, the chain, the outputs the mapping has, and the mapping's name.
struct CardGeometry {
    PanelSpec panel;                 ///< one panel and how it is clocked
    uint16_t chain = 1;              ///< the card's `chain`
    uint8_t maxChains = 1;           ///< the outputs the mapping drives
    const char* mapping = nullptr;   ///< the mapping's name, for the text
};

namespace detail {

/// Text appended into a caller's buffer. It counts what it needed past the end, as snprintf does, so a cut shows in the count.
struct Text {
    char* out;      ///< the caller's buffer
    size_t cap;     ///< its size
    size_t n = 0;   ///< the length written so far, or needed once past the end

    /// Append `fmt` with its arguments, as printf formats them.
    void add(const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        const bool room = out && n < cap;
        const int w = std::vsnprintf(room ? out + n : nullptr, room ? cap - n : 0, fmt, ap);
        va_end(ap);
        if (w > 0) n += static_cast<size_t>(w);
    }
};

inline const char* plural(uint32_t n, const char* one, const char* many) { return n == 1 ? one : many; }

/// `3 chains of 2 (256x192, 243 Hz)`.
inline void addArrangement(Text& t, const Arrangement& a) {
    t.add("%u chain%s of %u (%ux%u, %u Hz)", static_cast<unsigned>(a.parallel), plural(a.parallel, "", "s"), static_cast<unsigned>(a.chain),
          static_cast<unsigned>(a.width), static_cast<unsigned>(a.height), static_cast<unsigned>(a.hz));
}

/// The first sentence: what is wrong with the panels as the card has them.
inline void addReason(Text& t, GeometryFit why, uint32_t panels, const CardGeometry& g) {
    const char* const word = plural(panels, "panel", "panels");
    if (why == GeometryFit::Empty) {
        t.add("The layout holds fewer lights than one %ux%u panel.", static_cast<unsigned>(g.panel.cols), static_cast<unsigned>(g.panel.rows));
    } else if (why == GeometryFit::TooManyChains) {
        t.add("%u %s in chains of %u need %u chains, but the %s mapping drives at most %u.", static_cast<unsigned>(panels), word,
              static_cast<unsigned>(g.chain), static_cast<unsigned>(panels / g.chain), g.mapping ? g.mapping : "chosen",
              static_cast<unsigned>(g.maxChains));
    } else {
        t.add("%u %s cannot be cabled in chains of %u.", static_cast<unsigned>(panels), word, static_cast<unsigned>(g.chain));
    }
}

/// Every arrangement that uses all the panels, widest first; none when every one would chain more than kChainMax.
inline void addArrangements(Text& t, uint32_t panels, const CardGeometry& g) {
    Arrangement all[8];   // a mapping drives six chains at most
    const size_t count = panelArrangements(panels, g.panel, g.maxChains, all, 8);
    if (count == 0) return;
    t.add(" Cable all %u: ", static_cast<unsigned>(panels));
    for (size_t i = 0; i < count; i++) {
        if (i) t.add("; ");
        addArrangement(t, all[i]);
    }
    t.add(".");
}

/// What keeps the chains already built: use fewer panels, or add some.
inline void addKeep(Text& t, uint32_t panels, const CardGeometry& g) {
    const KeepChains keep = keepChains(panels, g.panel, g.chain, g.maxChains);
    const bool hasUse = keep.use.chain != 0;
    const bool hasAdd = keep.add.chain != 0;
    if (!hasUse && !hasAdd) return;
    t.add(" Keep chains of %u: ", static_cast<unsigned>(g.chain));
    if (hasUse) {
        const uint32_t used = static_cast<uint32_t>(keep.use.chain) * keep.use.parallel;
        t.add("use %u %s, %u spare, as %u chain%s (%ux%u, %u Hz)", static_cast<unsigned>(used), plural(used, "panel", "panels"),
              static_cast<unsigned>(keep.spare), static_cast<unsigned>(keep.use.parallel), plural(keep.use.parallel, "", "s"),
              static_cast<unsigned>(keep.use.width), static_cast<unsigned>(keep.use.height), static_cast<unsigned>(keep.use.hz));
    }
    if (hasUse && hasAdd) t.add(", or ");
    if (hasAdd) {
        t.add("add %u %s for %u chain%s (%ux%u, %u Hz)", static_cast<unsigned>(keep.added), plural(keep.added, "panel", "panels"),
              static_cast<unsigned>(keep.add.parallel), plural(keep.add.parallel, "", "s"), static_cast<unsigned>(keep.add.width),
              static_cast<unsigned>(keep.add.height), static_cast<unsigned>(keep.add.hz));
    }
    t.add(".");
}

} // namespace detail

/// What is wrong with a geometry that does not fit, every arrangement using all the panels, then how to keep the chains; the length needed, snprintf style.
inline size_t geometryMessage(GeometryFit why, uint32_t panels, const CardGeometry& g, char* out, size_t cap) {
    detail::Text t{out, cap};
    if (out && cap) out[0] = 0;
    if (why == GeometryFit::Ok) return 0;
    detail::addReason(t, why, panels, g);
    if (why == GeometryFit::Empty) return t.n;
    detail::addArrangements(t, panels, g);
    detail::addKeep(t, panels, g);
    return t.n;
}

} // namespace mm
