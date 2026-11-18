#pragma once
#include "cyaudio/core/DspTypes.h"
#include "cyaudio/midi/Ump.h"
#include "cydsp/core/AudioBlock.h"
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
namespace cyaudio {
class ClapExtensions;
struct ClapHostServices;
namespace detail {
struct PluginInstanceAccess; // Private door for the separate-process worker service.
struct PluginRemoteAccess;
} // namespace detail
enum class PluginFormat { clap, vst3, audioUnit, aax, lv2, jsfx, custom };
enum class PluginScheduling { pipelined, sameCycle };
// An explicit caller-owned worker executable must handle --cydsp-worker <mapping>.
// No public built-in provider ever falls back to loading foreign code in the host.
struct PluginIsolationOptions {
    std::filesystem::path workerExecutable;
    PluginScheduling scheduling = PluginScheduling::pipelined;
    std::uint32_t maximumFrames = 8192, maximumEvents = 4096, maximumChannelsPerDirection = 64,
                  maximumPayloadBytes = 65536, maximumStateBytes = 16 * 1024 * 1024;
    unsigned startupTimeoutMs = 5000, controlTimeoutMs = 2000, heartbeatTimeoutMs = 3000;
    bool clapThreadPool = false; // Optional worker-local serial CLAP task executor.
};
enum class PluginWindowApi { win32, cocoa, x11, wayland, unspecified }; // CLAP floating API.
struct PluginWindow {
    PluginWindowApi api = PluginWindowApi::win32;
    std::uintptr_t handle = 0;
    bool floating = false;
    double scale = 1;
};
enum class PluginEditorKeyCode : std::uint16_t {
    none,
    backspace,
    tab,
    enter,
    escape,
    space,
    left,
    up,
    right,
    down,
    pageUp,
    pageDown,
    home,
    end,
    insert,
    deleteKey,
    f1,
    f2,
    f3,
    f4,
    f5,
    f6,
    f7,
    f8,
    f9,
    f10,
    f11,
    f12,
    f13,
    f14,
    f15,
    f16,
    f17,
    f18,
    f19,
    f20,
    f21,
    f22,
    f23,
    f24
};
enum PluginEditorModifier : std::uint16_t {
    editorShift = 1,
    editorAlt = 2,
    editorControl = 4,
    editorSuper = 8
};
struct PluginEditorKey {
    std::uint16_t character = 0; // UTF-16 code unit; surrogate pairs arrive as separate events.
    PluginEditorKeyCode key = PluginEditorKeyCode::none;
    std::uint16_t modifiers = 0;
    bool down = true;
};
struct PluginEditorSize {
    std::uint32_t width = 0, height = 0;
};
struct PluginDescriptor {
    PluginFormat format = PluginFormat::custom;
    std::string id, name, vendor, version, description;
};
struct PluginAudioPort {
    std::uint32_t id = 0, channels = 0, inPlacePair = 0xffffffffu;
    std::string name, type;
    bool main = false, supports64 = false;
    bool prefers64 = false, commonSampleSize = false;
    std::vector<std::uint8_t> surroundMap;
    bool ambisonicKnown = false;
    std::uint32_t ambisonicOrdering = 0, ambisonicNormalization = 0;
};
struct PluginNotePort {
    std::uint32_t id = 0, dialects = 0, preferredDialect = 0;
    std::string name;
};
enum PluginParameterTarget : std::uint32_t {
    parameterNoteId = 1,
    parameterPort = 2,
    parameterChannel = 4,
    parameterKey = 8
};
struct PluginParameter {
    std::uint32_t id = 0;
    std::string name, module;
    double minimum = 0, maximum = 1, defaultValue = 0;
    bool stepped = false, readOnly = false, automatable = false, modulatable = false,
         hidden = false;
    bool normalized = false; // VST3 values use [0,1]; CLAP keeps plain native values.
    std::uint32_t automationTargets = 0,
                  modulationTargets = 0; // Per-note/port/channel/key support.
    std::string units{};
    bool periodic = false, bypass = false, requiresProcess = false, enumeration = false;
};
struct PluginDescription {
    PluginDescriptor plugin;
    std::vector<PluginAudioPort> inputs, outputs;
    std::vector<PluginNotePort> noteInputs, noteOutputs;
    std::vector<PluginParameter> parameters;
    bool state = false, editor = false, float32 = true, float64 = false;
};
struct PluginTiming {
    std::uint32_t latencyFrames = 0, tailFrames = 0;
    bool latencyKnown = false, tailKnown = false, infiniteTail = false;
};
struct PluginTransport {
    std::int64_t songFrame = 0;
    double beats = 0, seconds = 0, tempo = 120, tempoIncrement = 0;
    double loopStartBeats = 0, loopEndBeats = 0, loopStartSeconds = 0, loopEndSeconds = 0;
    double barStart = 0;
    std::int32_t barNumber = 0;
    std::uint16_t numerator = 4, denominator = 4;
    bool hasSongFrame = false, hasBeats = false, hasSeconds = false, hasTempo = false,
         hasSignature = false, playing = false, recording = false, looping = false, preRoll = false;
};
enum class PluginEventKind {
    noteOn,
    noteOff,
    noteChoke,
    noteEnd,
    noteExpression,
    parameterValue,
    parameterModulation,
    gestureBegin,
    gestureEnd,
    transport,
    midi1,
    sysex,
    ump
};
struct PluginEvent {
    PluginEventKind kind = PluginEventKind::parameterValue;
    std::uint32_t frame = 0, id = 0, flags = 0;
    std::int32_t noteId = -1;
    std::int16_t port = -1, channel = -1, key = -1; // Channel 0..15; -1 wildcard.
    double value = 0;
    std::span<const std::uint8_t> bytes; // MIDI/SysEx, copied by add().
    UmpPacket packet;
    PluginTransport time;
};
// Stable ordered events and copied variable payload, prepared on control thread.
// add/reset/event are allocation-free; never retains foreign plugin payload pointers.
class PluginEventBuffer {
  public:
    PluginEventBuffer() = default;
    PluginEventBuffer(const PluginEventBuffer&);
    PluginEventBuffer& operator=(const PluginEventBuffer&);
    PluginEventBuffer(PluginEventBuffer&&) noexcept;
    PluginEventBuffer& operator=(PluginEventBuffer&&) noexcept;
    Status prepare(std::size_t maximumEvents = 1024, std::size_t maximumBytes = 65536) noexcept;
    Status add(const PluginEvent&) noexcept;
    void reset() noexcept {
        count_ = used_ = 0;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return count_;
    }
    [[nodiscard]] const PluginEvent* event(std::size_t index) const noexcept;

