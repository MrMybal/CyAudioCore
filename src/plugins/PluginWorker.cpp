#include "cyaudio/core/DspTypes.h"
#include "PluginWire.h"
#include "cydsp/math/VectorOps.h"
#include <algorithm>
#include <chrono>
#include <random>
#include <thread>
namespace cyaudio {
namespace {
using namespace detail::wire;
std::string mappingName() {
    std::random_device random;
#if defined(_WIN32)
    std::string name = "Local\\CyDSP-";
#else
    std::string name = "/cydsp-";
#endif
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned n = 0; n < 4; ++n) {
        const auto word = random();
        for (unsigned i = 0; i < 8; ++i)
            name += digits[(word >> (i * 4)) & 15];
    }
    return name;
}
void configure(Header& h, const PluginWorkerOptions& o, PluginFormat format) noexcept {
    h.format = std::uint32_t(format);
    h.minimumFrames = o.processing.minimumFrames;
    h.frames = o.processing.maximumFrames;
    h.events = o.processing.maximumEvents;
    h.channels = o.maximumChannelsPerDirection;
    h.payload = o.maximumPayloadBytes;
    h.stateBytes = o.maximumStateBytes;
    h.flags = (o.processing.useFloat64 ? 1u : 0u) | (o.processing.offline ? 2u : 0u);
    h.sampleRate = o.processing.sampleRate;
    h.parentPid = processId();
    h.mappingBytes = layout(h).total;
}
// Works on values already copied out of the mapping.
bool shape(std::uint32_t inputCount, std::uint32_t outputCount,
           const std::array<std::uint32_t, 64>& inputChannels,
           const std::array<std::uint32_t, 64>& outputChannels, std::uint32_t channels) noexcept {
    if (inputCount > 64 || outputCount > 64)
        return false;
    std::uint32_t inputs = 0, outputs = 0;
    for (std::uint32_t i = 0; i < inputCount; ++i) {
        if (inputChannels[i] > channels - inputs)
            return false;
        inputs += inputChannels[i];
    }
    for (std::uint32_t i = 0; i < outputCount; ++i) {
        if (outputChannels[i] > channels - outputs)
            return false;
        outputs += outputChannels[i];
    }
    return true;
}
} // namespace
struct PluginWorker::State {
    Mapping mapping;
    Child child;
    Wake audioWake, controlWake;
    Header* header = nullptr;
    Layout offsets;
    // Private, immutable copy of the launch dimensions. The child can write the
    // shared header, so nothing below reads a bound from it after boot.
    Limits limits;
    PluginWorkerInfo info;
    PluginWorkerOptions options;
    VectorOps ops;
    PluginEventBuffer discardEvents;
    std::unique_ptr<std::uint8_t[]> payload; // Private copy of one reply's payload.
    std::thread::id owner = std::this_thread::get_id();
    std::atomic<std::uint32_t> alive{1};
    std::uint64_t sequence = 0;
    bool previous = false;
    std::uint32_t lastHeartbeat = 0;
    std::chrono::steady_clock::time_point heartbeatTime = std::chrono::steady_clock::now();
    ~State() {
        if (header) {
            header->shutdown.store(1, std::memory_order_release);
            audioWake.signal();
            controlWake.signal();
        }
        child.close();
    }
    // Stops using the child and terminates it. Control thread only.
    void abandon() noexcept {
        alive.store(0, std::memory_order_release);
        header->shutdown.store(1, std::memory_order_release);
        audioWake.signal();
        controlWake.signal();
        child.close();
    }
    Status health() noexcept {
        if (!alive.load(std::memory_order_acquire))
            return Status::disconnected;
        if (!child.running()) {
            alive.store(0, std::memory_order_release);
            return Status::disconnected;
        }
        const auto beat = header->heartbeat.load(std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        if (beat != lastHeartbeat) {
            lastHeartbeat = beat;
            heartbeatTime = now;
        } else if (now - heartbeatTime >= std::chrono::milliseconds(options.heartbeatTimeoutMs)) {
            abandon();
            return Status::timed_out;
        }
        return Status::ok;
    }
    // The worker main thread publishes latency/tail under a seqlock.
    void refreshTiming() noexcept {
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            const auto before = header->timingSequence.load(std::memory_order_acquire);
            if (before & 1u)
                continue;
            const auto latency = header->latency.load(std::memory_order_acquire),
                       tail = header->tail.load(std::memory_order_acquire),
                       flags = header->timingFlags.load(std::memory_order_acquire);
            if (header->timingSequence.load(std::memory_order_acquire) != before)
                continue;
            info.pluginTiming = {latency, tail, (flags & 1u) != 0, (flags & 2u) != 0,
                                 (flags & 4u) != 0};
            return;
        }
    }
    Status canControl() noexcept {
        if (owner != std::this_thread::get_id())
            return Status::invalid_argument;
        // A worker abandoned after a control timeout reports disconnected, not a
        // transition that will never end.
        const auto r = health();
        if (r != Status::ok)
            return r;
        if (header->controlState.load(std::memory_order_acquire))
            return Status::transition_in_progress;
        return Status::ok;
    }
    Status rpc(Command command) noexcept {
        if (owner != std::this_thread::get_id())
            return Status::invalid_argument;
        auto r = canControl();
        if (r != Status::ok)
            return r;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(options.controlTimeoutMs);
        // Either wait can only expire on a stuck child, which is terminated.
        while (header->audioState.load(std::memory_order_acquire) == 1) {
            if (health() != Status::ok)
                return Status::disconnected;
            if (std::chrono::steady_clock::now() >= deadline) {
                abandon();
                return Status::timed_out;
            }
            pumpNativeMessages();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        previous = false;
        header->audioState.store(0, std::memory_order_release);
        header->command = std::uint32_t(command);
        header->controlState.store(1, std::memory_order_release);
        controlWake.signal();
        while (header->controlState.load(std::memory_order_acquire) != 2) {
            if (health() != Status::ok)
                return Status::disconnected;
            if (std::chrono::steady_clock::now() >= deadline) {
                abandon();
                return Status::timed_out;
            }
            pumpNativeMessages();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto status = header->controlStatus;
        refreshTiming();
        header->controlState.store(0, std::memory_order_release);
        return status <= std::uint32_t(Status::disconnected) ? Status(status)
                                                             : Status::invalid_argument;
    }
};
PluginWorker::PluginWorker() = default;
PluginWorker::~PluginWorker() = default;
PluginWorker::PluginWorker(PluginWorker&&) noexcept = default;
PluginWorker& PluginWorker::operator=(PluginWorker&&) noexcept = default;
Status PluginWorker::launch(const std::filesystem::path& executable, PluginFormat format,
                            const std::filesystem::path& plugin, std::string_view id,
                            const PluginWorkerOptions& options) noexcept {
    if (!executable.is_absolute() || !plugin.is_absolute() || id.empty() || id.size() >= 4096 ||
        id.find(char{}) != std::string_view::npos || !options.startupTimeoutMs ||
        options.startupTimeoutMs > 60000 || !options.controlTimeoutMs ||
        options.controlTimeoutMs > 60000 || !options.heartbeatTimeoutMs ||
        options.heartbeatTimeoutMs > 60000)
        return Status::invalid_argument;
    try {
        Header configuration;
        configure(configuration, options, format);
        if (!valid(configuration))
            return Status::invalid_spec;
        const auto path = plugin.u8string();
        if (path.empty() || path.size() >= configuration.pluginPath.size())
            return Status::invalid_argument;
        auto next = std::make_unique<State>();
        next->options = options;
        next->offsets = layout(configuration);
        next->limits = limits(configuration);
        auto r = VectorOps::create(CpuBackend::automatic, next->ops);
        if (r != Status::ok)
            return r;
        r = next->discardEvents.prepare(configuration.events, configuration.payload);
        if (r != Status::ok)
            return r;
        next->payload = std::make_unique_for_overwrite<std::uint8_t[]>(
            std::max<std::size_t>(configuration.payload, 1));
        const auto name = mappingName();
        r = next->mapping.create(name, next->offsets.total);
        if (r != Status::ok)
            return r;
        next->header = std::construct_at(reinterpret_cast<Header*>(next->mapping.data));
        configure(*next->header, options, format);
        std::copy(path.begin(), path.end(), next->header->pluginPath.begin());
        std::copy(id.begin(), id.end(), next->header->pluginId.begin());
        r = next->audioWake.create(name, "-audio", next->header->audioWake);
        if (r != Status::ok)
            return r;
        r = next->controlWake.create(name, "-control", next->header->controlWake);
        if (r != Status::ok)
            return r;
        r = next->child.launch(executable, name);
        if (r != Status::ok)
            return r;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(options.startupTimeoutMs);
        while (!next->header->boot.load(std::memory_order_acquire)) {
            if (!next->child.running())
                return Status::disconnected;
            if (std::chrono::steady_clock::now() >= deadline)
                return Status::timed_out;
            pumpNativeMessages();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (next->header->boot.load(std::memory_order_acquire) != 1) {
            const auto failure = next->header->failure.load(std::memory_order_acquire);
            return failure <= std::uint32_t(Status::disconnected) ? Status(failure)
                                                                  : Status::io_error;
        }
        // The child had a writable view since it started. Accept the boot only if
        // every dimension still equals the configuration this side computed, and
        // take the bus shape from one copy of the header fields.
        const auto& h = *next->header;
        if (!sameConfiguration(h, configuration))
            return Status::invalid_argument;
        const auto inputCount = h.inputCount, outputCount = h.outputCount;
        const auto inputChannels = h.inputChannels, outputChannels = h.outputChannels;
        if (!shape(inputCount, outputCount, inputChannels, outputChannels,
                   configuration.channels))
            return Status::invalid_argument;
        next->info.inputChannels.assign(inputChannels.begin(), inputChannels.begin() + inputCount);
        next->info.outputChannels.assign(outputChannels.begin(),
                                         outputChannels.begin() + outputCount);
        next->refreshTiming();
        next->info.pipelineLatencyFrames = configuration.frames;
        next->info.protocolVersion = version;
        next->lastHeartbeat = h.heartbeat.load(std::memory_order_relaxed);
        next->heartbeatTime = std::chrono::steady_clock::now();
        state_.swap(next);
        return Status::ok;
    } catch (const std::bad_alloc&) {
        return Status::allocation_failed;
    } catch (...) {
        return Status::io_error;
    }
}
const PluginWorkerInfo* PluginWorker::info() const noexcept {
    return state_ ? &state_->info : nullptr;
}
template <Sample T>
Status PluginWorker::processImpl(const PluginProcess<T>& p,
                                 PluginWorkerBlockResult& result) noexcept {
    result = {};
    if (!state_)
        return Status::not_prepared;
    auto& s = *state_;
    auto& h = *s.header;
    const auto& limit = s.limits;
    if (p.frames != limit.frames)
        return Status::block_too_large;
    if (std::same_as<T, double> != bool(limit.flags & 1))
        return Status::unsupported_format;
    if (p.inputs.size() != s.info.inputChannels.size() ||
        p.outputs.size() != s.info.outputChannels.size())
        return Status::channel_mismatch;
    for (std::size_t i = 0; i < p.inputs.size(); ++i)
        if (!p.inputs[i].isValid() || p.inputs[i].channels() != s.info.inputChannels[i] ||
            p.inputs[i].frames() < p.frames)
            return Status::channel_mismatch;
    for (std::size_t i = 0; i < p.outputs.size(); ++i)
        if (!p.outputs[i].isValid() || p.outputs[i].channels() != s.info.outputChannels[i] ||
            p.outputs[i].frames() < p.frames)
            return Status::channel_mismatch;
    if (p.inputEvents && p.inputEvents == p.outputEvents)
        return Status::invalid_argument;
    if (p.transport && !detail::validPluginTransport(*p.transport))
        return Status::invalid_argument;
    // The caller's events are checked completely before the mailbox is touched, so
    // an invalid request preserves the outputs and the block sequence.
    if (p.inputEvents) {
        if (p.inputEvents->size() > limit.events)
            return Status::block_too_large;
        std::size_t bytes = 0;
        for (std::size_t i = 0; i < p.inputEvents->size(); ++i) {
            const auto& e = *p.inputEvents->event(i);
            if (e.frame >= limit.frames)
                return Status::invalid_argument;
            bytes += e.bytes.size();
        }
        if (bytes > limit.payload)
            return Status::block_too_large;
    }
    const auto silence = [&]() noexcept {
        for (const auto& bus : p.outputs)
            for (std::size_t ch = 0; ch < bus.channels(); ++ch)
                s.ops.fill(bus.channel(ch).first(p.frames), T{});
        if (p.outputEvents)
            p.outputEvents->reset();
    };
    if (!s.alive.load(std::memory_order_acquire)) {
        silence();
        return Status::disconnected;
    }
    const auto sequence = ++s.sequence;
    Status received = Status::ok;
    bool reply = false;
    std::uint32_t replyEvents = 0, replyBytes = 0;
    const auto state = h.audioState.load(std::memory_order_acquire);
    if (state == 2) {
        // Each reply field is fetched exactly once and checked against private limits.
        const auto replySequence = h.sequence;
        const auto status = h.audioStatus;
        replyEvents = h.outputEvents;
        replyBytes = h.outputBytes;
        if (s.previous && replySequence == sequence - 1) {
            if (status > std::uint32_t(Status::disconnected) || replyEvents > limit.events ||
                replyBytes > limit.payload)
                received = Status::invalid_argument;
            else
                received = Status(status);
            reply = received == Status::ok;
        }
    }
    // The worker leaves the mailbox alone in states 0 and 2, so the input half is
    // written before the reply is copied out. Output buffers may therefore alias
    // input buffers without a staging copy.
    const bool submit = state != 1;
    std::uint32_t inputEvents = 0, inputBytes = 0;
    if (submit) {
        (void)writeEvents(p.inputEvents,
                          reinterpret_cast<Event*>(s.mapping.data + s.offsets.inputEvents),
                          reinterpret_cast<std::uint8_t*>(s.mapping.data + s.offsets.inputPayload),
                          limit.events, limit.payload, inputEvents, inputBytes);
        auto* audio = reinterpret_cast<T*>(s.mapping.data + s.offsets.input);
        std::size_t channel = 0;
        for (const auto& bus : p.inputs)
            for (std::size_t ch = 0; ch < bus.channels(); ++ch) {
                (void)s.ops.copy(bus.channel(ch).first(p.frames),
                                 std::span<T>(audio + channel * limit.frames, limit.frames));
                ++channel;
            }
    }
    if (reply) {
        const auto* audio = reinterpret_cast<const T*>(s.mapping.data + s.offsets.output);
        std::size_t channel = 0;
        for (const auto& bus : p.outputs)
            for (std::size_t ch = 0; ch < bus.channels(); ++ch)
                (void)s.ops.copy(
                    std::span<const T>(audio + (channel++) * limit.frames, limit.frames),
                    bus.channel(ch).first(p.frames));
        // Events are decoded from a private copy of the payload, not from memory the
        // child can still rewrite.
        if (replyBytes)
            std::memcpy(s.payload.get(), s.mapping.data + s.offsets.outputPayload, replyBytes);
        auto& events = p.outputEvents ? *p.outputEvents : s.discardEvents;
        received = readEvents(
            reinterpret_cast<const Event*>(s.mapping.data + s.offsets.outputEvents),
            s.payload.get(), replyEvents, replyBytes, limit.events, limit.payload, limit.frames,
            events);
        if (received == Status::ok) {
            result.received = true;
            result.receivedSequence = sequence - 1;
        }
    }
    if (!result.received)
        silence(); // Only a block without a usable reply is cleared.
    result.missedDeadline = s.previous && !result.received;
    if (submit) {
        h.inputEvents = inputEvents;
        h.inputBytes = inputBytes;
        h.sequence = sequence;
        h.steadyFrame = p.steadyFrame;
        h.hasTransport = p.transport ? 1u : 0u;
        h.transport = encode(p.transport ? *p.transport : PluginTransport{});
        h.audioState.store(1, std::memory_order_seq_cst);
        // One non-blocking system call, and only while the worker really sleeps.
        if (h.audioSleeping.load(std::memory_order_seq_cst))
            s.audioWake.signal();
        result.submitted = true;
        result.submittedSequence = sequence;
    }
    s.previous = true;
    if (received != Status::ok)
        return received;
    return result.missedDeadline ? Status::would_block : Status::ok;
}
Status PluginWorker::process(const PluginProcess<float>& p, PluginWorkerBlockResult& r) noexcept {
    return processImpl(p, r);
}
Status PluginWorker::process(const PluginProcess<double>& p, PluginWorkerBlockResult& r) noexcept {
    return processImpl(p, r);
}
Status PluginWorker::poll() noexcept {
    std::uint32_t flags = 0;
    return poll(flags);
}
Status PluginWorker::poll(std::uint32_t& requests) noexcept {
    requests = 0;
    if (!state_)
        return Status::not_prepared;
    const auto r = state_->health();
    if (r == Status::ok) {
        requests = state_->header->requests.exchange(0, std::memory_order_acq_rel);
        state_->refreshTiming(); // Latency/tail may change while the plugin runs.
    }
    return r;
}
Status PluginWorker::saveState(std::vector<std::uint8_t>& out) noexcept {
    if (!state_)
        return Status::not_prepared;
    const auto r = state_->rpc(Command::save);
    if (r != Status::ok)
        return r;
    const auto bytes = state_->header->controlBytes; // Single fetch, private bound.
    if (bytes > state_->limits.stateBytes)
        return Status::invalid_argument;
    try {
        const auto* p =
            reinterpret_cast<const std::uint8_t*>(state_->mapping.data + state_->offsets.control);
        std::vector<std::uint8_t> next(p, p + bytes);
        out.swap(next);
        return Status::ok;
    } catch (...) {
        return Status::allocation_failed;
    }
}
Status PluginWorker::loadState(std::span<const std::uint8_t> bytes) noexcept {
    if (!state_)
        return Status::not_prepared;
    const auto ready = state_->canControl();
    if (ready != Status::ok)
        return ready;
    if (bytes.size() > state_->limits.stateBytes)
        return Status::block_too_large;
    if (!bytes.empty())
        std::memcpy(state_->mapping.data + state_->offsets.control, bytes.data(), bytes.size());
    state_->header->controlBytes = std::uint32_t(bytes.size());
    return state_->rpc(Command::load);
}
Status PluginWorker::restart() noexcept {
    return state_ ? state_->rpc(Command::restart) : Status::not_prepared;
}
Status PluginWorker::openEditor(const PluginWindow& window, PluginEditorSize& size) noexcept {
    if (!state_)
        return Status::not_prepared;
    const auto ready = state_->canControl();
    if (ready != Status::ok)
        return ready;
    if (!isFinite(window.scale) || window.scale <= 0 || window.scale > 16)
        return Status::invalid_argument;
    auto& h = *state_->header;
    h.windowApi = std::uint32_t(window.api);
    h.windowHandle = window.handle;
    h.floating = window.floating ? 1u : 0u;
    h.scale = window.scale;
    const auto r = state_->rpc(Command::openEditor);
    if (r == Status::ok)
        size = {h.width, h.height};
    return r;
}
Status PluginWorker::resizeEditor(PluginEditorSize& size) noexcept {
    if (!state_)
        return Status::not_prepared;
    const auto ready = state_->canControl();
    if (ready != Status::ok)
        return ready;
    auto& h = *state_->header;
    h.width = size.width;
    h.height = size.height;
    const auto r = state_->rpc(Command::resizeEditor);
    if (r == Status::ok)
        size = {h.width, h.height};
    return r;
}
Status PluginWorker::scaleEditor(double scale) noexcept {
    if (!state_)
        return Status::not_prepared;
    const auto r = state_->canControl();
    if (r != Status::ok)
        return r;
    if (!isFinite(scale) || scale <= 0 || scale > 16)
        return Status::invalid_argument;
    state_->header->scale = scale;
    return state_->rpc(Command::scaleEditor);
}
Status PluginWorker::showEditor(bool show) noexcept {
    if (!state_)
        return Status::not_prepared;
    const auto r = state_->canControl();
    if (r != Status::ok)
        return r;
    state_->header->boolean = show ? 1u : 0u;
    return state_->rpc(Command::showEditor);
}
Status PluginWorker::focusEditor(bool focus) noexcept {
    if (!state_)
        return Status::not_prepared;
    const auto r = state_->canControl();
    if (r != Status::ok)
        return r;
    state_->header->boolean = focus ? 1u : 0u;
    return state_->rpc(Command::focusEditor);
}
Status PluginWorker::keyEditor(const PluginEditorKey& key) noexcept {
    if (!state_)
        return Status::not_prepared;
    const auto r = state_->canControl();
    if (r != Status::ok)
        return r;
    auto& h = *state_->header;
    h.keyCharacter = key.character;
    h.keyCode = std::uint16_t(key.key);
    h.keyModifiers = key.modifiers;
    h.keyDown = key.down ? 1u : 0u;
    return state_->rpc(Command::keyEditor);
}
Status PluginWorker::wheelEditor(float distance) noexcept {
    if (!state_)
        return Status::not_prepared;
    const auto r = state_->canControl();
    if (r != Status::ok)
        return r;
    if (!isFinite(distance))
        return Status::invalid_argument;
    state_->header->wheel = distance;
    return state_->rpc(Command::wheelEditor);
}
PluginEditorSize PluginWorker::requestedEditorSize() const noexcept {
    if (!state_)
        return {};
    return {state_->header->requestedWidth.load(std::memory_order_acquire),
            state_->header->requestedHeight.load(std::memory_order_acquire)};
}
Status PluginWorker::closeEditor() noexcept {
    return state_ ? state_->rpc(Command::closeEditor) : Status::not_prepared;
}
void PluginWorker::close() noexcept {
    state_.reset();
}
} // namespace cyaudio
