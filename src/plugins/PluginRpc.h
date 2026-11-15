#pragma once
#include "PluginWire.h"
#include "cyaudio/plugins/ClapExtensions.h"
#include "cyaudio/plugins/PluginWorker.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>
namespace cyaudio::detail::rpc {
struct Invalid {};
inline constexpr std::size_t maximumBytes = 64 * 1024 * 1024;
template <class A> void fields(A& a, PluginDescriptor& v) {
    a(v.format, v.id, v.name, v.vendor, v.version, v.description);
}
template <class A> void fields(A& a, const PluginDescriptor& v) {
    a(v.format, v.id, v.name, v.vendor, v.version, v.description);
}
template <class A> void fields(A& a, PluginAudioPort& v) {
    a(v.id, v.channels, v.inPlacePair, v.name, v.type, v.main, v.supports64, v.prefers64,
      v.commonSampleSize, v.surroundMap, v.ambisonicKnown, v.ambisonicOrdering,
      v.ambisonicNormalization);
}
template <class A> void fields(A& a, const PluginAudioPort& v) {
    a(v.id, v.channels, v.inPlacePair, v.name, v.type, v.main, v.supports64, v.prefers64,
      v.commonSampleSize, v.surroundMap, v.ambisonicKnown, v.ambisonicOrdering,
      v.ambisonicNormalization);
}
template <class A> void fields(A& a, PluginNotePort& v) {
    a(v.id, v.dialects, v.preferredDialect, v.name);
}
template <class A> void fields(A& a, const PluginNotePort& v) {
    a(v.id, v.dialects, v.preferredDialect, v.name);
}
template <class A> void fields(A& a, PluginParameter& v) {
    a(v.id, v.name, v.module, v.minimum, v.maximum, v.defaultValue, v.stepped, v.readOnly,
      v.automatable, v.modulatable, v.hidden, v.normalized, v.automationTargets,
      v.modulationTargets, v.units, v.periodic, v.bypass, v.requiresProcess, v.enumeration);
}
template <class A> void fields(A& a, const PluginParameter& v) {
    a(v.id, v.name, v.module, v.minimum, v.maximum, v.defaultValue, v.stepped, v.readOnly,
      v.automatable, v.modulatable, v.hidden, v.normalized, v.automationTargets,
      v.modulationTargets, v.units, v.periodic, v.bypass, v.requiresProcess, v.enumeration);
}
template <class A> void fields(A& a, PluginDescription& v) {
    a(v.plugin, v.inputs, v.outputs, v.noteInputs, v.noteOutputs, v.parameters, v.state, v.editor,
      v.float32, v.float64);
}
template <class A> void fields(A& a, const PluginDescription& v) {
    a(v.plugin, v.inputs, v.outputs, v.noteInputs, v.noteOutputs, v.parameters, v.state, v.editor,
      v.float32, v.float64);
}
template <class A> void fields(A& a, PluginPrepareOptions& v) {
    a(v.sampleRate, v.minimumFrames, v.maximumFrames, v.maximumEvents, v.useFloat64, v.offline,
      v.sleep.mode, v.sleep.quietFrames, v.sleep.silenceThreshold);
}
template <class A> void fields(A& a, const PluginPrepareOptions& v) {
    a(v.sampleRate, v.minimumFrames, v.maximumFrames, v.maximumEvents, v.useFloat64, v.offline,
      v.sleep.mode, v.sleep.quietFrames, v.sleep.silenceThreshold);
}
template <class A> void fields(A& a, ClapColor& v) {
    a(v.alpha, v.red, v.green, v.blue);
}
template <class A> void fields(A& a, const ClapColor& v) {
    a(v.alpha, v.red, v.green, v.blue);
}
template <class A> void fields(A& a, ClapTrackInfo& v) {
    a(v.name, v.color, v.channels, v.portType, v.returnTrack, v.bus, v.master);
}
template <class A> void fields(A& a, const ClapTrackInfo& v) {
    a(v.name, v.color, v.channels, v.portType, v.returnTrack, v.bus, v.master);
}
template <class A> void fields(A& a, ClapVoiceInfo& v) {
    a(v.count, v.capacity, v.overlappingNotes);
}
template <class A> void fields(A& a, const ClapVoiceInfo& v) {
    a(v.count, v.capacity, v.overlappingNotes);
}
template <class A> void fields(A& a, ClapNoteName& v) {
    a(v.name, v.port, v.key, v.channel);
}
template <class A> void fields(A& a, const ClapNoteName& v) {
    a(v.name, v.port, v.key, v.channel);
}
template <class A> void fields(A& a, ClapRemoteControlsPage& v) {
    a(v.id, v.section, v.name, v.parameters, v.presetSpecific);
}
template <class A> void fields(A& a, const ClapRemoteControlsPage& v) {
    a(v.id, v.section, v.name, v.parameters, v.presetSpecific);
}
template <class A> void fields(A& a, ClapAudioConfiguration& v) {
    a(v.id, v.inputCount, v.outputCount, v.name, v.mainInputChannels, v.mainOutputChannels,
      v.mainInputType, v.mainOutputType, v.current, v.detailedPortsKnown, v.inputs, v.outputs);
}
template <class A> void fields(A& a, const ClapAudioConfiguration& v) {
    a(v.id, v.inputCount, v.outputCount, v.name, v.mainInputChannels, v.mainOutputChannels,
      v.mainInputType, v.mainOutputType, v.current, v.detailedPortsKnown, v.inputs, v.outputs);
}
template <class A> void fields(A& a, ClapAudioPortRequest& v) {
    a(v.input, v.index, v.channels, v.type, v.surroundMap, v.ambisonicOrdering,
      v.ambisonicNormalization);
}
template <class A> void fields(A& a, const ClapAudioPortRequest& v) {
    a(v.input, v.index, v.channels, v.type, v.surroundMap, v.ambisonicOrdering,
      v.ambisonicNormalization);
}
template <class A> void fields(A& a, ClapMenuTarget& v) {
    a(v.parameter, v.id);
}
template <class A> void fields(A& a, const ClapMenuTarget& v) {
    a(v.parameter, v.id);
}
template <class A> void fields(A& a, ClapMenuItem& v) {
    a(v.kind, v.label, v.action, v.enabled, v.checked);
}
template <class A> void fields(A& a, const ClapMenuItem& v) {
    a(v.kind, v.label, v.action, v.enabled, v.checked);
}
template <class A> void fields(A& a, ClapGuiHints& v) {
    a(v.horizontal, v.vertical, v.preserveAspect, v.aspectWidth, v.aspectHeight);
}
template <class A> void fields(A& a, const ClapGuiHints& v) {
    a(v.horizontal, v.vertical, v.preserveAspect, v.aspectWidth, v.aspectHeight);
}
template <class A> void fields(A& a, ClapCapabilities& v) {
    a(v.audioConfigurations, v.configurationDetails, v.configurablePorts, v.portActivation,
      v.livePortActivation, v.surround, v.ambisonic, v.stateContext, v.presetLoad, v.remoteControls,
      v.noteNames, v.voiceInfo, v.parameterIndication, v.contextMenu, v.hardRealtime);
}
template <class A> void fields(A& a, const ClapCapabilities& v) {
    a(v.audioConfigurations, v.configurationDetails, v.configurablePorts, v.portActivation,
      v.livePortActivation, v.surround, v.ambisonic, v.stateContext, v.presetLoad, v.remoteControls,
      v.noteNames, v.voiceInfo, v.parameterIndication, v.contextMenu, v.hardRealtime);
}
template <class A> void fields(A& a, ClapPresetLocation& v) {
    a(v.kind, v.location, v.loadKey);
}
template <class A> void fields(A& a, const ClapPresetLocation& v) {
    a(v.kind, v.location, v.loadKey);
}
template <class A> void fields(A& a, ClapPresetProviderInfo& v) {
    a(v.id, v.name, v.vendor);
}
template <class A> void fields(A& a, const ClapPresetProviderInfo& v) {
    a(v.id, v.name, v.vendor);
}
template <class A> void fields(A& a, ClapPresetFileType& v) {
    a(v.name, v.description, v.extension);
}
template <class A> void fields(A& a, const ClapPresetFileType& v) {
    a(v.name, v.description, v.extension);
}
template <class A> void fields(A& a, ClapPresetSearchLocation& v) {
    a(v.flags, v.name, v.kind, v.location);
}
template <class A> void fields(A& a, const ClapPresetSearchLocation& v) {
    a(v.flags, v.name, v.kind, v.location);
}
template <class A> void fields(A& a, ClapSoundpack& v) {
    a(v.flags, v.id, v.name, v.description, v.homepage, v.vendor, v.imagePath, v.releaseTime);
}
template <class A> void fields(A& a, const ClapSoundpack& v) {
    a(v.flags, v.id, v.name, v.description, v.homepage, v.vendor, v.imagePath, v.releaseTime);
}
template <class A> void fields(A& a, ClapPresetCatalogue& v) {
    a(v.fileTypes, v.locations, v.soundpacks);
}
template <class A> void fields(A& a, const ClapPresetCatalogue& v) {
    a(v.fileTypes, v.locations, v.soundpacks);
}
template <class A> void fields(A& a, ClapPresetMetadata& v) {
    a(v.name, v.loadKey, v.soundpackId, v.description, v.flags, v.flagsKnown, v.creationTime,
      v.modificationTime, v.pluginIds, v.extra, v.creators, v.features);
}
template <class A> void fields(A& a, const ClapPresetMetadata& v) {
    a(v.name, v.loadKey, v.soundpackId, v.description, v.flags, v.flagsKnown, v.creationTime,
      v.modificationTime, v.pluginIds, v.extra, v.creators, v.features);
}
struct Writer {
    std::vector<std::uint8_t> bytes;
    template <class... T> void operator()(const T&... v) {
        (one(v), ...);
    }
    void raw(const void* p, std::size_t n) {
        if (n > maximumBytes - bytes.size())
            throw Invalid{};
        if (n) {
            const auto* b = static_cast<const std::uint8_t*>(p);
            bytes.insert(bytes.end(), b, b + n);
        }
    }
    template <class T> void one(const T& v) {
        if constexpr (std::is_same_v<T, bool>) {
            const std::uint8_t b = v ? 1 : 0;
            raw(&b, 1);
        } else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>)
            raw(&v, sizeof(v));
        else
            fields(*this, v);
    }
    void one(const std::string& v) {
        if (v.size() > 65535 || v.find(char{}) != std::string::npos)
            throw Invalid{};
        one(std::uint32_t(v.size()));
        raw(v.data(), v.size());
    }
    void one(const std::vector<std::uint8_t>& v) {
        if (v.size() > maximumBytes)
            throw Invalid{};
        one(std::uint32_t(v.size()));
        raw(v.data(), v.size());
    }
    template <class T> void one(const std::vector<T>& v) {
        if (v.size() > 65536)
            throw Invalid{};
        one(std::uint32_t(v.size()));
        for (const auto& x : v)
            one(x);
    }
    template <class T, std::size_t N> void one(const std::array<T, N>& v) {
        for (const auto& x : v)
            one(x);
    }
    template <class T> void one(const std::optional<T>& v) {
        one(bool(v));
        if (v)
            one(*v);
    }
    template <class A, class B> void one(const std::pair<A, B>& v) {
        one(v.first);
        one(v.second);
    }
};
struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t at = 0, allocated = 0;
    explicit Reader(std::span<const std::uint8_t> b) : bytes(b) {
        if (b.size() > maximumBytes)
            throw Invalid{};
    }
    template <class... T> void operator()(T&... v) {
        (one(v), ...);
    }
    void raw(void* p, std::size_t n) {
        if (n > bytes.size() - at)
            throw Invalid{};
        if (n)
            std::memcpy(p, bytes.data() + at, n);
        at += n;
    }
    void allocate(std::size_t n) {
        if (n > maximumBytes - allocated)
            throw Invalid{};
        allocated += n;
    }
    void done() {
        if (at != bytes.size())
            throw Invalid{};
    }
    template <class T> void one(T& v) {
        if constexpr (std::is_same_v<T, bool>) {
            std::uint8_t b;
            raw(&b, 1);
            if (b > 1)
                throw Invalid{};
            v = b != 0;
        } else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>)
            raw(&v, sizeof(v));
        else
            fields(*this, v);
    }
    void one(std::string& v) {
        std::uint32_t n;
        one(n);
        if (n > 65535 || n > bytes.size() - at)
            throw Invalid{};
        allocate(n);
        v.assign(reinterpret_cast<const char*>(bytes.data() + at), n);
        at += n;
        if (v.find(char{}) != std::string::npos)
            throw Invalid{};
    }
    void one(std::vector<std::uint8_t>& v) {
        std::uint32_t n;
        one(n);
        if (n > bytes.size() - at)
            throw Invalid{};
        allocate(n);
        v.assign(bytes.begin() + at, bytes.begin() + at + n);
        at += n;
    }
    template <class T> void one(std::vector<T>& v) {
        std::uint32_t n;
        one(n);
        if (n > 65536 || n > bytes.size() - at)
            throw Invalid{};
        allocate(std::size_t(n) * sizeof(T));
        v.resize(n);
        for (auto& x : v)
            one(x);
    }
    template <class T, std::size_t N> void one(std::array<T, N>& v) {
        for (auto& x : v)
            one(x);
    }
    template <class T> void one(std::optional<T>& v) {
        bool b;
        one(b);
        if (b) {
            v.emplace();
            one(*v);
        } else
            v.reset();
    }
    template <class A, class B> void one(std::pair<A, B>& v) {
        one(v.first);
        one(v.second);
    }
};
enum class Op : std::uint32_t {
    scan,
    describe,
    activate,
    deactivate,
    refresh,
    parameterValue,
    parameterText,
    flush,
    save,
    load,
    capabilities,
    configurations,
    selectConfiguration,
    canConfigure,
    configure,
    portActive,
    surround,
    ambisonic,
    voices,
    noteNames,
    remotes,
    track,
    fromText,
    mapping,
    automation,
    saveContext,
    loadContext,
    preset,
    menu,
    menuAction,
    hints,
    preferredApi,
    title,
    pump,
    presetProviders,
    presetOpen,
    presetMetadata
};
template <class A> void fields(A& a, wire::Transport& v) {
    a(v.songFrame, v.beats, v.seconds, v.tempo, v.tempoIncrement, v.loopStartBeats, v.loopEndBeats,
      v.loopStartSeconds, v.loopEndSeconds, v.barStart, v.barNumber, v.numerator, v.denominator,
      v.flags);
}
template <class A> void fields(A& a, const wire::Transport& v) {
    a(v.songFrame, v.beats, v.seconds, v.tempo, v.tempoIncrement, v.loopStartBeats, v.loopEndBeats,
      v.loopStartSeconds, v.loopEndSeconds, v.barStart, v.barNumber, v.numerator, v.denominator,
      v.flags);
}
template <class A> void fields(A& a, wire::Event& v) {
    a(v.kind, v.frame, v.id, v.flags, v.noteId, v.port, v.channel, v.key, v.value, v.words,
      v.wordCount, v.offset, v.size, v.transport);
}
template <class A> void fields(A& a, const wire::Event& v) {
    a(v.kind, v.frame, v.id, v.flags, v.noteId, v.port, v.channel, v.key, v.value, v.words,
      v.wordCount, v.offset, v.size, v.transport);
}
struct Events {
    std::vector<wire::Event> records;
    std::vector<std::uint8_t> payload;
};
template <class A> void fields(A& a, Events& v) {
    a(v.records, v.payload);
}
template <class A> void fields(A& a, const Events& v) {
    a(v.records, v.payload);
}
inline Events encodeEvents(const PluginEventBuffer& events) {
    Events e;
    e.records.resize(events.size());
    e.payload.resize(65536);
    std::uint32_t count = 0, bytes = 0;
    if (wire::writeEvents(&events, e.records.data(), e.payload.data(), 65536, 65536, count,
                          bytes) != Status::ok)
        throw Invalid{};
    e.payload.resize(bytes);
    return e;
}
inline Status decodeEvents(const Events& e, PluginEventBuffer& out) {
    auto r = out.prepare(65536, 65536);
    if (r != Status::ok)
        return r;
    return wire::readEvents(e.records.data(), e.payload.data(), std::uint32_t(e.records.size()),
                            std::uint32_t(e.payload.size()), 65536, 65536, 65536, out);
}
struct HostSnapshot {
    std::string name = "CyAudioCore", vendor = "MrMybal CyberAlien",
                url = "https://github.com/MrMybal/CyAudioCore", version = "0.1.0";
    std::optional<ClapTrackInfo> track;
    bool threadPool = false, populateMenu = false, performMenu = false, popupMenu = false;
};
template <class A> void fields(A& a, HostSnapshot& v) {
    a(v.name, v.vendor, v.url, v.version, v.track, v.threadPool, v.populateMenu, v.performMenu,
      v.popupMenu);
}
template <class A> void fields(A& a, const HostSnapshot& v) {
    a(v.name, v.vendor, v.url, v.version, v.track, v.threadPool, v.populateMenu, v.performMenu,
      v.popupMenu);
}
enum class NoticeKind : std::uint32_t { log, rescan, clear, suggest, loaded, error, edited };
struct Notice {
    NoticeKind kind{};
    std::uint32_t a = 0, b = 0;
    std::int32_t code = 0;
    double value = 0;
    PluginEventKind eventKind{};
    std::string text;
    ClapPresetLocation location;
};
template <class A> void fields(A& a, Notice& v) {
    a(v.kind, v.a, v.b, v.code, v.value, v.eventKind, v.text, v.location);
}
template <class A> void fields(A& a, const Notice& v) {
    a(v.kind, v.a, v.b, v.code, v.value, v.eventKind, v.text, v.location);
}
} // namespace cyaudio::detail::rpc
namespace cyaudio::detail {
struct PluginRemoteAccess {
    static Status launch(PluginWorker&, const PluginIsolationOptions&, PluginFormat,
                         const std::filesystem::path&, std::string_view, std::uint32_t,
                         const rpc::HostSnapshot& = {}, const PluginHostCallbacks* = nullptr,
                         const ClapHostServices* = nullptr) noexcept;
    static Status request(PluginWorker&, std::span<const std::uint8_t>,
                          std::vector<std::uint8_t>&) noexcept;
    static Status callbacks(PluginWorker&, const PluginHostCallbacks&,
                            const ClapHostServices&) noexcept;
    static Status shape(PluginWorker&, const PluginDescription&) noexcept;
    static Status flushAudio(PluginWorker&, const PluginEventBuffer&, PluginEventBuffer&) noexcept;
    static Status preparation(PluginWorker&, const PluginPrepareOptions&) noexcept;
    static Status audioCommand(PluginWorker&, std::uint32_t) noexcept;
    static Status audioPort(PluginWorker&, bool, std::uint32_t, bool, std::uint32_t) noexcept;
    static ClapProcessStatus processStatus(const PluginWorker&) noexcept;
    static Status presetProviders(const std::filesystem::path&,
                                  std::vector<ClapPresetProviderInfo>&,
                                  const PluginIsolationOptions&) noexcept;
};
} // namespace cyaudio::detail
