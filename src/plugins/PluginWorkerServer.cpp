#include "cyaudio/core/DspTypes.h"
#include "PluginWire.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <semaphore>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <objbase.h>
#include <windows.h>
#endif
namespace cyaudio {
namespace {
using namespace detail::wire;
// Spin hint without any ISA header: those stay inside the CyDSP private kernels.
void relax() noexcept {
#if defined(_WIN32)
    YieldProcessor();
#else
    std::this_thread::yield();
#endif
}
void publishTiming(Header& h, PluginTiming timing) noexcept {
    h.timingSequence.fetch_add(1, std::memory_order_acq_rel); // Odd: being written.
    h.latency.store(timing.latencyFrames, std::memory_order_release);
    h.tail.store(timing.tailFrames, std::memory_order_release);
    h.timingFlags.store((timing.latencyKnown ? 1u : 0u) | (timing.tailKnown ? 2u : 0u) |
                            (timing.infiniteTail ? 4u : 0u),
                        std::memory_order_release);
    h.timingSequence.fetch_add(1, std::memory_order_release);
}
bool sameTiming(const PluginTiming& a, const PluginTiming& b) noexcept {
    return a.latencyFrames == b.latencyFrames && a.tailFrames == b.tailFrames &&
           a.latencyKnown == b.latencyKnown && a.tailKnown == b.tailKnown &&
           a.infiniteTail == b.infiniteTail;
}
bool describe(PluginInstance& instance, Header& h, std::uint32_t channels, bool initial) noexcept {
    const auto* d = instance.description();
    if (!d || d->inputs.size() > 64 || d->outputs.size() > 64)
        return false;
    std::uint32_t inputs = 0, outputs = 0;
    for (std::size_t i = 0; i < d->inputs.size(); ++i) {
        if (d->inputs[i].channels > channels - inputs)
            return false;
        inputs += d->inputs[i].channels;
        if (!initial && (i >= h.inputCount || h.inputChannels[i] != d->inputs[i].channels))
            return false;
        h.inputChannels[i] = d->inputs[i].channels;
    }
    for (std::size_t i = 0; i < d->outputs.size(); ++i) {
        if (d->outputs[i].channels > channels - outputs)
            return false;
        outputs += d->outputs[i].channels;
        if (!initial && (i >= h.outputCount || h.outputChannels[i] != d->outputs[i].channels))
            return false;
        h.outputChannels[i] = d->outputs[i].channels;
    }
    if (!initial && (h.inputCount != d->inputs.size() || h.outputCount != d->outputs.size()))
        return false;
    h.inputCount = std::uint32_t(d->inputs.size());
    h.outputCount = std::uint32_t(d->outputs.size());
    publishTiming(h, instance.timing());
    return true;
}
// Requests from the worker main thread to its audio thread. start/stop execute on the
// audio thread, which keeps the plugin's audio-thread affinity; pause/resume only
// park it outside the plugin. Every request is acknowledged before the next one.
enum class AudioCommand : std::uint32_t { none, pause, resume, stop, start, quit };
// Two threads. The main thread owns the plugin's UI/control side: control RPCs,
// pumpMainThread, native messages. The audio thread owns the audio mailbox, the
// heartbeat and the plugin's start/process/stop, so a modal loop on the main thread
// (popup menu, file dialog, drag) no longer stops audio or the heartbeat.
template <Sample T> class Server {
    Header& h;
    std::byte* data;
    const Limits limit; // Private copy; the shared header is not re-read for bounds.
    const Layout offsets;
    const std::uint32_t parent;
    const PluginPrepareOptions options;
    PluginInstance& plugin;
    Wake& audioWake;
    Wake& controlWake;
    PluginEventBuffer incoming, outgoing;
    std::vector<const T*> inputPointers;
    std::vector<T*> outputPointers;
    std::vector<AudioBlock<const T>> inputs;
    std::vector<AudioBlock<T>> outputs;
    std::thread thread;
    std::atomic<std::uint32_t> command{0}, commandStatus{0};
    std::binary_semaphore acknowledged{0};
    bool paused = false; // Audio thread only.

    // ---- audio thread ----
    void audio() noexcept {
        const auto count = h.inputEvents, used = h.inputBytes;
        Status result =
            readEvents(reinterpret_cast<const Event*>(data + offsets.inputEvents),
                       reinterpret_cast<const std::uint8_t*>(data + offsets.inputPayload), count,
                       used, limit.events, limit.payload, limit.frames, incoming);
        const auto wireTransport = h.transport;
        const auto hasTransport = h.hasTransport;
        const auto steadyFrame = h.steadyFrame;
        if (hasTransport > 1 || wireTransport.flags > 511 || wireTransport.numerator > 65535 ||
            wireTransport.denominator > 65535)
            result = Status::invalid_argument;
        const auto transport = decode(wireTransport);
        if (result == Status::ok)
            result = plugin.process(
                PluginProcess<T>{inputs, outputs, limit.frames, &incoming, &outgoing,
                                 hasTransport ? &transport : nullptr, steadyFrame});
        std::uint32_t outputEvents = 0, outputBytes = 0;
        if (result == Status::ok)
            result = writeEvents(&outgoing, reinterpret_cast<Event*>(data + offsets.outputEvents),
                                 reinterpret_cast<std::uint8_t*>(data + offsets.outputPayload),
                                 limit.events, limit.payload, outputEvents, outputBytes);
        if (result != Status::ok)
            outputEvents = outputBytes = 0;
        h.outputEvents = outputEvents;
        h.outputBytes = outputBytes;
        h.audioStatus = std::uint32_t(result);
        h.audioState.store(2, std::memory_order_release);
    }
    void obey(AudioCommand request) noexcept {
        Status status = Status::ok;
        if (request == AudioCommand::pause)
            paused = true;
        else if (request == AudioCommand::resume)
            paused = false;
        else if (request == AudioCommand::stop) {
            paused = true;
            if (plugin.lifecycle() == PluginLifecycle::processing)
                status = plugin.stop();
        } else if (request == AudioCommand::start) {
            const auto phase = plugin.lifecycle();
            status = phase == PluginLifecycle::active       ? plugin.start()
                     : phase == PluginLifecycle::processing ? Status::ok
                                                            : Status::not_prepared;
            paused = false; // A block sent while not processing is answered not_prepared.
        }
        commandStatus.store(std::uint32_t(status), std::memory_order_release);
        command.store(std::uint32_t(AudioCommand::none), std::memory_order_release);
        acknowledged.release();
    }
    void audioLoop() noexcept {
        raiseAudioThreadPriority();
        constexpr unsigned spinLimit = 256; // A few microseconds, then sleep.
        unsigned spins = 0;
        auto parentChecked = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point shutdownSeen{};
        bool closing = false;
        for (;;) {
            const auto request = AudioCommand(command.load(std::memory_order_acquire));
            if (request != AudioCommand::none) {
                obey(request);
                if (request == AudioCommand::quit)
                    return;
                continue;
            }
            const bool stopping = h.shutdown.load(std::memory_order_acquire) != 0;
            if (!paused && !stopping && h.audioState.load(std::memory_order_acquire) == 1) {
                audio();
                h.heartbeat.fetch_add(1, std::memory_order_relaxed);
                spins = 0;
                continue;
            }
            if (spins < spinLimit) {
                ++spins;
                relax();
                continue;
            }
            // Idle housekeeping, never between two back-to-back blocks.
            h.heartbeat.fetch_add(1, std::memory_order_relaxed);
            const auto now = std::chrono::steady_clock::now();
            if (stopping) {
                // The main thread normally stops this thread right after a shutdown
                // request. If it cannot (modal loop, hang) the process must not
                // outlive its parent or a close(): leave without unwinding.
                if (!closing) {
                    closing = true;
                    shutdownSeen = now;
                    controlWake.signal();
                } else if (now - shutdownSeen >= std::chrono::seconds(2))
                    std::_Exit(83);
            } else if (now - parentChecked >= std::chrono::milliseconds(250)) {
                parentChecked = now;
                if (!parentRunning(parent)) {
                    h.shutdown.store(1, std::memory_order_release);
                    controlWake.signal();
                    continue;
                }
            }
            // Publish the sleeping flag, re-check both inboxes, then wait. The client
            // wakes this thread only while the flag is set; the timeout is a safety
            // net and paces the heartbeat while no audio arrives.
            const auto token = audioWake.token();
            h.audioSleeping.store(1, std::memory_order_seq_cst);
            if (command.load(std::memory_order_seq_cst) == std::uint32_t(AudioCommand::none) &&
                (paused || stopping || h.audioState.load(std::memory_order_seq_cst) != 1))
                audioWake.wait(token, 20);
            h.audioSleeping.store(0, std::memory_order_release);
            spins = 0;
        }
    }

    // ---- main thread ----
    Status audioCall(AudioCommand request) noexcept {
        command.store(std::uint32_t(request), std::memory_order_seq_cst);
        audioWake.signal();
        // The audio thread may be inside a block. Native messages keep flowing so a
        // plugin whose audio code synchronously messages its own UI cannot deadlock.
        while (!acknowledged.try_acquire_for(std::chrono::milliseconds(1)))
            pumpNativeMessages();
        return Status(commandStatus.load(std::memory_order_acquire));
    }
    // From any lifecycle back to processing, with the audio thread paused on entry
    // and released on exit whatever happens.
    Status bringUp() noexcept {
        Status result = Status::ok;
        if (plugin.lifecycle() == PluginLifecycle::initialized)
            result = plugin.activate(options);
        if (result == Status::ok && plugin.lifecycle() == PluginLifecycle::active &&
            !describe(plugin, h, limit.channels, false))
            result = Status::channel_mismatch; // Ports changed: this mapping cannot serve.
        if (result != Status::ok) {
            (void)audioCall(AudioCommand::resume);
            return result;
        }
        return audioCall(AudioCommand::start);
    }
    // save, load and restart are driven from the instance's actual lifecycle, so a
    // command that failed half-way never leaves the worker unable to run the next.
    Status reconfigure(bool load) noexcept {
        const auto bytes = h.controlBytes;
        if (load && bytes > limit.stateBytes)
            return Status::block_too_large;
        const auto stopped = audioCall(AudioCommand::stop);
        if (stopped != Status::ok) {
            (void)audioCall(AudioCommand::resume);
            return stopped;
        }
        if (plugin.lifecycle() == PluginLifecycle::active) {
            const auto deactivated = plugin.deactivate();
            if (deactivated != Status::ok) {
                (void)bringUp();
                return deactivated;
            }
        }
        Status result = Status::ok;
        if (load)
            result = plugin.loadState(
                {reinterpret_cast<const std::uint8_t*>(data + offsets.control), bytes});
        const auto refreshed = plugin.refresh();
        auto up = bringUp();
        if (up != Status::ok && plugin.lifecycle() == PluginLifecycle::initialized) {
            // Activation can fail once right after a state change; try to get audio
            // back now instead of waiting for the next control command.
            (void)audioCall(AudioCommand::pause);
            up = bringUp();
        }
        return result != Status::ok ? result : refreshed != Status::ok ? refreshed : up;
    }
    Status save() noexcept {
        // Parking is enough: no stop()/start() cycle, DSP histories are kept.
        (void)audioCall(AudioCommand::pause);
        std::vector<std::uint8_t> state;
        const auto result =
            detail::PluginInstanceAccess::saveParked(plugin, state, limit.stateBytes);
        if (result == Status::ok) {
            h.controlBytes = std::uint32_t(state.size());
            if (!state.empty())
                std::memcpy(data + offsets.control, state.data(), state.size());
        }
        if (plugin.lifecycle() == PluginLifecycle::processing)
            (void)audioCall(AudioCommand::resume);
        else
            (void)bringUp(); // Recover from an earlier failed load/restart if possible.
        return result;
    }
    Status control() noexcept {
        const auto request = Command(h.command);
        Status result = Status::unsupported_format;
        if (request == Command::save)
            return save();
        if (request == Command::load || request == Command::restart)
            return reconfigure(request == Command::load);
        if (request == Command::openEditor) {
#if defined(_WIN32)
            if (h.windowApi != std::uint32_t(PluginWindowApi::win32) &&
                !(h.windowApi == std::uint32_t(PluginWindowApi::unspecified) && h.floating && !h.windowHandle))
                return Status::unsupported_backend;
#else
            if (!h.floating || h.windowHandle)
                return Status::unsupported_backend;
#endif
            if (h.windowApi > std::uint32_t(PluginWindowApi::unspecified) || h.floating > 1)
                return Status::invalid_argument;
            PluginEditorSize size;
            result = plugin.openEditor({PluginWindowApi(h.windowApi),
                                        std::uintptr_t(h.windowHandle), bool(h.floating), h.scale},
                                       size);
            if (result == Status::ok) {
                h.width = size.width;
                h.height = size.height;
            }
        } else if (request == Command::resizeEditor) {
            PluginEditorSize size{h.width, h.height};
            result = plugin.resizeEditor(size);
            if (result == Status::ok) {
                h.width = size.width;
                h.height = size.height;
            }
        } else if (request == Command::scaleEditor)
            result = plugin.scaleEditor(h.scale);
        else if (request == Command::showEditor)
            result = h.boolean <= 1 ? plugin.showEditor(h.boolean != 0) : Status::invalid_argument;
        else if (request == Command::focusEditor)
            result = h.boolean <= 1 ? plugin.focusEditor(h.boolean != 0) : Status::invalid_argument;
        else if (request == Command::keyEditor) {
            if (h.keyCharacter > 65535 || h.keyModifiers > 15 || h.keyDown > 1 ||
                h.keyCode > std::uint32_t(PluginEditorKeyCode::f24))
                return Status::invalid_argument;
            result =
                plugin.keyEditor({std::uint16_t(h.keyCharacter), PluginEditorKeyCode(h.keyCode),
                                  std::uint16_t(h.keyModifiers), h.keyDown != 0});
        } else if (request == Command::wheelEditor)
            result = plugin.wheelEditor(h.wheel);
        else if (request == Command::closeEditor)
            result = plugin.closeEditor();
        return result;
    }

  public:
    Server(Header& header, std::byte* memory, const Limits& fixed,
           const PluginPrepareOptions& prepared, PluginInstance& instance, Wake& audio,
           Wake& control)
        : h(header), data(memory), limit(fixed), offsets(layout(header)), parent(header.parentPid),
          options(prepared),
          plugin(instance), audioWake(audio), controlWake(control) {}
    ~Server() {
        finish();
    }
    Status prepare() noexcept {
        try {
            auto r = incoming.prepare(limit.events, limit.payload);
            if (r != Status::ok)
                return r;
            r = outgoing.prepare(limit.events, limit.payload);
            if (r != Status::ok)
                return r;
            inputPointers.resize(limit.channels);
            outputPointers.resize(limit.channels);
            auto* input = reinterpret_cast<const T*>(data + offsets.input);
            auto* output = reinterpret_cast<T*>(data + offsets.output);
            for (std::size_t ch = 0; ch < limit.channels; ++ch) {
                inputPointers[ch] = input + ch * limit.frames;
                outputPointers[ch] = output + ch * limit.frames;
            }
            std::size_t channel = 0;
            for (std::uint32_t i = 0; i < h.inputCount; ++i) {
                inputs.emplace_back(inputPointers.data() + channel, h.inputChannels[i],
                                    limit.frames);
                channel += h.inputChannels[i];
            }
            channel = 0;
            for (std::uint32_t i = 0; i < h.outputCount; ++i) {
                outputs.emplace_back(outputPointers.data() + channel, h.outputChannels[i],
                                     limit.frames);
                channel += h.outputChannels[i];
            }
            thread = std::thread([this] { audioLoop(); });
            return audioCall(AudioCommand::start);
        } catch (const std::bad_alloc&) {
            return Status::allocation_failed;
        } catch (...) {
            return Status::io_error;
        }
    }
    void run() noexcept {
        auto published = plugin.timing();
        auto pumped = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        while (!h.shutdown.load(std::memory_order_acquire)) {
            const auto token = controlWake.token();
            if (h.controlState.load(std::memory_order_acquire) == 1) {
                h.controlStatus = std::uint32_t(control());
                published = plugin.timing(); // describe() published it when it ran.
                publishTiming(h, published);
                h.controlState.store(2, std::memory_order_release);
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - pumped >= std::chrono::milliseconds(4)) {
                pumped = now;
                std::uint32_t requests = 0;
                (void)plugin.pumpMainThread(requests);
                if (requests)
                    h.requests.fetch_or(requests, std::memory_order_release);
                // Latency/tail can change while running; publish only on change.
                const auto timing = plugin.timing();
                if (!sameTiming(timing, published)) {
                    published = timing;
                    publishTiming(h, timing);
                }
            }
            pumpNativeMessages();
            if (h.controlState.load(std::memory_order_acquire) != 1)
                controlWake.waitOrMessages(token, 8);
        }
    }
    // Stops the plugin on its audio thread and joins that thread. Idempotent.
    void finish() noexcept {
        if (!thread.joinable())
            return;
        (void)audioCall(AudioCommand::stop);
        (void)audioCall(AudioCommand::quit);
        thread.join();
    }
};
} // namespace
int runPluginWorker(std::string_view mappingName) noexcept {
    using namespace detail::wire;
    Mapping memory;
    Header* header = nullptr;
    try {
        auto r = memory.open(mappingName);
        if (r != Status::ok)
            return 80;
        auto& h = *reinterpret_cast<Header*>(memory.data);
        header = &h;
        const auto fail = [&](Status status) {
            h.failure.store(std::uint32_t(status), std::memory_order_relaxed);
            h.boot.store(2, std::memory_order_release);
            return 81;
        };
        if (!valid(h) || h.boot.load() || h.pluginPath.back() != 0 || h.pluginId.back() != 0)
            return fail(Status::incompatible_version);
        // One private copy of the launch configuration for the whole run.
        const Limits fixed = limits(h);
        const PluginPrepareOptions options{h.sampleRate,       h.minimumFrames,
                                           fixed.frames,       fixed.events,
                                           bool(fixed.flags & 1), bool(fixed.flags & 2)};
        Wake audioWake, controlWake;
        if (audioWake.open(mappingName, "-audio", h.audioWake) != Status::ok ||
            controlWake.open(mappingName, "-control", h.controlWake) != Status::ok)
            return fail(Status::io_error);
#if defined(_WIN32)
        struct ComScope {
            HRESULT status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            ~ComScope() {
                if (SUCCEEDED(status))
                    CoUninitialize();
            }
        } com;
        if (FAILED(com.status))
            return fail(Status::unsupported_backend);
#endif
        std::unique_ptr<PluginProvider> provider;
        r = createPluginProvider(PluginFormat(h.format), provider);
        if (r != Status::ok)
            return fail(r);
        const auto path = std::filesystem::path(
            std::u8string_view(reinterpret_cast<const char8_t*>(h.pluginPath.data())));
        PluginInstance plugin;
        PluginHostCallbacks callbacks;
        callbacks.context = &h;
        callbacks.resizeEditor = [](void* context, PluginEditorSize size) noexcept {
            auto& header = *static_cast<Header*>(context);
            header.requestedWidth.store(size.width, std::memory_order_relaxed);
            header.requestedHeight.store(size.height, std::memory_order_release);
            header.requests.fetch_or(pluginEditorResize, std::memory_order_release);
            return false; // Parent UI must decide after poll; never acknowledge a resize not
                          // applied.
        };
        r = provider->create(path, h.pluginId.data(), callbacks, plugin);
        if (r != Status::ok)
            return fail(r);
        r = plugin.activate(options);
        if (r != Status::ok)
            return fail(r);
        if (!describe(plugin, h, fixed.channels, true))
            return fail(Status::channel_mismatch);
        const auto serve = [&](auto& server) {
            const auto started = server.prepare(); // Buffers, audio thread, plugin start.
            if (started != Status::ok) {
                server.finish();
                return fail(started);
            }
            h.boot.store(1, std::memory_order_release);
            server.run();
            server.finish();
            return 0;
        };
        int code = 0;
        if (fixed.flags & 1) {
            Server<double> server(h, memory.data, fixed, options, plugin, audioWake, controlWake);
            code = serve(server);
        } else {
            Server<float> server(h, memory.data, fixed, options, plugin, audioWake, controlWake);
            code = serve(server);
        }
        if (plugin.lifecycle() == PluginLifecycle::processing)
            (void)plugin.stop();
        if (plugin.lifecycle() == PluginLifecycle::active)
            (void)plugin.deactivate();
        (void)plugin.close();
        return code;
    } catch (...) {
        if (header) {
            header->failure.store(std::uint32_t(Status::io_error), std::memory_order_relaxed);
            header->boot.store(2, std::memory_order_release);
        }
        return 82;
    }
}
} // namespace cyaudio
