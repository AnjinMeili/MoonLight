// Ableton Link on a desktop host: join the session on the network and read its beat, through Ableton's own library.
#include "platform/platform.h"

#ifdef MM_HAS_LINK
#include <ableton/Link.hpp>

#include <algorithm>
#include <memory>

namespace mm::platform {

namespace {
/// The one Link instance, joined while the audio service's `link` is on; Ableton's library runs its own network thread.
std::unique_ptr<ableton::Link> gLink;
/// The tempo a new instance offers a session it starts alone; a session already running keeps its own.
constexpr double kOfferedBpm = 120.0;
}   // namespace

void linkEnable(bool on) {
    if (on && !gLink) gLink = std::make_unique<ableton::Link>(kOfferedBpm);
    if (!gLink) return;
    gLink->enable(on);
    if (!on) gLink.reset();
}

// Clang gains the effect-warning suppression only where it has the warning, as platform_host.cpp probes it.
#if defined(__clang__) && defined(__has_warning)
#  if __has_warning("-Wfunction-effects")
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wfunction-effects"
#    define MM_LINK_EFFECTS_PUSHED 1
#  endif
#endif
// The audio-thread capture, lock-free and real-time safe as Link documents it; the audio service reads it from its tick alone, one thread.
bool linkRead(LinkBeat& out, float quantum) MM_NONBLOCKING {
    if (!gLink || !gLink->isEnabled()) return false;
    const auto now = gLink->clock().micros();
    const auto state = gLink->captureAudioSessionState();
    out.bpm = static_cast<float>(state.tempo());
    out.beat = state.beatAtTime(now, static_cast<double>(quantum));
    out.phase = static_cast<float>(state.phaseAtTime(now, static_cast<double>(quantum)));
    out.peers = static_cast<uint8_t>(std::min<std::size_t>(255, gLink->numPeers()));
    return true;
}
#ifdef MM_LINK_EFFECTS_PUSHED
#  pragma clang diagnostic pop
#endif

}   // namespace mm::platform

#endif   // MM_HAS_LINK
