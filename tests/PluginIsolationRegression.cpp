#include "../src/plugins/PluginRpc.h"
#include <array>
#include <chrono>
#include <cmath>
#include <cyaudio/CyAudioCore.h>
#include <cyaudio/plugins/ClapExtensions.h>
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
namespace {
using namespace cyaudio;
void require(bool v, const std::source_location at = std::source_location::current()) {
    if (!v)
        throw std::runtime_error("plugin isolation line " + std::to_string(at.line()));
}
void ok(Status s, const std::source_location at = std::source_location::current()) {
    require(s == Status::ok, at);
}
struct Instance {
    PluginInstance p;
    ~Instance() {
        if (p.lifecycle() == PluginLifecycle::processing)
            (void)p.stop();
        if (p.lifecycle() == PluginLifecycle::active)
            (void)p.deactivate();
    }
};
struct Mode {
    explicit Mode(const char* mode) {
#if defined(_WIN32)
        require(SetEnvironmentVariableA("CYAUDIO_TEST_ISOLATION", mode) != 0);
#else
        if (mode)
            require(setenv("CYAUDIO_TEST_ISOLATION", mode, 1) == 0);
        else
            require(unsetenv("CYAUDIO_TEST_ISOLATION") == 0);
#endif
    }
    ~Mode() {
#if defined(_WIN32)
        (void)SetEnvironmentVariableA("CYAUDIO_TEST_ISOLATION", nullptr);
#else
        (void)unsetenv("CYAUDIO_TEST_ISOLATION");
#endif
    }
};
PluginIsolationOptions settings(const std::filesystem::path& directory) {
    PluginIsolationOptions o;
    o.workerExecutable = std::filesystem::absolute(directory /
#if defined(_WIN32)
                                                   "cydsp_plugin_worker_fixture.exe"
#else
                                                   "cydsp_plugin_worker_fixture"
#endif
    );
#if defined(CYAUDIOCORE_WORKER_HELPER)
    o.workerExecutable = std::filesystem::path(CYAUDIOCORE_WORKER_HELPER);
#endif
    o.scheduling = PluginScheduling::sameCycle;
    o.maximumFrames = 128;
    o.maximumEvents = 32;
    return o;
}
template <Sample T> struct Blocks {
    AudioBuffer<T> main, side, out;
    std::array<AudioBlock<const T>, 2> inputs;
    std::array<AudioBlock<T>, 1> outputs;
    PluginEventBuffer events, reply;
    explicit Blocks(std::uint32_t n = 128) {
        ok(main.resize(2, n));
        ok(side.resize(1, n));
        ok(out.resize(2, n));
        inputs = {main.block(), side.block()};
        outputs = {out.block()};
        for (auto& b : outputs)
            for (std::size_t ch = 0; ch < b.channels(); ++ch)
                std::fill(b.channel(ch).begin(), b.channel(ch).end(), T(9));
        for (std::size_t ch = 0; ch < 2; ++ch)
            std::fill(main.block().channel(ch).begin(), main.block().channel(ch).end(), T(2));
        std::fill(side.block().channel(0).begin(), side.block().channel(0).end(), T(.25));
        ok(events.prepare(32, 1024));
        ok(reply.prepare(32, 1024));
    }
    PluginProcess<T> call(std::uint32_t n) {
        return {inputs, outputs, n, &events, &reply};
    }
};
template <class F> Status eventually(F&& f) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    Status s;
    do {
        s = f();
        if (s != Status::would_block)
            return s;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return Status::timed_out;
}
template <Sample T>
void exchange(PluginProvider& provider, const std::filesystem::path& path, const std::string& id) {
    Instance value;
    auto& p = value.p;
    ok(provider.create(path, id, {}, p));
    require(p.isolated());
    PluginPrepareOptions o;
    o.maximumFrames = 128;
    o.maximumEvents = 32;
    o.useFloat64 = std::same_as<T, double>;
    ok(p.activate(o));
    ok(p.start());
    Blocks<T> b;
    PluginEvent note;
    note.kind = PluginEventKind::noteOn;
    note.channel = note.port = 0;
    note.key = 60;
    note.value = 1;
    ok(b.events.add(note));
    note.key = 62;
    ok(b.events.add(note));
    ok(b.reply.prepare(1, 0));
    std::int64_t frame = 0;
    for (const auto n : {1u, 17u, 64u, 127u, 128u}) {
        auto block = b.call(n);
        block.steadyFrame = frame;
        frame += n;
        std::uint64_t ticket = 0;
        struct Submit {
            PluginInstance* p;
            PluginProcess<T>* block;
            std::uint64_t* ticket;
            Status status;
        } submit{&p, &block, &ticket, Status::invalid_argument};
        assertNoRealtimeAllocations(
            [](void* c) noexcept {
                auto& x = *static_cast<Submit*>(c);
                x.status = x.p->submit(*x.block, *x.ticket);
            },
            &submit);
        ok(submit.status);
        require(ticket != 0);
        // A second in-flight block must never overwrite the child's input.
        std::uint64_t another = 0;
        require(p.submit(block, another) == Status::invalid_argument); // Rewound timeline.
        auto nextBlock=block;nextBlock.steadyFrame=frame;
        require(p.submit(nextBlock, another) == Status::transition_in_progress);
        ok(eventually([&] {
            struct Collect {
                PluginInstance* p;
                PluginProcess<T>* block;
                std::uint64_t ticket;
                Status status;
            } c{&p, &block, ticket, Status::invalid_argument};
            assertNoRealtimeAllocations(
                [](void* context) noexcept {
                    auto& x = *static_cast<Collect*>(context);
                    x.status = x.p->collect(*x.block, x.ticket);
                },
                &c);
            return c.status;
        }));
        for (const auto& out : b.outputs)
            for (std::size_t ch = 0; ch < out.channels(); ++ch)
                for (auto s : out.channel(ch).first(n))
                    require(s == T(1.25));
        require(b.reply.size() == 1 && b.reply.event(0)->key == 60);
        require(p.collect(block, ticket) == Status::invalid_argument);
    }
    std::uint32_t flags = 0;
    ok(p.pumpMainThread(flags));
    require((flags & pluginEventsDropped) != 0);
    ok(p.pumpMainThread(flags));
    require((flags & pluginEventsDropped) == 0);
    b.events.reset();
    // Late replies are deliberately abandoned, then never played in a later cycle.
    const std::array<std::uint8_t, 4> delay{0xf0, 0x7d, 0x44, 0xf7};
    PluginEvent event;
    event.kind = PluginEventKind::sysex;
    event.port = 0;
    event.bytes = delay;
    ok(b.events.add(event));
    auto block = b.call(64);
    std::uint64_t ticket = 0;
    ok(p.submit(block, ticket));
    require(p.collect(block, ticket) == Status::would_block);
    for (auto s : b.out.block().channel(0).first(64))
        require(s == T{});
    ok(p.discard(ticket));
    b.events.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(180));
    ok(p.submit(block, ticket));
    ok(eventually([&] { return p.collect(block, ticket); }));
    require(b.reply.size() == 0);
    ok(p.stop());
    ok(p.deactivate());
}
void controls(const std::filesystem::path& directory, PluginIsolationOptions isolation) {
    const auto path = std::filesystem::absolute(directory / "cyaudio_clap_advanced.clap");
    isolation.clapThreadPool = true;
    std::unique_ptr<PluginProvider> provider;
    ok(createPluginProvider(provider, isolation));
    std::vector<PluginDescriptor> list;
    ok(provider->scan(path, list));
    require(list.size() == 1);
    struct Host {
        unsigned presets = 0, errors = 0, actions = 0, logs = 0;
    };
    Host host;
    ClapHostServices services;
    services.context = &host;
    services.track = ClapTrackInfo{};
    services.track->name = "Isolated";
    services.log = [](void* c, std::int32_t, std::string_view) noexcept {
        ++static_cast<Host*>(c)->logs;
    };
    services.presetLoaded = [](void* c, const ClapPresetLocation&) noexcept {
        ++static_cast<Host*>(c)->presets;
    };
    services.presetError = [](void* c, const ClapPresetLocation&, std::int32_t,
                              std::string_view) noexcept { ++static_cast<Host*>(c)->errors; };
    services.populateMenu = [](void*, ClapMenuTarget, std::vector<ClapMenuItem>& out) noexcept {
        try {
            out.push_back({ClapMenuItemKind::entry, "Host action", 88});
            return true;
        } catch (...) {
            return false;
        }
    };
    services.performMenu = [](void* c, ClapMenuTarget, std::uint32_t id) noexcept {
        if (id != 88)
            return false;
        ++static_cast<Host*>(c)->actions;
        return true;
    };
    PluginHostCallbacks callbacks;
    callbacks.clap = &services;
    Instance value;
    auto& p = value.p;
    ok(provider->create(path, list[0].id, callbacks, p));
    auto* ext = p.clapExtensions();
    require(ext && p.isolated());
    const auto caps = ext->capabilities();
    require(caps.audioConfigurations && caps.remoteControls && caps.stateContext &&
            caps.contextMenu);
    std::vector<ClapAudioConfiguration> configs;
    ok(ext->audioConfigurations(configs));
    require(configs.size() == 4);
    ok(ext->selectAudioConfiguration(12));
    require(p.description()->outputs[0].channels == 6);
    ok(ext->selectAudioConfiguration(13));
    require(p.description()->outputs[0].ambisonicKnown);
    ok(ext->selectAudioConfiguration(10));
    std::array<ClapAudioPortRequest, 2> ports;
    ports[0].input = true;
    ports[1].input = false;
    for (auto& x : ports) {
        x.channels = 1;
        x.type = "mono";
    }
    bool yes = false;
    ok(ext->canConfigureAudioPorts(ports, yes));
    require(yes);
    ok(ext->configureAudioPorts(ports));
    require(p.description()->inputs[0].channels == 1);
    ok(ext->selectAudioConfiguration(10));
    ok(ext->supportsSurround(3, yes));
    require(yes);
    ok(ext->supportsAmbisonic(1, 1, yes));
    require(yes);
    std::vector<ClapNoteName> names;
    ok(ext->noteNames(names));
    require(names.size() == 1 && names[0].name == "Kick");
    std::vector<ClapRemoteControlsPage> pages;
    ok(ext->remoteControls(pages));
    require(pages.size() == 1 && pages[0].id == 99);
    ClapTrackInfo track;
    track.name = "Updated";
    track.channels = 2;
    track.portType = "stereo";
    ok(ext->setTrackInfo(track));
    double v = 0;
    ok(ext->parameterFromText(7, "0.75", v));
    require(v == .75);
    require(ext->parameterFromText(7, "nan", v) == Status::invalid_argument);
    ok(ext->indicateMapping(7, true, nullptr, "Knob", "Control"));
    ok(ext->indicateAutomation(7, ClapAutomationIndication::recording));
    std::vector<std::uint8_t> state;
    ok(ext->saveState(ClapStateContext::preset, state, 1024));
    ok(ext->loadState(ClapStateContext::project, state));
    ok(ext->loadPreset({ClapPresetLocationKind::plugin, {}, "warm"}));
    require(ext->loadPreset({ClapPresetLocationKind::plugin, {}, "bad"}) == Status::io_error);
    std::vector<ClapMenuItem> menu;
    ok(ext->contextMenu({}, menu));
    require(menu.size() == 1 && menu[0].action == 55);
    ok(ext->performContextMenu({}, 55));
    require(host.actions == 1);
    PluginWindowApi api;
    bool floating = false;
    ok(ext->preferredEditorApi(api, floating));
    require(api == PluginWindowApi::unspecified && floating);
    PluginEditorSize size;
    ok(p.openEditor({api, 0, floating, 1}, size));
    ok(ext->suggestEditorTitle("Isolated fixture"));
    ok(p.closeEditor());
    std::uint32_t requests = 0;
    ok(p.pumpMainThread(requests));
    require(host.presets == 1 && host.errors == 1);
    PluginEventBuffer in, out;
    ok(in.prepare(32, 1024));
    ok(out.prepare(1, 0));
    PluginEvent param;
    param.id = 7;
    param.value = .25;
    ok(in.add(param));
    param.value = .75;
    ok(in.add(param));
    ok(p.flush(in, out));
    require(out.size() == 1 && out.event(0)->value == .25);
    ok(p.pumpMainThread(requests));
    require((requests & pluginEventsDropped) != 0);
    in.reset();
    ok(p.flush(in, out));
    require(out.size() == 0);
    ok(in.add(param));
    ok(p.flush(in, out));
    require(out.size() == 1 && out.event(0)->value == .75);
    PluginPrepareOptions spec;
    spec.maximumFrames = 128;
    spec.maximumEvents = 32;
    ok(p.activate(spec));
    ClapVoiceInfo voices;
    ok(ext->voiceInfo(voices));
    require(voices.count == 8);
    ok(p.start());
    ok(out.prepare(32, 1024));
    ok(eventually([&] { return p.flushAudio(in, out); }));
    require(out.size() == 1 && out.event(0)->value == .75);
    AudioBuffer<float> input, output;
    ok(input.resize(2, 128));
    ok(output.resize(2, 128));
    for (std::size_t ch = 0; ch < 2; ++ch)
        std::fill(input.block().channel(ch).begin(), input.block().channel(ch).end(), 2.f);
    std::array<AudioBlock<const float>, 1> inputs{input.block()};
    std::array<AudioBlock<float>, 1> outputs{output.block()};
    PluginProcess<float> block{inputs, outputs, 17, &in, &out};
    std::uint64_t ticket = 0;
    ok(p.submit(block, ticket));
    ok(eventually([&] { return p.collect(block, ticket); }));
    require(output.block().channel(0)[0] == 3.f &&
            ext->lastProcessStatus() == ClapProcessStatus::continueProcessing);

    ok(eventually([&] { return ext->setAudioPortActive(true, 0, false, 32); }));
    ok(p.stop());
    ok(p.deactivate());
    ok(p.close());
    std::vector<ClapPresetProviderInfo> providers;
    ok(ClapPresetDiscovery::providers(path, providers, isolation));
    require(providers.size() == 1);
    ClapPresetDiscovery discovery;
    ok(discovery.open(path, providers[0].id, isolation));
    require(discovery.catalogue()->locations.size() == 1);
    std::vector<ClapPresetMetadata> presets;
    ok(discovery.metadata(ClapPresetLocationKind::plugin, {}, presets));
    require(presets.size() == 1 && presets[0].name == "Warm");
}
} // namespace
void pluginIsolationRegression(const std::filesystem::path& directory) {
    using namespace cyaudio;
    auto isolation = settings(directory);
    std::unique_ptr<PluginProvider> provider;
    require(createPluginProvider(provider) == Status::invalid_argument && !provider);
    ok(createPluginProvider(provider, isolation));
    const auto path = std::filesystem::absolute(directory / "cydsp_fixture.clap");
    std::vector<PluginDescriptor> list;
    require(provider->scan("relative.clap", list) == Status::invalid_argument);
    ok(provider->scan(path, list));
    require(list.size() == 1);
    const auto id = list[0].id;
    // Crashes and stalls during foreign module initialization and instance creation
    // must leave the catalogue and any existing host instance intact.
    {
        Mode mode("scan-exit");
        require(provider->scan(path, list) == Status::disconnected);
        require(list.size() == 1);
    }
    {
        Mode mode("create-exit");
        PluginInstance p;
        require(provider->create(path, id, {}, p) == Status::disconnected);
        require(p.lifecycle() == PluginLifecycle::empty);
    }
    {
        auto shortOptions = isolation;
        shortOptions.controlTimeoutMs = 100;
        std::unique_ptr<PluginProvider> shortProvider;
        ok(createPluginProvider(shortProvider, shortOptions));
        Mode mode("scan-stall");
        require(shortProvider->scan(path, list) == Status::timed_out);
    }
    ok(provider->scan(path, list));
    exchange<float>(*provider, path, id);
    exchange<double>(*provider, path, id);
    // An actual abrupt plugin exit is confined to its instance's child process.
    {
        Instance crashed;
        ok(provider->create(path, id, {}, crashed.p));
        PluginPrepareOptions o;
        o.maximumFrames = 128;
        o.maximumEvents = 32;
        ok(crashed.p.activate(o));
        ok(crashed.p.start());
        Blocks<float> b;
        PluginEvent e;
        e.kind = PluginEventKind::noteOn;
        e.port = 0;
        e.key = 127;
        e.channel = 15;
        e.value = 1;
        ok(b.events.add(e));
        auto block = b.call(64);
        std::uint64_t ticket = 0;
        ok(crashed.p.submit(block, ticket));
        Status health = Status::ok;
        for (unsigned i = 0; i < 200 && health == Status::ok; ++i) {
            std::uint32_t flags = 0;
            health = crashed.p.pumpMainThread(flags);
            if (health == Status::ok)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(health == Status::disconnected);
        require(crashed.p.collect(block, ticket) == Status::disconnected);
        for (auto s : b.out.block().channel(0).first(64))
            require(s == 0);
    }
    exchange<float>(*provider, path, id);
    controls(directory, isolation);
    for (const auto& [format, name] : std::array<std::pair<PluginFormat, const char*>, 3>{
             {{PluginFormat::vst3, "cydsp_vst_fixture.vst3"},
              {PluginFormat::lv2, "cydsp_fixture.lv2"},
              {PluginFormat::jsfx, "gain.jsfx"}}}) {
        std::unique_ptr<PluginProvider> other;
        ok(createPluginProvider(format, other, isolation));
        const auto module = std::filesystem::absolute(directory / name);
        std::vector<PluginDescriptor> descriptors;
        ok(other->scan(module, descriptors));
        require(!descriptors.empty());
        Instance instance;
        ok(other->create(module, descriptors[0].id, {}, instance.p));
        require(instance.p.isolated());
        PluginPrepareOptions o;
        o.maximumFrames = 128;
        o.maximumEvents = 32;
        ok(instance.p.activate(o));
        ok(instance.p.deactivate());
    }
    {
        auto pipelined = isolation;
        pipelined.scheduling = PluginScheduling::pipelined;
        std::unique_ptr<PluginProvider> async;
        ok(createPluginProvider(async, pipelined));
        Instance value;
        ok(async->create(path, id, {}, value.p));
        PluginPrepareOptions spec;
        spec.maximumFrames = 64;
        spec.maximumEvents = 32;
        ok(value.p.activate(spec));
        require(value.p.timing().latencyKnown && value.p.timing().latencyFrames == 81);
        ok(value.p.start());
        Blocks<float> b;
        auto block = b.call(64);
        ok(value.p.process(block));
        for (auto x : b.out.block().channel(0).first(64))
            require(x == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ok(value.p.process(block));
        require(b.out.block().channel(0)[0] == 1.25f);
    }
    std::cout
        << "PASS mandatory plugin isolation/discovery/CLAP "
           "controls/float32+64/split-phase/variable frames/deadlines/crash recovery/realtime\n";
    // Malformed serialized data is rejected before it can allocate an unbounded list.
    {
        detail::rpc::Writer writer;
        writer(std::uint32_t(0xffffffff));
        detail::rpc::Reader reader(writer.bytes);
        std::vector<ClapMenuItem> menu;
        bool rejected = false;
        try {
            reader(menu);
        } catch (...) {
            rejected = true;
        }
        require(rejected);
    }
}
