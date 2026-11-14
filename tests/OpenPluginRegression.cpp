#include "../src/plugins/PluginInternal.h"
#include "cyaudio/CyAudioCore.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <thread>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif
void assertNoRealtimeAllocations(void (*)(void*) noexcept, void*);
namespace {
using namespace cydsp;
using namespace cyaudio;
void require(bool b, const std::source_location loc = std::source_location::current()) {
    if (!b)
        throw std::runtime_error("open plugin regression line " + std::to_string(loc.line()));
}
void ok(Status r, const std::source_location loc = std::source_location::current()) {
    require(r == Status::ok, loc);
}
template <Sample T>
void run(const std::filesystem::path& path, PluginFormat format, std::uint32_t gainId) {
    std::unique_ptr<PluginProvider> provider;
    ok(cyaudio::detail::createInProcessPluginProvider(format, provider));
    std::vector<PluginDescriptor> list;
    ok(provider->scan(path, list));
    require(list.size() == 1);
    PluginInstance plugin;
    ok(provider->create(path, list[0].id, {}, plugin));
    require(plugin.description()->inputs.size() == (format == PluginFormat::lv2 ? 2u : 1u) &&
            plugin.description()->inputs[0].channels == 2);
    std::vector<std::uint8_t> state;
    ok(plugin.saveState(state));
    PluginPrepareOptions options;
    options.maximumFrames = 64;
    options.maximumEvents = 128;
    options.useFloat64 = std::same_as<T, double>;
    ok(plugin.activate(options));
    ok(plugin.start());
    AudioBuffer<T> input, output, side;
    ok(side.resize(1, 64));
    std::fill(side.block().channel(0).begin(), side.block().channel(0).end(), T{.125});
    ok(input.resize(2, 64));
    ok(output.resize(2, 64));
    for (auto ch : {0u, 1u})
        std::fill(input.block().channel(ch).begin(), input.block().channel(ch).end(), T{2});
    std::array<AudioBlock<const T>, 2> allIns{input.block(), side.block()};
    std::span<const AudioBlock<const T>> ins(allIns.data(), format == PluginFormat::lv2 ? 2u : 1u);
    std::array<AudioBlock<T>, 1> outs{output.block()};
    PluginEventBuffer events, returned;
    ok(events.prepare(128, 16000));
    ok(returned.prepare(128, 16000));
    PluginEvent e;
    e.kind = PluginEventKind::parameterValue;
    e.id = gainId;
    e.value = .5;
    ok(events.add(e));
    std::array<std::uint8_t, 3> note{0x91, 64, 100};
    e = {};
    e.kind = PluginEventKind::midi1;
    e.port = 0;
    e.frame = 7;
    e.bytes = note;
    ok(events.add(e));
    e = {};
    e.kind = PluginEventKind::parameterValue;
    e.id = gainId;
    e.value = 1;
    e.frame = 17;
    ok(events.add(e));
    std::vector<std::uint8_t> sysex(12000, 0x12);
    sysex.front() = 0xf0;
    sysex.back() = 0xf7;
    e = {};
    e.kind = PluginEventKind::sysex;
    e.port = 0;
    e.frame = 31;
    e.bytes = sysex;
    ok(events.add(e));
    PluginTransport time;
    time.hasTempo = true;
    time.tempo = 130;
    time.playing = true;
    struct Audio {
        PluginInstance* instance;
        PluginProcess<T> call;
        Status status = Status::not_prepared;
    } audio{&plugin, {ins, outs, 64, &events, &returned, &time}};
    assertNoRealtimeAllocations(
        [](void* p) noexcept {
            auto& a = *static_cast<Audio*>(p);
            a.status = a.instance->process(a.call);
        },
        &audio);
    ok(audio.status);
    const auto bias = format == PluginFormat::lv2 ? .25 : .25;
    for (std::size_t i = 0; i < 64; ++i)
        require(std::abs(double(output.block().channel(0)[i]) - ((i < 17 ? 1. : 2.) + bias)) <
                1e-6);
    require(returned.size() == 2 && returned.event(0)->frame == 7 &&
            returned.event(1)->frame == 31);
    require(returned.event(1)->bytes.size() == 12000);
    events.reset();
    // Worker completion is asynchronous. A fixed sleep races the scheduler,
    // especially under instrumentation; drain responses until observable completion.
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    double worked=0;
    for(;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ok(plugin.process(PluginProcess<T>{ins, outs, 64, nullptr, &returned}));
        ok(plugin.stop());
        if(format!=PluginFormat::lv2)break;
        ok(plugin.parameterValue(8,worked)); // Main, active but audio owner stopped.
        if(worked>0 || std::chrono::steady_clock::now()>=deadline)break;
        ok(plugin.start());
    }
    ok(plugin.deactivate());
    if(format==PluginFormat::lv2)require(worked>0);
    ok(plugin.loadState(state));
    double value = 0;
    ok(plugin.parameterValue(gainId, value));
    require(value == .5);
    // Sparse slider8 survives the upstream serialization fix.
    if (format == PluginFormat::jsfx) {
        ok(plugin.parameterValue(7, value));
        require(value == .25);
    }
    if (format == PluginFormat::jsfx) {
        auto corrupt = state;
        corrupt.resize(3);
        require(plugin.loadState(corrupt) != Status::ok);
    }
    ok(plugin.close());
}
// An output event the host cannot place is dropped and reported; the block is kept.
void rejectedOutput(const std::filesystem::path& path, PluginFormat format) {
    const bool lv2 = format == PluginFormat::lv2;
    std::unique_ptr<PluginProvider> provider;
    ok(cyaudio::detail::createInProcessPluginProvider(format, provider));
    PluginInstance plugin;
    ok(provider->create(path, lv2 ? "urn:cydsp:lv2-fixture" : "jsfx", {}, plugin));
    PluginPrepareOptions options;
    options.maximumFrames = 64;
    ok(plugin.activate(options));
    ok(plugin.start());
    AudioBuffer<float> input, output, side;
    ok(side.resize(1, 64));
    std::fill(side.block().channel(0).begin(), side.block().channel(0).end(), .125f);
    ok(input.resize(2, 64));
    ok(output.resize(2, 64));
    for (auto ch : {0u, 1u})
        std::fill(input.block().channel(ch).begin(), input.block().channel(ch).end(), 2.f);
    std::array<AudioBlock<const float>, 2> allIns{input.block(), side.block()};
    std::span<const AudioBlock<const float>> ins(allIns.data(), lv2 ? 2u : 1u);
    std::array<AudioBlock<float>, 1> outs{output.block()};
    PluginEventBuffer events, returned;
    ok(events.prepare(16, 64));
    ok(returned.prepare(16, 64));
    PluginEvent trigger;
    trigger.kind = PluginEventKind::parameterValue;
    trigger.id = lv2 ? 4u : 7u;
    trigger.value = lv2 ? 1.75 : .5;
    ok(events.add(trigger));
    std::uint32_t requests = 0;
    ok(plugin.pumpMainThread(requests));
    require((requests & pluginEventsDropped) == 0);
    ok(plugin.process(PluginProcess<float>{ins, outs, 64, &events, &returned}));
    const float expected = lv2 ? 3.75f : 1.5f;
    for (std::size_t i = 0; i < 64; ++i)
        require(output.block().channel(0)[i] == expected &&
                output.block().channel(1)[i] == expected);
    require(returned.size() == 0);
    ok(plugin.pumpMainThread(requests));
    require((requests & pluginEventsDropped) != 0);
    ok(plugin.pumpMainThread(requests));
    require((requests & pluginEventsDropped) == 0);
    // A very quiet note-on stays a note-on: MIDI velocity 0 would mean note-off. Both
    // fixtures echo their MIDI input.
    events.reset();
    PluginEvent quiet;
    quiet.kind = PluginEventKind::noteOn;
    quiet.port = 0;
    quiet.channel = 1;
    quiet.key = 60;
    quiet.value = .001;
    ok(events.add(quiet));
    ok(plugin.process(PluginProcess<float>{ins, outs, 64, &events, &returned}));
    require(returned.size() == 1 && returned.event(0)->bytes.size() == 3 &&
            returned.event(0)->bytes[0] == 0x91 && returned.event(0)->bytes[1] == 60 &&
            returned.event(0)->bytes[2] == 1);
    ok(plugin.stop());
    ok(plugin.deactivate());
    ok(plugin.close());
}
// gain.jsfx: @init history=0; @sample history+=1; @serialize file_var(0,history).
float jsfxHistory(const std::vector<std::uint8_t>& state) {
    const auto word = [&](std::size_t at) {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i)
            value |= std::uint32_t(state[at + i]) << (8 * i);
        return value;
    };
    require(state.size() >= 16);
    const auto sliders = word(8), bytes = word(12);
    require(bytes == 4 && state.size() == 16 + std::size_t(sliders) * 12 + 4);
    float history = 0;
    std::memcpy(&history, state.data() + 16 + std::size_t(sliders) * 12, 4);
    return history;
}
// Restored @serialize state must survive the @init of activation and the first block,
// including a stopped-to-playing transport transition, and later reactivations.
void jsfxSerialize(const std::filesystem::path& path) {
    std::unique_ptr<PluginProvider> provider;
    ok(cyaudio::detail::createInProcessPluginProvider(PluginFormat::jsfx, provider));
    PluginPrepareOptions options;
    options.maximumFrames = 64;
    AudioBuffer<float> input, output;
    ok(input.resize(2, 64));
    ok(output.resize(2, 64));
    std::array<AudioBlock<const float>, 1> ins{input.block()};
    std::array<AudioBlock<float>, 1> outs{output.block()};
    PluginTransport playing;
    playing.playing = true;
    const auto blocks = [&](PluginInstance& plugin, unsigned count, const PluginTransport* time) {
        ok(plugin.activate(options));
        ok(plugin.start());
        for (unsigned i = 0; i < count; ++i)
            ok(plugin.process(PluginProcess<float>{ins, outs, 64, nullptr, nullptr, time}));
        ok(plugin.stop());
        ok(plugin.deactivate());
    };
    std::vector<std::uint8_t> saved, after;
    {
        PluginInstance first;
        ok(provider->create(path, "jsfx", {}, first));
        blocks(first, 2, &playing);
        ok(first.saveState(saved));
        require(jsfxHistory(saved) == 128);
        ok(first.close());
    }
    PluginInstance second;
    ok(provider->create(path, "jsfx", {}, second));
    ok(second.loadState(saved));
    blocks(second, 1, &playing); // First block after restore, transport starting.
    ok(second.saveState(after));
    require(jsfxHistory(after) == 192);
    blocks(second, 1, nullptr); // Reactivation keeps the evolved state, not the loaded blob.
    ok(second.saveState(after));
    require(jsfxHistory(after) == 256);
    ok(second.loadState(saved)); // An explicit load still replaces it.
    blocks(second, 1, &playing);
    ok(second.saveState(after));
    require(jsfxHistory(after) == 192);
    ok(second.close());
}
#if !defined(__ANDROID__)
void isolated(const std::filesystem::path& directory, PluginFormat format) {
    auto path = directory / (format == PluginFormat::lv2 ? "cydsp_fixture.lv2" : "gain.jsfx");
    auto exe = directory /
#if defined(_WIN32)
               "cydsp_plugin_worker_fixture.exe";
#else
               "cydsp_plugin_worker_fixture";
#endif
    PluginWorker w;
    PluginWorkerOptions o;
    o.processing.maximumFrames = 64;
    ok(w.launch(exe, format, path, format == PluginFormat::lv2 ? "urn:cydsp:lv2-fixture" : "jsfx",
                o));
    AudioBuffer<float> input, output, side;
    ok(side.resize(1, 64));
    ok(input.resize(2, 64));
    ok(output.resize(2, 64));
    for (auto ch : {0u, 1u})
        std::fill(input.block().channel(ch).begin(), input.block().channel(ch).end(), 2.f);
    std::array<AudioBlock<const float>, 2> allIns{input.block(), side.block()};
    std::span<const AudioBlock<const float>> ins(allIns.data(),
                                                 format == PluginFormat::lv2 ? 2u : 1u);
    std::array<AudioBlock<float>, 1> outs{output.block()};
    PluginProcess<float> call{ins, outs, 64};
    PluginWorkerBlockResult block;
    ok(w.process(call, block));
    require(block.submitted && !block.received);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ok(w.process(call, block));
    require(block.received && std::abs(output.block().channel(0)[0] -
                                       (format == PluginFormat::lv2 ? 1.125f : 1.25f)) < 1e-6f);
    if (format == PluginFormat::lv2) {
        // The fixture reports another latency once its gain is 1.5. No control call
        // is involved: the worker publishes the change and poll() picks it up.
        require(w.info()->pluginTiming.latencyKnown && w.info()->pluginTiming.latencyFrames == 11);
        PluginEventBuffer events;
        ok(events.prepare(4, 0));
        PluginEvent gain;
        gain.kind = PluginEventKind::parameterValue;
        gain.id = 4;
        gain.value = 1.5;
        ok(events.add(gain));
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // Previous block answered.
        ok(w.process(PluginProcess<float>{ins, outs, 64, &events}, block));
        require(block.submitted);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (w.info()->pluginTiming.latencyFrames != 13 &&
               std::chrono::steady_clock::now() < deadline) {
            ok(w.poll());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(w.info()->pluginTiming.latencyKnown && w.info()->pluginTiming.latencyFrames == 13);
    }
#if defined(_WIN32)
    auto parent = CreateWindowExW(0, L"STATIC", L"CyDSP worker editor", WS_OVERLAPPED, 0, 0, 640,
                                  480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(parent != nullptr);
    PluginEditorSize size;
    auto status = w.openEditor(
        {PluginWindowApi::win32, reinterpret_cast<std::uintptr_t>(parent), false, 1}, size);
    if (status != Status::ok) {
        DestroyWindow(parent);
        ok(status);
    }
    require(GetWindow(parent, GW_CHILD) != nullptr);
    size = {450, 230};
    ok(w.resizeEditor(size));
    ok(w.closeEditor());
    require(!GetWindow(parent, GW_CHILD));
    DestroyWindow(parent);
#endif
    std::vector<std::uint8_t> saved;
    ok(w.saveState(saved));
    ok(w.loadState(saved));
    w.close();
}
#endif
#if defined(_WIN32)
void editor(const std::filesystem::path& directory, PluginFormat format) {
    std::unique_ptr<PluginProvider> provider;
    ok(cyaudio::detail::createInProcessPluginProvider(format, provider));
    PluginInstance p;
    PluginEditorSize requested;
    PluginHostCallbacks cb{&requested,
                           [](void* ctx, PluginEditorSize s) noexcept {
                               *static_cast<PluginEditorSize*>(ctx) = s;
                               return true;
                           },
                           nullptr};
    ok(provider->create(directory /
                            (format == PluginFormat::lv2 ? "cydsp_fixture.lv2" : "gain.jsfx"),
                        format == PluginFormat::lv2 ? "urn:cydsp:lv2-fixture" : "jsfx", cb, p));
    auto parent = CreateWindowExW(0, L"STATIC", L"CyDSP LV2 host", WS_OVERLAPPED, 0, 0, 640, 480,
                                  nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(parent != nullptr);
    PluginEditorSize size;
    auto r = p.openEditor(
        {PluginWindowApi::win32, reinterpret_cast<std::uintptr_t>(parent), false, 1}, size);
    if (r != Status::ok) {
        DestroyWindow(parent);
        ok(r);
    }
    require(size.height == 200 && GetWindow(parent, GW_CHILD));
    require(format == PluginFormat::lv2 ? requested.width == 400 : size.width == 320);
    size = {500, 250};
    ok(p.resizeEditor(size));
    if (format == PluginFormat::jsfx) {
        ok(p.scaleEditor(1.5));
        ok(p.focusEditor(true));
        ok(p.wheelEditor(1));
        ok(p.keyEditor({std::uint16_t('a'), PluginEditorKeyCode::none, 0, true}));
        std::uint32_t flags = 0;
        ok(p.pumpMainThread(flags));
        PluginPrepareOptions options;
        options.maximumFrames = 64;
        ok(p.activate(options));
        ok(p.start());
        AudioBuffer<float> input, output, side;
        ok(side.resize(1, 64));
        ok(input.resize(2, 64));
        ok(output.resize(2, 64));
        std::array<AudioBlock<const float>, 2> allIns{input.block(), side.block()};
        std::span<const AudioBlock<const float>> ins(allIns.data(),
                                                     format == PluginFormat::lv2 ? 2u : 1u);
        std::array<AudioBlock<float>, 1> outs{output.block()};
        PluginEventBuffer events;
        ok(events.prepare());
        ok(p.process(PluginProcess<float>{ins, outs, 64, nullptr, &events}));
        require(events.size() > 0 && events.event(0)->id == 7 && events.event(0)->value == .75);
        // @gfx leaves slider8 out of range and automates the undeclared slider3. The
        // out-of-range edit is clamped, the undeclared one is not forwarded, and
        // neither may wedge the edit queue and silence every later block.
        std::fill(input.block().channel(0).begin(), input.block().channel(0).end(), 2.f);
        ok(p.keyEditor({std::uint16_t('b'), PluginEditorKeyCode::none, 0, true}));
        ok(p.pumpMainThread(flags));
        ok(p.process(PluginProcess<float>{ins, outs, 64, nullptr, &events}));
        require(events.size() == 1 && events.event(0)->id == 7 && events.event(0)->value == 1);
        require(output.block().channel(0)[63] == 2.f);
        ok(p.process(PluginProcess<float>{ins, outs, 64, nullptr, &events}));
        require(events.size() == 0 && output.block().channel(0)[63] == 2.f);
        ok(p.stop());
        double clamped = 0;
        ok(p.parameterValue(7, clamped));
        require(clamped == 1);
        ok(p.deactivate());
    }
    ok(p.closeEditor());
    require(!GetWindow(parent, GW_CHILD));
    DestroyWindow(parent);
    ok(p.close());
}
#endif
} // namespace
void openPluginRegression(const std::filesystem::path& directory) {
    for (auto format : {cyaudio::PluginFormat::lv2, cyaudio::PluginFormat::jsfx}) {
        auto path =
            directory / (format == cyaudio::PluginFormat::lv2 ? "cydsp_fixture.lv2" : "gain.jsfx");
        auto gain = format == cyaudio::PluginFormat::lv2 ? 4u : 0u;
        run<float>(path, format, gain);
        run<double>(path, format, gain);
        rejectedOutput(path, format);
        if (format == cyaudio::PluginFormat::jsfx)
            jsfxSerialize(path);
#if !defined(__ANDROID__)
        isolated(directory, format);
#endif
    }
#if defined(_WIN32)
    editor(directory, PluginFormat::lv2);
    editor(directory, PluginFormat::jsfx);
#endif
    std::cout << "PASS plugin LV2/JSFX audio/MIDI/automation/state/worker/realtime\n";
}
