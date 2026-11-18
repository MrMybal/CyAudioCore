#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cyaudio/CyAudioCore.h>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
void assertNoRealtimeAllocations(void (*)(void*) noexcept, void*);
#if !defined(__ANDROID__)
namespace {
using namespace cydsp;
using namespace cyaudio;
void require(bool v, const std::source_location where = std::source_location::current()) {
    if (!v)
        throw std::runtime_error("plugin worker line " + std::to_string(where.line()));
}
void ok(Status r, const std::source_location where = std::source_location::current()) {
    require(r == Status::ok, where);
}
template <class T> void run(const std::filesystem::path& directory) {
    const auto executable = std::filesystem::absolute(directory /
#if defined(_WIN32)
                                                      "cydsp_plugin_worker_fixture.exe"
#else
                                                      "cydsp_plugin_worker_fixture"
#endif
    );
    const auto path = std::filesystem::absolute(directory / "cydsp_fixture.clap");
    PluginWorker worker;
    PluginWorkerOptions options;
    options.processing.maximumFrames = 64;
    options.processing.useFloat64 = std::same_as<T, double>;
    ok(worker.launch(executable, PluginFormat::clap, path, "org.cydsp.fixture", options));
    require(worker.info()->inputChannels == std::vector<std::uint32_t>{2, 1});
    require(worker.info()->pipelineLatencyFrames == 64 &&
            worker.info()->pluginTiming.latencyFrames == 17);
    AudioBuffer<T> main, side, out;
    ok(main.resize(2, 64));
    ok(side.resize(1, 64));
    ok(out.resize(2, 64));
    for (std::size_t ch = 0; ch < 2; ++ch)
        std::fill(main.block().channel(ch).begin(), main.block().channel(ch).end(), T{2});
    std::fill(side.block().channel(0).begin(), side.block().channel(0).end(), T{.25});
    std::array<AudioBlock<const T>, 2> inputs{main.block(), side.block()};
    std::array<AudioBlock<T>, 1> outputs{out.block()};
    PluginEventBuffer events, result;
    ok(events.prepare(64, 16000));
    ok(result.prepare(64, 16000));
    std::vector<std::uint8_t> sysex(12000, 0x12);
    sysex.front() = 0xf0;
    sysex.back() = 0xf7;
    PluginEvent event;
    event.kind = PluginEventKind::sysex;
    event.port = 0;
    event.bytes = sysex;
    ok(events.add(event));
    const std::array<std::uint8_t, 3> note{0x90, 62, 100};
    PluginEvent midi;
    midi.kind = PluginEventKind::midi1;
    midi.port = 0;
    midi.bytes = note;
    ok(events.add(midi));
    PluginProcess<T> call{inputs, outputs, 64, &events, &result};
    PluginWorkerBlockResult observed;
    ok(worker.process(call, observed));
    require(observed.submitted && !observed.received);
    require(out.block().channel(0)[0] == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    struct Real {
        PluginWorker* worker;
        PluginProcess<T>* call;
        PluginWorkerBlockResult result{};
        Status status = Status::not_prepared;
    } real{&worker, &call};
    assertNoRealtimeAllocations(
        [](void* p) noexcept {
            auto& r = *static_cast<Real*>(p);
            r.status = r.worker->process(*r.call, r.result);
        },
        &real);
    ok(real.status);
    require(real.result.received && real.result.receivedSequence == 1 &&
            out.block().channel(0)[0] == T{1.25});
    require(result.size() == 2 && result.event(0)->bytes.size() == 12000);
    // The next reply's SysEx will not fit, but the following note and all audio fit.
    ok(result.prepare(2, note.size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    ok(worker.process(call, observed));
    require(observed.received && !observed.missedDeadline && out.block().channel(0)[0] == T{1.25});
    require(result.size() == 1 && std::ranges::equal(result.event(0)->bytes, note));
    std::uint32_t flags = 0;
    ok(worker.poll(flags));
    require((flags & pluginEventsDropped) != 0);
    ok(worker.poll(flags));
    require((flags & pluginEventsDropped) == 0);
    std::vector<std::uint8_t> state;
    ok(worker.saveState(state));
    require(state.size() == 8);
    ok(worker.loadState(state));
#if defined(_WIN32)
    struct Parent {
        HWND h = nullptr;
        ~Parent() {
            if (h)
                DestroyWindow(h);
        }
    } parent;
    parent.h = CreateWindowExW(0, L"STATIC", L"CyDSP hidden worker parent", WS_OVERLAPPED, 0, 0,
                               640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(parent.h != nullptr);
    PluginEditorSize size;
    ok(worker.openEditor(
        {PluginWindowApi::win32, reinterpret_cast<std::uintptr_t>(parent.h), false, 1}, size));
    require(size.width == 320 && GetWindow(parent.h, GW_CHILD) != nullptr);
    size = {250, 150};
    ok(worker.resizeEditor(size));
    ok(worker.scaleEditor(2));
    ok(worker.showEditor(false));
    ok(worker.showEditor(true));
    ok(worker.closeEditor());
    require(GetWindow(parent.h, GW_CHILD) == nullptr);
#endif
    // Controlled slow foreign plugin proves mailbox saturation never waits.
    events.reset();
    const std::uint8_t delayed[]{0xf0, 0x7d, 0x44, 0xf7};
    event.bytes = delayed;
    ok(events.add(event));
    ok(worker.process(call, observed));
    const auto started = std::chrono::steady_clock::now();
    const auto pending = worker.process(call, observed);
    require(pending == Status::would_block || pending == Status::ok);
    if (pending == Status::would_block)
        require(observed.missedDeadline && !observed.received && out.block().channel(0)[0] == 0);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    std::cout << "Worker nonblocking mailbox call us=" << elapsed
              << " missed=" << observed.missedDeadline << "\n";
    ok(worker.saveState(state)); // Control may wait; audio may not.
    events.reset();
    event = {};
    event.kind = PluginEventKind::noteOn;
    event.port = 0;
    event.channel = 15;
    event.key = 127;
    event.value = 1;
    ok(events.add(event));
    ok(worker.process(call, observed)); // The fixture exits its OWN worker process.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (worker.poll() == Status::ok && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(worker.poll() == Status::disconnected);
    require(worker.process(call, observed) == Status::disconnected &&
            out.block().channel(0)[0] == 0);
    ok(worker.launch(executable, PluginFormat::clap, path, "org.cydsp.fixture", options));
    ok(worker.loadState(state));
    events.reset();
    ok(worker.process(call, observed));
    require(observed.submitted && !observed.received);
    worker.close();
}
// Continuous real-time cadence: about two seconds of 128-frame blocks at 48 kHz.
// The driver yields instead of sleeping so its own timer granularity is not measured,
// and it always leaves the worker one full period after each submission.
void cadence(const std::filesystem::path& directory) {
    const auto executable = std::filesystem::absolute(directory /
#if defined(_WIN32)
                                                      "cydsp_plugin_worker_fixture.exe"
#else
                                                      "cydsp_plugin_worker_fixture"
#endif
    );
    PluginWorker worker;
    PluginWorkerOptions options;
    options.processing.maximumFrames = 128;
    ok(worker.launch(executable, PluginFormat::clap,
                     std::filesystem::absolute(directory / "cydsp_fixture.clap"),
                     "org.cydsp.fixture", options));
    AudioBuffer<float> main, side, out;
    ok(main.resize(2, 128));
    ok(side.resize(1, 128));
    ok(out.resize(2, 128));
    for (std::size_t ch = 0; ch < 2; ++ch)
        std::fill(main.block().channel(ch).begin(), main.block().channel(ch).end(), 2.f);
    std::fill(side.block().channel(0).begin(), side.block().channel(0).end(), .25f);
    std::array<AudioBlock<const float>, 2> inputs{main.block(), side.block()};
    std::array<AudioBlock<float>, 1> outputs{out.block()};
    PluginProcess<float> call{inputs, outputs, 128};
    constexpr unsigned blocks = 750;
    const auto period = std::chrono::nanoseconds(2666667);
    unsigned missed = 0, received = 0, wrong = 0;
    for (unsigned i = 0; i < blocks; ++i) {
        const auto started = std::chrono::steady_clock::now();
        PluginWorkerBlockResult observed;
        const auto status = worker.process(call, observed);
        require(status == Status::ok || status == Status::would_block);
        missed += observed.missedDeadline ? 1u : 0u;
        received += observed.received ? 1u : 0u;
        if (observed.received && out.block().channel(0)[127] != 1.25f)
            ++wrong;
        else if (!observed.received && out.block().channel(0)[127] != 0)
            ++wrong;
        while (std::chrono::steady_clock::now() - started < period)
            std::this_thread::yield();
    }
    std::cout << "Worker continuous cadence blocks=" << blocks << " frames=128 received="
              << received << " missed=" << missed << "\n";
    require(wrong == 0 && received + missed + 1 == blocks);
    // Loose on purpose (machine load varies). The polling worker this replaced
    // missed more than 99 % of the blocks at this cadence.
    require(missed <= blocks / 2);
    require(worker.poll() == Status::ok);
    worker.close();
}
std::filesystem::path workerExecutable(const std::filesystem::path& directory) {
    return std::filesystem::absolute(directory /
#if defined(_WIN32)
                                     "cydsp_plugin_worker_fixture.exe"
#else
                                     "cydsp_plugin_worker_fixture"
#endif
    );
}
// One CLAP fixture worker with 64-frame float blocks and the buffers to drive it.
struct ClapWorker {
    PluginWorker worker;
    AudioBuffer<float> main, side, out;
    std::array<AudioBlock<const float>, 2> inputs;
    std::array<AudioBlock<float>, 1> outputs;
    explicit ClapWorker(const std::filesystem::path& directory, unsigned heartbeatMs = 3000) {
        PluginWorkerOptions options;
        options.processing.maximumFrames = 64;
        options.heartbeatTimeoutMs = heartbeatMs;
        ok(worker.launch(workerExecutable(directory), PluginFormat::clap,
                         std::filesystem::absolute(directory / "cydsp_fixture.clap"),
                         "org.cydsp.fixture", options));
        ok(main.resize(2, 64));
        ok(side.resize(1, 64));
        ok(out.resize(2, 64));
        for (std::size_t ch = 0; ch < 2; ++ch)
            std::fill(main.block().channel(ch).begin(), main.block().channel(ch).end(), 2.f);
        std::fill(side.block().channel(0).begin(), side.block().channel(0).end(), .25f);
        inputs = {main.block(), side.block()};
        outputs = {out.block()};
    }
    // Submits one block, lets the worker answer, and returns the status of the block
    // that collects an answer. A slow machine only costs extra rounds, not a failure.
    Status exchange(PluginWorkerBlockResult& observed, const PluginEventBuffer* events = nullptr) {
        PluginProcess<float> call{inputs, outputs, 64, events};
        auto status = worker.process(call, observed);
        call.inputEvents = nullptr;
        for (unsigned round = 0; round < 25; ++round) {
            if (status != Status::ok && status != Status::would_block)
                return status;
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            status = worker.process(call, observed);
            if (observed.received)
                return status;
        }
        return status;
    }
};
std::vector<std::uint8_t> gainState(double gain) {
    std::vector<std::uint8_t> state(sizeof(double));
    std::memcpy(state.data(), &gain, sizeof(double));
    return state;
}
// A load whose re-activation fails must not leave the worker unable to process or to
// run the next control command.
void recovery(const std::filesystem::path& directory) {
    ClapWorker fixture(directory);
    auto& worker = fixture.worker;
    PluginWorkerBlockResult observed;
    ok(fixture.exchange(observed));
    require(observed.received && fixture.out.block().channel(0)[0] == 1.25f);
    // The fixture fails the first activation after this state; the worker retries.
    ok(worker.loadState(gainState(1.984375)));
    ok(fixture.exchange(observed));
    require(observed.received && fixture.out.block().channel(0)[0] == 4.21875f);
    // Two failures: the load reports the error and the plugin stays inactive...
    require(worker.loadState(gainState(1.96875)) != Status::ok);
    require(worker.poll() == Status::ok);
    // ...but blocks are still answered instead of timing out,
    require(fixture.exchange(observed) == Status::not_prepared && !observed.received &&
            fixture.out.block().channel(0)[0] == 0);
    // and the next control commands work and bring audio back.
    std::vector<std::uint8_t> state;
    ok(worker.saveState(state));
    require(state == gainState(1.96875));
    ok(fixture.exchange(observed));
    require(observed.received && fixture.out.block().channel(0)[0] == 4.1875f);
    ok(worker.loadState(gainState(.5)));
    ok(fixture.exchange(observed));
    require(observed.received && fixture.out.block().channel(0)[0] == 1.25f);
    worker.close();
}
// A blocked plugin main thread (modal menu, file dialog, drag loop) must stop neither
// the audio mailbox nor the heartbeat: both live on the worker's audio thread.
void modalMain(const std::filesystem::path& directory) {
    ClapWorker fixture(directory, 250);
    auto& worker = fixture.worker;
    PluginEventBuffer events;
    ok(events.prepare(4, 16));
    const std::uint8_t trigger[]{0xf0, 0x7d, 0x46, 0xf7};
    PluginEvent event;
    event.kind = PluginEventKind::sysex;
    event.port = 0;
    event.bytes = trigger;
    ok(events.add(event));
    PluginWorkerBlockResult observed;
    ok(fixture.exchange(observed, &events)); // The fixture main thread now sleeps 600 ms.
    require(observed.received);
    constexpr unsigned blocks = 150;
    unsigned missed = 0, received = 0;
    PluginProcess<float> call{fixture.inputs, fixture.outputs, 64};
    for (unsigned i = 0; i < blocks; ++i) {
        const auto started = std::chrono::steady_clock::now();
        const auto status = worker.process(call, observed);
        require(status == Status::ok || status == Status::would_block);
        missed += observed.missedDeadline ? 1u : 0u;
        received += observed.received ? 1u : 0u;
        require(worker.poll() == Status::ok); // Heartbeat timeout is 250 ms.
        while (std::chrono::steady_clock::now() - started < std::chrono::milliseconds(2))
            std::this_thread::yield();
    }
    std::cout << "Worker blocked main thread blocks=" << blocks << " received=" << received
              << " missed=" << missed << "\n";
    require(missed <= blocks / 2);
    std::vector<std::uint8_t> state;
    ok(worker.saveState(state)); // Control waits for the main thread and still completes.
    require(state.size() == 8);
    worker.close();
}
// The plugin asks for a restart from its audio thread; restart() serves it and the
// latency reported by the client follows.
void restartTiming(const std::filesystem::path& directory) {
    ClapWorker fixture(directory);
    auto& worker = fixture.worker;
    require(worker.info()->pluginTiming.latencyKnown &&
            worker.info()->pluginTiming.latencyFrames == 17);
    PluginEventBuffer events;
    ok(events.prepare(4, 16));
    const std::uint8_t trigger[]{0xf0, 0x7d, 0x45, 0xf7};
    PluginEvent event;
    event.kind = PluginEventKind::sysex;
    event.port = 0;
    event.bytes = trigger;
    ok(events.add(event));
    PluginWorkerBlockResult observed;
    ok(fixture.exchange(observed, &events));
    require(observed.received);
    std::uint32_t requests = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!(requests & pluginRestart) && std::chrono::steady_clock::now() < deadline) {
        std::uint32_t flags = 0;
        ok(worker.poll(flags));
        requests |= flags;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require((requests & pluginRestart) != 0);
    require(worker.info()->pluginTiming.latencyFrames == 17);
    ok(worker.restart());
    require(worker.info()->pluginTiming.latencyKnown &&
            worker.info()->pluginTiming.latencyFrames == 23);
    ok(fixture.exchange(observed));
    require(observed.received && fixture.out.block().channel(0)[0] == 1.25f);
    worker.close();
}
// The child owns a writable view of the mapping. The fixture executable acts as a
// hostile worker (see PluginWorkerMain.cpp) and the client must keep to the bounds
// it computed itself.
void tampered(const std::filesystem::path& directory) {
    struct Mode {
        explicit Mode(const char* value) {
            set(value);
        }
        ~Mode() {
            set("");
        }
        static void set(const char* value) {
#if defined(_WIN32)
            require(_putenv_s("CYAUDIO_TEST_WORKER_TAMPER", value) == 0);
#else
            require((*value ? setenv("CYAUDIO_TEST_WORKER_TAMPER", value, 1)
                            : unsetenv("CYAUDIO_TEST_WORKER_TAMPER")) == 0);
#endif
        }
    };
    const auto plugin = std::filesystem::absolute(directory / "cydsp_fixture.clap");
    PluginWorkerOptions options;
    options.processing.maximumFrames = 64;
    {
        // A self-consistent but different geometry reported at boot.
        Mode mode("boot");
        PluginWorker worker;
        require(worker.launch(workerExecutable(directory), PluginFormat::clap, plugin,
                              "org.cydsp.fixture", options) == Status::invalid_argument);
        require(worker.info() == nullptr);
    }
    Mode mode("run");
    PluginWorker worker;
    ok(worker.launch(workerExecutable(directory), PluginFormat::clap, plugin, "org.cydsp.fixture",
                     options));
    require(worker.info()->inputChannels == std::vector<std::uint32_t>{2} &&
            worker.info()->outputChannels == std::vector<std::uint32_t>{2} &&
            worker.info()->pipelineLatencyFrames == 64);
    AudioBuffer<float> in, out;
    ok(in.resize(2, 64));
    ok(out.resize(2, 64));
    std::array<AudioBlock<const float>, 1> inputs{in.block()};
    std::array<AudioBlock<float>, 1> outputs{out.block()};
    PluginEventBuffer returned;
    ok(returned.prepare(16, 64));
    PluginProcess<float> call{inputs, outputs, 64, nullptr, &returned};
    PluginWorkerBlockResult observed;
    ok(worker.process(call, observed));
    // Oversized counts, a record outside the payload, an invalid status, a frame
    // beyond the block: each reply is refused and the block is silent.
    // (A reply that arrives late is stale and only counts as a missed block.)
    unsigned refused = 0;
    for (unsigned round = 0; round < 60 && refused < 4; ++round) {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        std::fill(out.block().channel(0).begin(), out.block().channel(0).end(), 7.f);
        const auto status = worker.process(call, observed);
        require(status == Status::invalid_argument || status == Status::would_block);
        refused += status == Status::invalid_argument ? 1u : 0u;
        require(!observed.received && returned.size() == 0 && out.block().channel(0)[0] == 0 &&
                out.block().channel(0)[63] == 0);
    }
    require(refused == 4);
    // The header now claims frames=65536: the client still accepts its own block size.
    require(worker.process(PluginProcess<float>{inputs, outputs, 65, nullptr, &returned},
                           observed) == Status::block_too_large);
    // A state reply larger than the control area must not be copied.
    std::vector<std::uint8_t> state{0xa5};
    require(worker.saveState(state) == Status::invalid_argument &&
            state == std::vector<std::uint8_t>{0xa5});
    std::vector<std::uint8_t> large(options.maximumStateBytes + 1);
    require(worker.loadState(large) == Status::block_too_large);
    worker.close();
    // A worker that stops answering: either control wait terminates the child, and
    // later calls report disconnected instead of a transition that never ends.
    Mode silent("mute");
    options.controlTimeoutMs = 150;
    for (const bool audioPending : {true, false}) {
        PluginWorker stuck;
        ok(stuck.launch(workerExecutable(directory), PluginFormat::clap, plugin,
                        "org.cydsp.fixture", options));
        if (audioPending)
            ok(stuck.process(call, observed)); // Never answered: audio stays in flight.
        require(stuck.saveState(state) == Status::timed_out);
        require(stuck.saveState(state) == Status::disconnected);
        require(stuck.closeEditor() == Status::disconnected);
        require(stuck.poll() == Status::disconnected);
        require(stuck.process(call, observed) == Status::disconnected);
    }
}
#if defined(_WIN32)
void vstWorker(const std::filesystem::path& directory) {
    PluginWorker worker;
    PluginWorkerOptions options;
    options.processing.maximumFrames = 64;
    ok(worker.launch(std::filesystem::absolute(directory / "cydsp_plugin_worker_fixture.exe"),
                     PluginFormat::vst3,
                     std::filesystem::absolute(directory / "cydsp_vst_fixture.vst3"),
                     "vst3:43a6be13274d4ef1812b472fd0a0b713", options));
    struct Parent {
        HWND h = nullptr;
        ~Parent() {
            if (h)
                DestroyWindow(h);
        }
    } parent;
    parent.h = CreateWindowExW(0, L"STATIC", L"CyDSP hidden VST3 worker", WS_OVERLAPPED, 0, 0, 640,
                               480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(parent.h != nullptr);
    PluginEditorSize size;
    ok(worker.openEditor(
        {PluginWindowApi::win32, reinterpret_cast<std::uintptr_t>(parent.h), false, 1}, size));
    const auto requested = worker.requestedEditorSize();
    require(requested.width == 400 && requested.height == 200);
    size = {400, 200};
    ok(worker.resizeEditor(size));
    ok(worker.scaleEditor(2));
    ok(worker.focusEditor(true));
    ok(worker.keyEditor({std::uint16_t('a'), PluginEditorKeyCode::none, editorControl, true}));
    ok(worker.wheelEditor(1));
    AudioBuffer<float> main, side, out;
    ok(main.resize(2, 64));
    ok(side.resize(1, 64));
    ok(out.resize(2, 64));
    std::array<AudioBlock<const float>, 2> inputs{main.block(), side.block()};
    std::array<AudioBlock<float>, 1> outputs{out.block()};
    PluginEventBuffer recorded;
    ok(recorded.prepare());
    PluginProcess<float> call{inputs, outputs, 64, nullptr, &recorded};
    PluginWorkerBlockResult observed;
    ok(worker.process(call, observed));
    require(observed.submitted);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    ok(worker.process(call, observed));
    require(observed.received && recorded.size() >= 3);
    require(recorded.event(0)->kind == PluginEventKind::gestureBegin);
    require(recorded.event(1)->kind == PluginEventKind::parameterValue &&
            recorded.event(1)->value == .75);
    require(recorded.event(2)->kind == PluginEventKind::gestureEnd);
    require(GetWindow(parent.h, GW_CHILD) != nullptr);
    ok(worker.closeEditor());
    require(GetWindow(parent.h, GW_CHILD) == nullptr);
    std::vector<std::uint8_t> state;
    ok(worker.saveState(state));
    ok(worker.loadState(state));
    worker.close();
}
#endif
} // namespace
#endif
void pluginWorkerRegression(const std::filesystem::path& directory) {
#if !defined(__ANDROID__)
    run<float>(directory);
    run<double>(directory);
    cadence(directory);
    modalMain(directory);
    recovery(directory);
    restartTiming(directory);
    tampered(directory);
#if defined(_WIN32)
    vstWorker(directory);
#endif
    std::cout
        << "PASS plugin separate process/IPC/state/editor/late blocks/crash/restart/realtime/"
           "cadence/modal main thread/recovery/tamper\n";
#else
    (void)directory;
    std::cout << "SKIP plugin worker process: Android application service provider required\n";
#endif
}
