#include "plugins/PluginInternal.h"
#include <array>
#include <chrono>
#include <cmath>
#include <cyaudio/CyAudioCore.h>
#include <source_location>
#include <stdexcept>
#include <thread>
void assertNoRealtimeAllocations(void (*)(void*) noexcept, void*);
namespace {
using namespace cyaudio;
std::filesystem::path workerPath(const std::filesystem::path& dir) {
#if defined(_WIN32)
    return dir / "cydsp_plugin_worker_fixture.exe";
#else
    return dir / "cydsp_plugin_worker_fixture";
#endif
}
void require(bool v, std::source_location p = std::source_location::current()) {
    if (!v)
        throw std::runtime_error("plugin sleep contract line " + std::to_string(p.line()));
}
void ok(Status s, std::source_location p = std::source_location::current()) {
    require(s == Status::ok, p);
}
struct Backend final : PluginBackend {
    PluginDescription d;
    PluginSleepHint hint = PluginSleepHint::afterTail;
    PluginTiming time{3, 13, true, true, false};
    std::atomic<unsigned> generation{0};
    unsigned calls = 0, starts = 0, stops = 0;
    bool event = false, fail = false, running = false, failStart = false,
         requestDuringProcess = false;
    Backend() {
        d.inputs.push_back({1, 1});
        d.outputs.push_back({2, 1});
        d.float64 = true;
    }
    const PluginDescription& description() const noexcept override {
        return d;
    }
    Status activate(const PluginPrepareOptions&) noexcept override {
        return Status::ok;
    }
    Status deactivate() noexcept override {
        return Status::ok;
    }
    Status start() noexcept override {
        if (failStart)
            return Status::io_error;
        if (running)
            return Status::invalid_argument;
        running = true;
        ++starts;
        return Status::ok;
    }
    void stop() noexcept override {
        running = false;
        ++stops;
    }
    Status reset() noexcept override {
        return Status::ok;
    }
    PluginTiming timing() const noexcept override {
        return time;
    }
    PluginSleepHint sleepHint() const noexcept override {
        return hint;
    }
    std::uint32_t wakeGeneration() const noexcept override {
        return generation.load();
    }
    template <Sample T> Status run(const PluginProcess<T>& p) noexcept {
        if (!running)
            return Status::not_prepared;
        ++calls;
        if (requestDuringProcess) {
            ++generation;
            requestDuringProcess = false;
        }
        for (std::size_t i = 0; i < p.frames; ++i)
            p.outputs[0].channel(0)[i] = p.inputs.empty() ? T{} : p.inputs[0].channel(0)[i];
        if (event && p.outputEvents) {
            PluginEvent e;
            e.kind = PluginEventKind::gestureBegin;
            (void)p.outputEvents->add(e);
        }
        return fail ? Status::io_error : Status::ok;
    }
    Status process(const PluginProcess<float>& p) noexcept override {
        return run(p);
    }
    Status process(const PluginProcess<double>& p) noexcept override {
        return run(p);
    }
};
template <Sample T> void native() {
    PluginInstance p;
    auto backend = std::make_unique<Backend>();
    auto& b = *backend;
    ok(PluginInstance::adopt(std::move(backend), p));
    PluginPrepareOptions options;
    options.maximumFrames = 64;
    options.sleep.quietFrames = 8;
    options.useFloat64 = std::same_as<T, double>;
    auto invalid = options;
    invalid.sleep.silenceThreshold = std::numeric_limits<double>::quiet_NaN();
    require(p.activate(invalid) == Status::invalid_spec);
    ok(p.activate(options));
    ok(p.start());
    AudioBuffer<T> in, out;
    ok(in.resize(1, 80));
    ok(out.resize(1, 80));
    std::fill(in.block().channel(0).begin(), in.block().channel(0).end(), T{});
    const std::array<AudioBlock<const T>, 1> inputs{in.block().subBlock(4, 64)};
    const std::array<AudioBlock<T>, 1> outputs{out.block().subBlock(4, 64)};
    PluginEventBuffer events, result;
    ok(events.prepare());
    ok(result.prepare());
    PluginProcess<T> call{inputs, outputs, 8, &events, &result};
    for (int i = 0; i < 2; ++i) {
        ok(p.process(call));
        require(!p.sleepInfo().sleeping);
    }
    ok(p.process(call));
    require(p.sleepInfo().sleeping && b.calls == 3 && b.stops == 1);
    struct Audit {
        PluginInstance* p;
        PluginProcess<T>* call;
        Status r;
    };
    Audit audit{&p, &call, Status::ok};
    assertNoRealtimeAllocations(
        [](void* c) noexcept {
            auto& a = *static_cast<Audit*>(c);
            a.r = a.p->process(*a.call);
        },
        &audit);
    ok(audit.r);
    require(b.calls == 3 && p.sleepInfo().skippedBlocks == 1);
    call.frames = 1;
    ok(p.process(call));
    require(b.calls == 3);
    in.block().channel(0)[4] = T(.25);
    assertNoRealtimeAllocations(
        [](void* c) noexcept {
            auto& a = *static_cast<Audit*>(c);
            a.r = a.p->process(*a.call);
        },
        &audit);
    ok(audit.r);
    require(b.calls == 4 && out.block().channel(0)[4] == T(.25) && b.starts == 2);
    in.block().channel(0)[4] = T{};
    call.frames = 64;
    ok(p.process(call));
    require(p.sleepInfo().sleeping);
    p.wake();
    ok(p.process(call));
    require(b.calls == 6);
    ++b.generation;
    ok(p.process(call));
    require(b.calls == 7);
    PluginEvent e;
    e.id = 7;
    e.frame = 3;
    ok(events.add(e));
    ok(p.process(call));
    require(!p.sleepInfo().sleeping && b.calls == 8);
    events.reset();
    ok(p.process(call));
    require(p.sleepInfo().sleeping);
    PluginTransport transport;
    transport.hasSongFrame = true;
    transport.playing = true;
    call.transport = &transport;
    ok(p.process(call));
    require(b.calls == 10);
    transport.songFrame = 64;
    ok(p.process(call));
    require(b.calls == 10);
    transport.songFrame = 128;
    ok(p.process(call));
    require(b.calls == 10);
    transport.songFrame = 1024;
    ok(p.process(call));
    require(b.calls == 11);
    call.transport = nullptr;
    p.wake();
    b.time.infiniteTail = true;
    std::uint32_t flags;
    ok(p.pumpMainThread(flags));
    for (int i = 0; i < 5; ++i)
        ok(p.process(call));
    require(!p.sleepInfo().sleeping);
    b.time.infiniteTail = false;
    b.time.tailKnown = false;
    ok(p.pumpMainThread(flags));
    for (int i = 0; i < 5; ++i)
        ok(p.process(call));
    require(!p.sleepInfo().sleeping);
    b.hint = PluginSleepHint::whenQuiet;
    b.event = true;
    call.outputEvents = nullptr;
    for (int i = 0; i < 5; ++i)
        ok(p.process(call));
    require(!p.sleepInfo().sleeping);
    b.event = false;
    b.fail = true;
    require(p.process(call) == Status::io_error);
    require(!p.sleepInfo().sleeping);
    b.fail = false;
    b.hint = PluginSleepHint::sleep;
    ok(p.process(call));
    require(p.sleepInfo().sleeping);
    in.block().channel(0)[4] = std::numeric_limits<T>::quiet_NaN();
    ok(p.process(call));
    require(!p.sleepInfo().sleeping);
    in.block().channel(0)[4] = T{};
    ok(p.process(call));
    require(p.sleepInfo().sleeping);
    b.failStart = true;
    p.wake();
    std::fill_n(out.block().channel(0).begin() + 4, call.frames, T(-9));
    require(p.process(call) == Status::io_error);
    require(out.block().channel(0)[4] == T{} && p.sleepInfo().sleeping);
    b.failStart = false;
    b.requestDuringProcess = true;
    ok(p.process(call));
    require(!p.sleepInfo().sleeping);
    ok(p.process(call));
    require(p.sleepInfo().sleeping);
    const auto stopped = b.stops;
    ok(p.stop());
    require(b.stops == stopped);
    ok(p.deactivate());
    ok(p.close());
}
template <class F> void await(F&& f) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!f()) {
        require(std::chrono::steady_clock::now() < end);
        std::this_thread::yield();
    }
}
void policies() {
    for (int policy = 0; policy < 4; ++policy) {
        PluginInstance p;
        auto backend = std::make_unique<Backend>();
        auto& b = *backend;
        b.hint = policy < 2 ? PluginSleepHint::sleep : PluginSleepHint::keepProcessing;
        b.time.tailKnown = false;
        if (policy == 3)
            b.d.inputs.clear();
        ok(PluginInstance::adopt(std::move(backend), p));
        PluginPrepareOptions o;
        o.maximumFrames = 16;
        o.sleep.quietFrames = 8;
        o.sleep.mode = policy == 0   ? PluginSleepMode::disabled
                       : policy >= 2 ? PluginSleepMode::whenQuiet
                                     : PluginSleepMode::automatic;
        o.offline = policy == 1;
        ok(p.activate(o));
        ok(p.start());
        AudioBuffer<float> in, out;
        ok(in.resize(1, 16));
        ok(out.resize(1, 16));
        std::fill_n(in.block().channel(0).begin(), 16, 0.f);
        std::array<AudioBlock<const float>, 1> inputs{in.block()};
        std::array<AudioBlock<float>, 1> outputs{out.block()};
        PluginProcess<float> call{policy == 3 ? std::span<const AudioBlock<const float>>{}
                                              : std::span<const AudioBlock<const float>>(inputs),
                                  outputs, 16};
        if (policy == 2)
            call.frames = 4; // Drain known latency even with an unknown tail.
        for (int i = 0; i < 10; ++i)
            ok(p.process(call));
        require(p.sleepInfo().sleeping == (policy >= 2));
        require(b.calls == (policy == 2 ? 3u : policy == 3 ? 1u : 10u));
        ok(p.stop());
        ok(p.deactivate());
    }
}
template <Sample T> void vstEffect(const std::filesystem::path& dir) {
    PluginIsolationOptions isolation;
    isolation.workerExecutable = workerPath(dir);
    isolation.scheduling = PluginScheduling::sameCycle;
    std::unique_ptr<PluginProvider> provider;
    ok(createPluginProvider(PluginFormat::vst3, provider, isolation));
    std::vector<PluginDescriptor> descriptions;
    ok(provider->scan(dir / "cyaudio_sleep_vst.vst3", descriptions));
    require(descriptions.size() == 1);
    PluginInstance p;
    ok(provider->create(dir / "cyaudio_sleep_vst.vst3", descriptions[0].id, {}, p));
    require(p.description()->noteInputs.empty() && p.description()->noteOutputs.empty());
    PluginPrepareOptions options;
    options.maximumFrames = 64;
    options.useFloat64 = std::same_as<T, double>;
    options.sleep.quietFrames = 8;
    ok(p.activate(options));
    ok(p.start());
    AudioBuffer<T> main, side, out;
    ok(main.resize(2, 64));
    ok(side.resize(1, 64));
    ok(out.resize(2, 64));
    for (unsigned ch = 0; ch < 2; ++ch)
        std::fill_n(main.block().channel(ch).begin(), 64, T{});
    std::fill_n(side.block().channel(0).begin(), 64, T{});
    std::array<AudioBlock<const T>, 2> inputs{main.block(), side.block()};
    std::array<AudioBlock<T>, 1> outputs{out.block()};
    PluginProcess<T> call{inputs, outputs, 16};
    const auto cycle = [&] {
        std::uint64_t ticket;
        ok(p.submit(call, ticket));
        Status s;
        await([&] {
            s = p.collect(call, ticket);
            return s != Status::would_block;
        });
        ok(s);
    };
    for (int i = 0; i < 5; ++i) {
        cycle();
        require(!p.sleepInfo().sleeping);
    }
    cycle();
    require(p.sleepInfo().sleeping);
    const auto baseline = p.sleepInfo().processedBlocks;
    for (int i = 0; i < 100; ++i)
        cycle();
    require(p.sleepInfo().processedBlocks == baseline);
    side.block().channel(0)[4] = T(.25);
    cycle();
    require(out.block().channel(0)[4] == T(.25) && !p.sleepInfo().sleeping);
    side.block().channel(0)[4] = T{};
    call.frames = 64;
    cycle();
    require(!p.sleepInfo().sleeping);
    cycle();
    require(p.sleepInfo().sleeping);
    p.wake();
    cycle();
    require(p.sleepInfo().processedBlocks == baseline + 4);
    ok(p.stop());
    ok(p.deactivate());
}
template <Sample T> void isolated(const std::filesystem::path& dir, PluginScheduling scheduling) {
    PluginIsolationOptions isolation;
    isolation.workerExecutable = workerPath(dir);
    isolation.scheduling = scheduling;
    std::unique_ptr<PluginProvider> provider;
    ok(createPluginProvider(PluginFormat::clap, provider, isolation));
    PluginInstance p;
    ok(provider->create(dir / "cyaudio_sleep_fixture.clap", "org.cyaudio.sleep", {}, p));
    PluginPrepareOptions options;
    options.maximumFrames = 64;
    options.useFloat64 = std::same_as<T, double>;
    options.sleep.quietFrames = 8;
    ok(p.activate(options));
    ok(p.start());
    AudioBuffer<T> in, out;
    ok(in.resize(2, 64));
    ok(out.resize(2, 64));
    for (unsigned ch = 0; ch < 2; ++ch)
        std::fill(in.block().channel(ch).begin(), in.block().channel(ch).end(), T{});
    std::array<AudioBlock<const T>, 1> inputs{in.block()};
    std::array<AudioBlock<T>, 1> outputs{out.block()};
    PluginEventBuffer events, result;
    ok(events.prepare());
    ok(result.prepare());
    PluginProcess<T> call{inputs, outputs, 64, &events, &result};
    const auto cycle = [&] {
        if (scheduling == PluginScheduling::sameCycle) {
            std::uint64_t ticket;
            ok(p.submit(call, ticket));
            Status s;
            await([&] {
                s = p.collect(call, ticket);
                return s != Status::would_block;
            });
            ok(s);
        } else
            ok(p.process(call));
    };
    cycle();
    await([&] { return p.sleepInfo().sleeping; });
    cycle();
    const auto baseline = p.sleepInfo();
    for (int i = 0; i < 100; ++i)
        cycle();
    require(p.sleepInfo().processedBlocks == baseline.processedBlocks &&
            p.sleepInfo().skippedBlocks >= baseline.skippedBlocks + 100);
    if (scheduling == PluginScheduling::sameCycle) {
        Status s;
        await([&] {
            s = p.flushAudio(events, result);
            return s != Status::would_block;
        });
        ok(s);
        cycle();
        require(p.sleepInfo().processedBlocks == baseline.processedBlocks + 1);
        auto* ext = p.clapExtensions();
        await([&] {
            s = ext->setAudioPortActive(true, 0, true, sizeof(T) * 8);
            return s != Status::would_block;
        });
        ok(s);
        cycle();
        require(p.sleepInfo().processedBlocks == baseline.processedBlocks + 2);
    }
    double value;
    ok(p.parameterValue(3, value));
    cycle();
    await([&] { return p.sleepInfo().sleeping; });
    if (scheduling == PluginScheduling::pipelined)
        cycle();
    require(out.block().channel(0)[0] == T(.25));
    if (scheduling == PluginScheduling::sameCycle) {
        call.frames = 7;
        cycle();
        require(p.sleepInfo().sleeping);
        PluginEvent e;
        e.frame = 2;
        e.id = 0;
        e.value = 1;
        ok(events.add(e));
        cycle();
        events.reset();
        require(!p.sleepInfo().sleeping);
        const auto n = p.sleepInfo().processedBlocks;
        cycle();
        require(p.sleepInfo().processedBlocks == n + 1);
        e.value = 0;
        ok(events.add(e));
        cycle();
        events.reset();
        cycle();
        require(p.sleepInfo().sleeping);
        std::uint64_t ticket;
        ok(p.submit(call, ticket));
        ok(p.discard(ticket));
        require(p.collect(call, ticket) == Status::invalid_argument);
        cycle();
        ok(p.stop());
        ok(p.start());
        cycle();
        require(p.sleepInfo().processedBlocks == 1 && p.sleepInfo().skippedBlocks == 0);
    }
    ok(p.stop());
    ok(p.deactivate());
    ok(p.close());
}
} // namespace
void pluginSleepRegression(const std::filesystem::path& directory) {
    native<float>();
    native<double>();
    policies();
    vstEffect<float>(directory);
    vstEffect<double>(directory);
    isolated<float>(directory, PluginScheduling::sameCycle);
    isolated<double>(directory, PluginScheduling::sameCycle);
    isolated<float>(directory, PluginScheduling::pipelined);
    isolated<double>(directory, PluginScheduling::pipelined);
}
