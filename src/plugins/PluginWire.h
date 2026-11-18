#pragma once
#include "PluginInternal.h"
#include "cyaudio/core/DspTypes.h"
#include "cyaudio/plugins/PluginWorker.h"
#include <array>
#include <atomic>
#include <cstring>
namespace cyaudio::detail::wire {
inline constexpr std::uint64_t magic = 0x3150434950534443ull;
// Version 4 adds prepared sleep policy, wake generations and DSP sleep counters
// to isolated hosting, reverse UI calls and split-phase audio.
// Mismatched client/worker binaries refuse to pair.
inline constexpr std::uint32_t version = 4;
enum class Command : std::uint32_t {
    save = 1,
    load,
    openEditor,
    resizeEditor,
    closeEditor,
    scaleEditor,
    showEditor,
    focusEditor,
    keyEditor,
    wheelEditor,
    restart,
    remote
};
struct Transport {
    std::int64_t songFrame = 0;
    double beats = 0, seconds = 0, tempo = 120, tempoIncrement = 0;
    double loopStartBeats = 0, loopEndBeats = 0, loopStartSeconds = 0, loopEndSeconds = 0,
           barStart = 0;
    std::int32_t barNumber = 0;
    std::uint32_t numerator = 4, denominator = 4, flags = 0;
};
inline Transport encode(const PluginTransport& t) noexcept {
    return {t.songFrame,
            t.beats,
            t.seconds,
            t.tempo,
            t.tempoIncrement,
            t.loopStartBeats,
            t.loopEndBeats,
            t.loopStartSeconds,
            t.loopEndSeconds,
            t.barStart,
            t.barNumber,
            t.numerator,
            t.denominator,
            (t.hasSongFrame ? 1u : 0u) | (t.hasBeats ? 2u : 0u) | (t.hasSeconds ? 4u : 0u) |
                (t.hasTempo ? 8u : 0u) | (t.hasSignature ? 16u : 0u) | (t.playing ? 32u : 0u) |
                (t.recording ? 64u : 0u) | (t.looping ? 128u : 0u) | (t.preRoll ? 256u : 0u)};
}
inline PluginTransport decode(const Transport& t) noexcept {
    PluginTransport out;
    out.songFrame = t.songFrame;
    out.beats = t.beats;
    out.seconds = t.seconds;
    out.tempo = t.tempo;
    out.tempoIncrement = t.tempoIncrement;
    out.loopStartBeats = t.loopStartBeats;
    out.loopEndBeats = t.loopEndBeats;
    out.loopStartSeconds = t.loopStartSeconds;
    out.loopEndSeconds = t.loopEndSeconds;
    out.barStart = t.barStart;
    out.barNumber = t.barNumber;
    out.numerator = std::uint16_t(t.numerator);
    out.denominator = std::uint16_t(t.denominator);
    out.hasSongFrame = t.flags & 1;
    out.hasBeats = t.flags & 2;
    out.hasSeconds = t.flags & 4;
    out.hasTempo = t.flags & 8;
    out.hasSignature = t.flags & 16;
    out.playing = t.flags & 32;
    out.recording = t.flags & 64;
    out.looping = t.flags & 128;
    out.preRoll = t.flags & 256;
    return out;
}
struct Event {
    std::uint32_t kind = 0, frame = 0, id = 0, flags = 0;
    std::int32_t noteId = -1, port = -1, channel = -1, key = -1;
    double value = 0;
    std::array<std::uint32_t, 4> words{};
    std::uint32_t wordCount = 0, offset = 0, size = 0;
    Transport transport;
};
struct Header {
    std::uint64_t magicValue = magic, mappingBytes = 0;
    std::uint32_t protocol = version, wordBits = sizeof(void*) * 8, headerBytes = sizeof(Header),
                  eventBytes = sizeof(Event);
    std::uint32_t format = 0, minimumFrames = 1, frames = 512, events = 1024, channels = 64,
                  payload = 65536, stateBytes = 16 * 1024 * 1024, flags = 0, parentPid = 0;
    double sampleRate = 48000;
    std::uint32_t sleepMode = 0, quietFrames = 2048;
    double silenceThreshold = 0;
    std::array<char, 32768> pluginPath{};
    std::array<char, 4096> pluginId{};
    std::array<std::byte, 128> bootSeparation{};
    std::atomic<std::uint32_t> boot{0}, shutdown{0}, failure{0}, requests{0}, heartbeat{0};
    // Plugin timing, published by the worker main thread whenever it changes. The
    // sequence is odd while the three values are being written (seqlock).
    std::atomic<std::uint32_t> timingSequence{0}, latency{0}, tail{0}, timingFlags{0};
    std::uint32_t inputCount = 0, outputCount = 0;
    std::array<std::uint32_t, 64> inputChannels{}, outputChannels{};
    std::array<std::byte, 128> audioSeparation{};
    std::atomic<std::uint32_t> audioState{0};
    // audioSleeping is set by the worker audio thread before it waits and makes the
    // client issue its non-blocking wake. audioWake/controlWake are the wake words
    // (futex words where the platform has no named event).
    std::atomic<std::uint32_t> audioSleeping{0}, audioWake{0};
    std::uint64_t sequence = 0;
    std::int64_t steadyFrame = -1;
    std::uint32_t inputEvents = 0, inputBytes = 0, outputEvents = 0, outputBytes = 0,
                  audioStatus = 0, hasTransport = 0;
    Transport transport;
    std::uint32_t blockFrames = 0, audioOperation = 0;
    std::atomic<std::uint32_t> clientAudioCommand{0}, clientReset{0}, clientAudioStatus{0};
    std::atomic<std::uint32_t> clapProcessStatus{1};
    std::atomic<std::uint32_t> pluginSleeping{0}, wakeGeneration{0}, sleepGeneration{0},
        processedBlocks{0}, skippedBlocks{0};
    std::uint32_t audioPortInput = 0, audioPortIndex = 0, audioPortActive = 0, audioPortBits = 0;
    std::atomic<std::uint32_t> audioPortState{0}, audioPortStatus{0};
    std::uint32_t bootMode = 0, initialBytes = 0, reverseTimeoutMs = 2000;
    std::array<std::byte, 128> reverseSeparation{};
    std::atomic<std::uint32_t> reverseState{0};
    std::uint32_t reverseBytes = 0, reverseStatus = 0;
    std::array<std::uint8_t, 65536> reversePayload{};
    std::array<std::byte, 128> controlSeparation{};
    std::atomic<std::uint32_t> controlState{0}, controlWake{0};
    std::uint32_t command = 0, controlStatus = 0, controlBytes = 0, windowApi = 0, floating = 0,
                  width = 0, height = 0;
    std::uint64_t windowHandle = 0;
    double scale = 1;
    std::uint32_t keyCharacter = 0, keyCode = 0, keyModifiers = 0, keyDown = 1, boolean = 0;
    float wheel = 0;
    std::atomic<std::uint32_t> requestedWidth{0}, requestedHeight{0};
};
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t));
// Dimensions fixed at launch. The mapping is writable by the other process, so each
// side takes one private copy and never re-reads the header for a bound afterwards.
struct Limits {
    std::uint32_t frames = 0, events = 0, channels = 0, payload = 0, stateBytes = 0, flags = 0;
};
inline Limits limits(const Header& h) noexcept {
    return {h.frames, h.events, h.channels, h.payload, h.stateBytes, h.flags};
}
struct Layout {
    std::size_t input = 0, output = 0, inputEvents = 0, outputEvents = 0, inputPayload = 0,
                outputPayload = 0, control = 0, total = 0;
};
inline Layout layout(const Header& h) noexcept {
    Layout l;
    std::size_t at = sizeof(Header);
    const auto reserve = [&](std::size_t bytes) {
        at = (at + 63) & ~std::size_t(63);
        const auto result = at;
        at += bytes;
        return result;
    };
    const auto audio =
        std::size_t(h.channels) * h.frames * ((h.flags & 1) ? sizeof(double) : sizeof(float));
    l.input = reserve(audio);
    l.output = reserve(audio);
    l.inputEvents = reserve(std::size_t(h.events) * sizeof(Event));
    l.outputEvents = reserve(std::size_t(h.events) * sizeof(Event));
    l.inputPayload = reserve(h.payload);
    l.outputPayload = reserve(h.payload);
    l.control = reserve(h.stateBytes);
    l.total = at;
    return l;
}
inline bool valid(const Header& h) noexcept {
    if (h.magicValue != magic || h.protocol != version || h.wordBits != sizeof(void*) * 8 ||
        h.headerBytes != sizeof(Header) || h.eventBytes != sizeof(Event) ||
        h.format > std::uint32_t(PluginFormat::custom) || !isFinite(h.sampleRate) ||
        h.sampleRate <= 0 || h.sampleRate > 1000000 || !h.minimumFrames ||
        h.minimumFrames > h.frames || !h.frames || h.frames > 65536 || !h.events ||
        h.events > 65536 || !h.channels || h.channels > 256 || h.payload > 16 * 1024 * 1024 ||
        !h.stateBytes || h.stateBytes > 64 * 1024 * 1024 || h.flags > 3 || h.bootMode > 3 ||
        h.sleepMode > 2 || !h.quietFrames || !isFinite(h.silenceThreshold) ||
        h.silenceThreshold < 0 || h.silenceThreshold > 1 || h.initialBytes > h.stateBytes ||
        !h.reverseTimeoutMs || h.reverseTimeoutMs > 60000)
        return false;
    return layout(h).total == h.mappingBytes && h.mappingBytes <= 512 * 1024 * 1024;
}
// True when a header read back from the mapping still carries exactly the launch
// configuration. A self-consistent but different geometry is rejected.
inline bool sameConfiguration(const Header& shared, const Header& configured) noexcept {
    return shared.magicValue == configured.magicValue &&
           shared.mappingBytes == configured.mappingBytes &&
           shared.protocol == configured.protocol && shared.wordBits == configured.wordBits &&
           shared.headerBytes == configured.headerBytes &&
           shared.eventBytes == configured.eventBytes && shared.format == configured.format &&
           shared.minimumFrames == configured.minimumFrames && shared.frames == configured.frames &&
           shared.events == configured.events && shared.channels == configured.channels &&
           shared.payload == configured.payload && shared.stateBytes == configured.stateBytes &&
           shared.flags == configured.flags && shared.parentPid == configured.parentPid &&
           shared.sampleRate == configured.sampleRate && shared.bootMode == configured.bootMode &&
           shared.sleepMode == configured.sleepMode &&
           shared.quietFrames == configured.quietFrames &&
           shared.silenceThreshold == configured.silenceThreshold &&
           shared.initialBytes == configured.initialBytes &&
           shared.reverseTimeoutMs == configured.reverseTimeoutMs;
}
inline Status writeEvents(const PluginEventBuffer* in, Event* records, std::uint8_t* payload,
                          std::uint32_t maximumEvents, std::uint32_t maximumBytes,
                          std::uint32_t& count, std::uint32_t& used) noexcept {
    count = used = 0;
    if (!in)
        return Status::ok;
    if (in->size() > maximumEvents)
        return Status::block_too_large;
    for (std::size_t i = 0; i < in->size(); ++i) {
        const auto& e = *in->event(i);
        if (e.bytes.size() > maximumBytes - used)
            return Status::block_too_large;
        records[count++] = {std::uint32_t(e.kind),
                            e.frame,
                            e.id,
                            e.flags,
                            e.noteId,
                            e.port,
                            e.channel,
                            e.key,
                            e.value,
                            e.packet.words,
                            e.packet.count,
                            used,
                            std::uint32_t(e.bytes.size()),
                            encode(e.time)};
        if (!e.bytes.empty())
            std::memcpy(payload + used, e.bytes.data(), e.bytes.size());
        used += std::uint32_t(e.bytes.size());
    }
    return Status::ok;
}
// count/used are values the caller fetched once; the maxima are its private limits.
// Each record is copied out of the shared mapping before it is validated, so the
// other process cannot change it between the check and the use.
inline Status readEvents(const Event* records, const std::uint8_t* payload, std::uint32_t count,
                         std::uint32_t used, std::uint32_t maximumEvents,
                         std::uint32_t maximumBytes, std::uint32_t frames,
                         PluginEventBuffer& out, DroppedEvents* overflow = nullptr) noexcept {
    out.reset();
    if (count > maximumEvents || used > maximumBytes)
        return Status::invalid_argument;
    std::uint32_t previousFrame = 0, decodedBytes = 0, dropped = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        Event e;
        std::memcpy(static_cast<void*>(&e), records + i, sizeof(Event));
        if (e.kind > std::uint32_t(PluginEventKind::ump) || e.frame >= frames ||
            e.frame < previousFrame || e.offset > used || e.size > used - e.offset ||
            e.size > used - decodedBytes || e.wordCount > 4 || e.port < -1 || e.port > 32767 ||
            e.channel < -1 || e.channel > 15 || e.key < -1 || e.key > 127 ||
            e.transport.flags > 511 || e.transport.numerator > 65535 ||
            e.transport.denominator > 65535)
            return Status::invalid_argument;
        previousFrame = e.frame;
        decodedBytes += e.size;
        PluginEvent p;
        p.kind = PluginEventKind(e.kind);
        p.frame = e.frame;
        p.id = e.id;
        p.flags = e.flags;
        p.noteId = e.noteId;
        p.port = std::int16_t(e.port);
        p.channel = std::int16_t(e.channel);
        p.key = std::int16_t(e.key);
        p.value = e.value;
        if (e.size)
            p.bytes = {payload + e.offset, e.size};
        p.packet.words = e.words;
        p.packet.count = std::uint8_t(e.wordCount);
        p.time = decode(e.transport);
        const auto r = out.add(p);
        if (r == Status::update_queue_full && overflow)
            ++dropped;
        else if (r != Status::ok)
            return r;
    }
    if (overflow)
        overflow->add(dropped);
    return Status::ok;
}
class Mapping {
  public:
    Mapping() = default;
    ~Mapping();
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    Status create(std::string_view, std::size_t) noexcept;
    Status open(std::string_view) noexcept;
    std::byte* data = nullptr;
    std::size_t bytes = 0;

