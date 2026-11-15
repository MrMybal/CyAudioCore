#pragma once
#include "cyaudio/plugins/Plugin.h"
#include <algorithm>
#include <cmath>
namespace cyaudio::detail {
inline bool validSleepOptions(const PluginSleepOptions& o) noexcept {
    return unsigned(o.mode) <= unsigned(PluginSleepMode::whenQuiet) && o.quietFrames &&
           isFinite(o.silenceThreshold) && o.silenceThreshold >= 0 && o.silenceThreshold <= 1;
}
// A predicate, not an energy calculation. NaN/Inf always count as activity.
template <class Blocks>
bool quietPluginAudio(const Blocks& blocks, std::uint32_t frames, double threshold) noexcept {
    for (const auto& bus : blocks)
        for (std::size_t ch = 0; ch < bus.channels(); ++ch)
            for (const auto sample : bus.channel(ch).first(frames))
                if (!(sample >= -threshold && sample <= threshold))
                    return false;
    return true;
}
struct PluginTransportHistory {
    PluginTransport previous;
    std::uint32_t frames = 0;
    bool present = false, initialized = false;
    bool update(const PluginTransport* t, std::uint32_t count, double rate) noexcept {
        bool changed = initialized && present != bool(t);
        if (initialized && present && t) {
            const auto& p = previous;
            changed =
                changed || t->hasSongFrame != p.hasSongFrame || t->hasBeats != p.hasBeats ||
                t->hasSeconds != p.hasSeconds || t->hasTempo != p.hasTempo ||
                t->hasSignature != p.hasSignature || t->playing != p.playing ||
                t->recording != p.recording || t->looping != p.looping || t->preRoll != p.preRoll ||
                (t->hasTempo && (t->tempo != p.tempo || t->tempoIncrement != p.tempoIncrement)) ||
                (t->hasSignature &&
                 (t->numerator != p.numerator || t->denominator != p.denominator)) ||
                t->loopStartBeats != p.loopStartBeats || t->loopEndBeats != p.loopEndBeats ||
                t->loopStartSeconds != p.loopStartSeconds || t->loopEndSeconds != p.loopEndSeconds;
            const auto advance = p.playing ? double(frames) : 0.;
            if (t->hasSongFrame) {
                const auto offset = p.playing ? std::int64_t(frames) : 0;
                changed = changed ||
                          p.songFrame > std::numeric_limits<std::int64_t>::max() - offset ||
                          t->songFrame != p.songFrame + offset;
            }
            if (t->hasSeconds)
                changed = changed || std::abs(t->seconds - p.seconds - advance / rate) > 1e-7;
            if (t->hasBeats && p.hasTempo) {
                const auto beats =
                    (advance * p.tempo + advance * (advance - 1) * .5 * p.tempoIncrement) /
                    (60 * rate);
                changed = changed || std::abs(t->beats - p.beats - beats) > 1e-7;
            } else if (t->hasBeats)
                changed = changed || t->beats != p.beats;
        }
        initialized = true;
        present = bool(t);
        if (t)
            previous = *t;
        frames = count;
        return changed;
    }
};
} // namespace cyaudio::detail