  private:
    void swap(PluginEventBuffer&) noexcept;
    std::vector<PluginEvent> events_;
    std::vector<std::uint8_t> bytes_;
    std::size_t count_ = 0, used_ = 0;
};
enum class PluginSleepMode { automatic, disabled, whenQuiet };
// automatic follows CLAP's process status or a finite-tail VST3 effect contract.
// whenQuiet explicitly asserts that an otherwise unknown processor is safe to
// suspend after silence. Known infinite tails are never suspended heuristically.
struct PluginSleepOptions {
    PluginSleepMode mode = PluginSleepMode::automatic;
    std::uint32_t quietFrames = 2048;
    double silenceThreshold = 0; // Exact digital silence by default; finite [0, 1].
};
struct PluginSleepInfo {
    bool sleeping = false;
    std::uint32_t processedBlocks = 0, skippedBlocks = 0; // Wrapping counters.
};
enum class PluginSleepHint { keepProcessing, whenQuiet, afterTail, sleep };
struct PluginPrepareOptions {
    double sampleRate = 48000;
    std::uint32_t minimumFrames = 1, maximumFrames = 512, maximumEvents = 1024;
    bool useFloat64 = false, offline = false;
    PluginSleepOptions sleep;
};
template <Sample T> struct PluginProcess {
    std::span<const AudioBlock<const T>> inputs;
    std::span<const AudioBlock<T>> outputs;
    std::uint32_t frames = 0;
    const PluginEventBuffer* inputEvents = nullptr;
    PluginEventBuffer* outputEvents = nullptr; // Reset by instance before process.
    const PluginTransport* transport = nullptr;
    std::int64_t steadyFrame = -1;
};
enum PluginRequest : std::uint32_t {
    pluginRestart = 1,
    pluginProcess = 2,
    pluginMainThread = 4,
    pluginParametersChanged = 8,
    pluginPortsChanged = 16,
    pluginLatencyChanged = 32,
    pluginTailChanged = 64,
    pluginStateDirty = 128,
    pluginEditorClosed = 256,
    pluginEditorShow = 512,
    pluginEditorHide = 1024,
    pluginEditorResize = 2048,
    // Output events the host could not translate or store were dropped; audio is kept.
    pluginEventsDropped = 4096,
    pluginParameterFlush = 8192,
    pluginAudioConfigurationsChanged = 16384,
    pluginNoteNamesChanged = 32768,
    pluginRemoteControlsChanged = 65536,
    pluginVoiceInfoChanged = 131072,
    pluginPresetLoaded = 262144,
    pluginPresetError = 524288,
    pluginLogsDropped = 1048576
};
struct PluginHostCallbacks {
    void* context = nullptr;
    // Called only from pumpMainThread()/main editor methods. Resize callback must
    // synchronously resize the parent's client area before accepting.
    bool (*resizeEditor)(void*, PluginEditorSize) noexcept = nullptr;
    void (*requests)(void*, std::uint32_t flags) noexcept = nullptr;
    // Optional native controller edits on UI. Audio/worker recording receives
    // the same live gestures/values through PluginProcess::outputEvents.
    void (*edited)(void*, const PluginEvent&) noexcept = nullptr;
    const ClapHostServices* clap = nullptr; // Copied at create(); see ClapExtensions.h.
    // May run on any plugin thread. Only signal an atomic wake request here; never
    // re-enter the instance. Isolated providers use this inside the worker only.
    void (*wakeAudio)(void*) noexcept = nullptr;
};
// Original provider boundary for separately supplied licensed/native formats.
// Control/UI functions run on the creating UI thread; start/stop/reset/process
// share a single serialized symbolic audio thread. The application owns handoff.
// Built-in providers always return isolated backends. A custom adopted backend
// is trusted application code and executes in its caller process.
class PluginBackend {
  public:
    virtual ~PluginBackend() = default;
    virtual Status status() const noexcept {
        return Status::ok;
    }
    virtual bool isolated() const noexcept {
        return false;
    }
    // Native opt-in. Hints run on audio; the generation must be thread-safe and
    // change whenever queued edits/external work require another process call.
    virtual PluginSleepHint sleepHint() const noexcept {
        return PluginSleepHint::keepProcessing;
    }
    virtual std::uint32_t wakeGeneration() const noexcept {
        return 0;
    }
    virtual PluginSleepInfo sleepInfo() const noexcept {
        return {};
    }
    virtual void wake() noexcept {}
    virtual Status submit(const PluginProcess<float>&, std::uint64_t&) noexcept {
        return Status::unsupported_format;
    }
    virtual Status submit(const PluginProcess<double>&, std::uint64_t&) noexcept {
        return Status::unsupported_format;
    }
    virtual Status collect(const PluginProcess<float>&, std::uint64_t) noexcept {
        return Status::unsupported_format;
    }
    virtual Status collect(const PluginProcess<double>&, std::uint64_t) noexcept {
        return Status::unsupported_format;
    }
    virtual Status discard(std::uint64_t) noexcept {
        return Status::unsupported_format;
    }
    virtual ClapExtensions* clapExtensions() noexcept {
        return nullptr;
    }
    [[nodiscard]] virtual const PluginDescription& description() const noexcept = 0;
    virtual Status activate(const PluginPrepareOptions&) noexcept = 0;
    virtual Status deactivate() noexcept = 0;
    virtual Status start() noexcept = 0;
    virtual void stop() noexcept = 0;
    virtual Status reset() noexcept = 0;
    virtual Status process(const PluginProcess<float>&) noexcept = 0;
    virtual Status process(const PluginProcess<double>&) noexcept = 0;
    virtual Status refresh() noexcept {
        return Status::ok;
    }
    virtual PluginTiming timing() const noexcept {
        return {};
    }
    virtual Status parameterValue(std::uint32_t, double&) noexcept {
        return Status::unsupported_format;
    }
    virtual Status parameterText(std::uint32_t, double, std::string&) noexcept {
        return Status::unsupported_format;
    }
    virtual Status flush(const PluginEventBuffer&, PluginEventBuffer&) noexcept {
        return Status::unsupported_format;
    }
    virtual Status saveState(std::vector<std::uint8_t>&, std::size_t) noexcept {
        return Status::unsupported_format;
    }
    virtual Status loadState(std::span<const std::uint8_t>) noexcept {
        return Status::unsupported_format;
    }
    virtual Status openEditor(const PluginWindow&, PluginEditorSize&) noexcept {
        return Status::unsupported_format;
    }
    virtual Status resizeEditor(PluginEditorSize&) noexcept {
        return Status::unsupported_format;
    }
    virtual Status scaleEditor(double) noexcept {
        return Status::unsupported_format;
    }
    virtual Status showEditor(bool) noexcept {
        return Status::unsupported_format;
    }
    virtual Status focusEditor(bool) noexcept {
        return Status::unsupported_format;
    }
    virtual Status keyEditor(const PluginEditorKey&) noexcept {
        return Status::unsupported_format;
    }
    virtual Status wheelEditor(float) noexcept {
        return Status::unsupported_format;
    }
    virtual void closeEditor() noexcept {}
    virtual std::uint32_t pumpMainThread() noexcept {
        return 0;
    }
};
enum class PluginLifecycle { empty, initialized, active, processing };
// Enforces lifecycle, bus sizes, prepared precision, event offsets and UI affinity.
// destroy/move/control require audio owner idle; stop() must precede deactivate().
// The foreign plugin's own realtime behavior is outside CyDSP's control.
class PluginInstance {
  public:
    PluginInstance();
    ~PluginInstance();
    PluginInstance(PluginInstance&&) noexcept;
    PluginInstance& operator=(PluginInstance&&) noexcept;
    // Trusted custom backends only; this is not a loader for foreign modules.
    static Status adopt(std::unique_ptr<PluginBackend>, PluginInstance&) noexcept;
    const PluginDescription* description() const noexcept;
    PluginLifecycle lifecycle() const noexcept;
    ClapExtensions* clapExtensions() noexcept;
    bool isolated() const noexcept;
    Status activate(const PluginPrepareOptions& = {}) noexcept;
    Status start() noexcept;
    Status process(const PluginProcess<float>&) noexcept;
    Status process(const PluginProcess<double>&) noexcept;
    // sameCycle only. Each call is bounded and never waits. collect() may be retried
    // while useful independent work is available; discard() closes a missed cycle.
    Status submit(const PluginProcess<float>&, std::uint64_t& ticket) noexcept;
    Status submit(const PluginProcess<double>&, std::uint64_t& ticket) noexcept;
    Status collect(const PluginProcess<float>&, std::uint64_t ticket) noexcept;
    Status collect(const PluginProcess<double>&, std::uint64_t ticket) noexcept;
    Status discard(std::uint64_t ticket) noexcept;
    Status stop() noexcept;
    Status reset() noexcept;
    Status deactivate() noexcept;
    Status refresh() noexcept; // Inactive; refreshes ports/parameters after restart/rescan.
    Status parameterValue(std::uint32_t,
                          double&) noexcept; // Main; CLAP also allows while processing.
    Status parameterText(std::uint32_t, double, std::string&) noexcept;
    Status flush(const PluginEventBuffer&, PluginEventBuffer&) noexcept; // Inactive main only.
    // Active CLAP flush: serialized audio owner, never concurrently with process().
    Status flushAudio(const PluginEventBuffer&, PluginEventBuffer&) noexcept;
    Status saveState(std::vector<std::uint8_t>&,
                     std::size_t maximumBytes = 64 * 1024 * 1024) noexcept;
    Status loadState(std::span<const std::uint8_t>) noexcept; // Inactive, bounded by 64 MiB.
    PluginTiming timing() const noexcept;                     // Cached during activate/pump.
    PluginSleepInfo sleepInfo() const noexcept; // Lock-free snapshot; counters may wrap.
    void wake() noexcept; // Thread-safe, allocation-free request; resumes on next audio block.
    Status openEditor(const PluginWindow&, PluginEditorSize&) noexcept;
    Status resizeEditor(PluginEditorSize&) noexcept;
    Status scaleEditor(double) noexcept;
    Status showEditor(bool) noexcept;
    Status focusEditor(bool) noexcept;
    Status keyEditor(const PluginEditorKey&) noexcept;
    Status wheelEditor(float distance) noexcept;
    Status closeEditor() noexcept;
    Status pumpMainThread(std::uint32_t& requests) noexcept;
    Status close() noexcept; // Main + audio idle, processing must already be stopped.
  private:
    friend struct detail::PluginInstanceAccess;
    struct State;
    std::unique_ptr<State> state_;
    template <Sample T>
    Status processImpl(const PluginProcess<T>&, unsigned operation = 0,
                       std::uint64_t* ticket = nullptr) noexcept;
};
class PluginProvider {
  public:
    virtual ~PluginProvider() = default;
    virtual Status scan(const std::filesystem::path&, std::vector<PluginDescriptor>&) noexcept = 0;
    virtual Status create(const std::filesystem::path&, std::string_view id,
                          const PluginHostCallbacks&, PluginInstance&) noexcept = 0;
};
// Creates an actual built-in adapter when compiled/supported. Never substitutes formats.
Status createPluginProvider(PluginFormat, std::unique_ptr<PluginProvider>&,
                            const PluginIsolationOptions& = {}) noexcept;
// CLAP is the primary format; other formats remain explicit choices.
inline Status createPluginProvider(std::unique_ptr<PluginProvider>& result,
                                   const PluginIsolationOptions& isolation = {}) noexcept {
    return createPluginProvider(PluginFormat::clap, result, isolation);
}
} // namespace cyaudio