  private:
#if defined(_WIN32)
    void* handle = nullptr;
#elif !defined(__ANDROID__)
    int fd = -1;
    bool owner = false;
#endif
    std::string name;
};
class Child {
  public:
    Child() = default;
    ~Child();
    Status launch(const std::filesystem::path&, std::string_view) noexcept;
    bool running() noexcept;
    void close() noexcept;

  private:
#if defined(_WIN32)
    void* handle = nullptr;
#elif !defined(__ANDROID__)
    int pid = -1;
#endif
};
// Cross-process wake-up: a named auto-reset event on Windows, a futex on the shared
// word on Linux, a short polling wait on that word elsewhere. signal() never blocks,
// allocates or waits and may be called from the client audio path.
class Wake {
  public:
    Wake() = default;
    ~Wake();
    Wake(const Wake&) = delete;
    Wake& operator=(const Wake&) = delete;
    Status create(std::string_view mapping, std::string_view suffix,
                  std::atomic<std::uint32_t>& word) noexcept;
    Status open(std::string_view mapping, std::string_view suffix,
                std::atomic<std::uint32_t>& word) noexcept;
    // Waiter: read the token, re-check the condition, then wait(token, ms). A signal
    // issued after the token was read ends the wait immediately.
    std::uint32_t token() const noexcept;
    void wait(std::uint32_t token, unsigned milliseconds) noexcept;
    void signal() noexcept;
    // Like wait(), and also returns when native window messages are pending.
    void waitOrMessages(std::uint32_t token, unsigned milliseconds) noexcept;

  private:
    std::atomic<std::uint32_t>* word_ = nullptr;
#if defined(_WIN32)
    void* handle_ = nullptr;
#endif
};
std::uint32_t processId() noexcept;
bool parentRunning(std::uint32_t) noexcept;
void pumpNativeMessages() noexcept;
// Best-effort scheduling raise for the worker's audio thread; never fails.
void raiseAudioThreadPriority() noexcept;
} // namespace cyaudio::detail::wire
