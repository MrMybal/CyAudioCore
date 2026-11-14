#include "fixtures/ClapAdvancedContract.h"
#include "plugins/PluginInternal.h"
#include <algorithm>
#include <array>
#include <cyaudio/CyAudioCore.h>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <thread>

void assertNoRealtimeAllocations(void (*)(void*) noexcept, void*);
namespace {
using namespace cyaudio;
void require(bool value, std::source_location at = std::source_location::current()) {
    if (!value)
        throw std::runtime_error("CLAP extension contract line " + std::to_string(at.line()));
}
void ok(Status status, std::source_location at = std::source_location::current()) {
    require(status == Status::ok, at);
}
struct Host {
    unsigned logs = 0, rescan = 0, cleared = 0, suggested = 0, preset = 0, errors = 0, actions = 0;
    std::thread::id main = std::this_thread::get_id();
    bool wrongThread = false;
    bool parallel = false;
    ClapHostServices services() {
        ClapHostServices s;
        s.context = this;
        s.track = ClapTrackInfo{};
        s.track->name = "Fixture track";
        s.track->channels = 2;
        s.track->portType = "stereo";
        s.log = [](void* p, std::int32_t, std::string_view message) noexcept {
            auto& s = *static_cast<Host*>(p);
            ++s.logs;
            if (s.main != std::this_thread::get_id() || message.empty())
                s.wrongThread = true;
        };
        s.parameterRescan = [](void* p, std::uint32_t f) noexcept {
            static_cast<Host*>(p)->rescan |= f;
        };
        s.parameterClear = [](void* p, std::uint32_t id, std::uint32_t f) noexcept {
            if (id == 7)
                static_cast<Host*>(p)->cleared |= f;
        };
        s.suggestRemotePage = [](void* p, std::uint32_t id) noexcept {
            static_cast<Host*>(p)->suggested = id;
        };
        s.presetLoaded = [](void* p, const ClapPresetLocation& l) noexcept {
            if (l.kind == ClapPresetLocationKind::plugin && l.loadKey == "warm")
                ++static_cast<Host*>(p)->preset;
        };
        s.presetError = [](void* p, const ClapPresetLocation&, std::int32_t e,
                           std::string_view message) noexcept {
            if (e == 23 && !message.empty())
                ++static_cast<Host*>(p)->errors;
        };
        s.populateMenu = [](void*, ClapMenuTarget, std::vector<ClapMenuItem>& result) noexcept {
            try {
                result.push_back({ClapMenuItemKind::entry, "Host action", 88});
                return true;
            } catch (...) {
                return false;
            }
        };
        s.performMenu = [](void* p, ClapMenuTarget, std::uint32_t action) noexcept {
            if (action != 88)
                return false;
            ++static_cast<Host*>(p)->actions;
            return true;
        };
        s.executeTasks = [](void* context, std::uint32_t count,
                            void (*run)(void*, std::uint32_t) noexcept, void* task) noexcept {
            if (static_cast<Host*>(context)->parallel) {
                // A foreign test executor, outside the host allocation audit.
                // Application-owned allocation/join are outside the host RT promise.
                try {
                    std::thread worker([=] {
                        for (std::uint32_t i = 0; i < count; ++i)
                            run(task, i);
                    });
                    worker.join();
                    return true;
                } catch (...) {
                    return false;
                }
            }
            // The bridge also accepts a prepared application pool. This deterministic
            // executor checks completion, task thread identity and recursive rejection.
            for (std::uint32_t i = 0; i < count; ++i)
                run(task, i);
            return true;
        };
        return s;
    }
};
template <class T>
void audio(PluginInstance& instance, ClapFixtureControl command, ClapFixtureRead read, Host& host) {
    auto& ext = *instance.clapExtensions();
    PluginPrepareOptions options;
    options.maximumFrames = 64;
    options.useFloat64 = std::same_as<T, double>;
    ok(instance.activate(options));
    ClapVoiceInfo voices;
    ok(ext.voiceInfo(voices));
    require(voices.count == 8 && voices.capacity == 32 && voices.overlappingNotes);
    require(ext.selectAudioConfiguration(11) == Status::transition_in_progress);
    AudioBuffer<T> input, output;
    ok(input.resize(2, 80));
    ok(output.resize(2, 80));
    for (unsigned ch = 0; ch < 2; ++ch) {
        std::fill_n(input.block().channel(ch).data(), 80, T{2});
        std::fill_n(output.block().channel(ch).data(), 80, T{-9});
    }
    const std::array<AudioBlock<const T>, 1> ins{input.block().subBlock(5, 64)};
    const std::array<AudioBlock<T>, 1> outs{output.block().subBlock(5, 64)};
    PluginEventBuffer events, result;
    ok(events.prepare(32, 1024));
    ok(result.prepare(32, 1024));
    for (auto kind :
         {PluginEventKind::parameterValue, PluginEventKind::parameterModulation,
          PluginEventKind::noteOn, PluginEventKind::noteExpression, PluginEventKind::noteChoke}) {
        PluginEvent e;
        e.kind = kind;
        e.noteId = 42;
        e.port = 0;
        e.channel = 3;
        e.key = 60;
        e.value = .25;
        e.id = kind == PluginEventKind::noteExpression ? 1 : 7;
        ok(events.add(e));
    }
    PluginEvent e;
    e.kind = PluginEventKind::transport;
    e.time.hasTempo = true;
    e.time.tempo = 137;
    e.time.tempoIncrement = .01;
    e.time.hasBeats = true;
    e.time.beats = 7.25;
    ok(events.add(e));
    e = {};
    e.kind = PluginEventKind::ump;
    e.port = 0;
    e.packet.words[0] = 0x20903c40u;
    e.packet.count = 1;
    ok(events.add(e));
    PluginTransport transport;
    transport.hasSignature = true;
    transport.numerator = 7;
    transport.denominator = 8;
    PluginProcess<T> call{ins, outs, 1, &events, &result, &transport, -1};
    ok(instance.start());
    const auto beforeTasks = read().poolTasks;
    for (const auto frames : {1u, 3u, 64u, 17u}) {
        call.frames = frames;
        struct Run {
            PluginInstance* instance;
            PluginProcess<T>* call;
            Status result;
        } run{&instance, &call, Status::not_prepared};
        assertNoRealtimeAllocations(
            [](void* p) noexcept {
                auto& r = *static_cast<Run*>(p);
                r.result = r.instance->process(*r.call);
            },
            &run);
        ok(run.result);
        require(result.size() == 7 && result.event(4)->kind == PluginEventKind::noteEnd &&
                result.event(4)->value == 0 && result.event(0)->noteId == 42 &&
                result.event(5)->time.tempoIncrement == .01 &&
                result.event(5)->time.beats == 7.25 &&
                result.event(6)->packet.words[0] == 0x20903c40u);
        require(output.block().channel(0)[5 + frames - 1] == T{3});
    }
    require(read().poolTasks == beforeTasks + 16 && read().noteEnds >= 4);
    host.parallel = true;
    ok(instance.process(call));
    host.parallel = false;
    require(read().poolTasks == beforeTasks + 20);
    require(host.logs == 0); // No application logger was called from audio.
    require(output.block().channel(0)[4] == T{-9} && output.block().channel(0)[69] == T{-9});
    for (auto [c, expected] :
         {std::pair{ClapFixtureCommand::processSleep, ClapProcessStatus::sleep},
          std::pair{ClapFixtureCommand::processTail, ClapProcessStatus::tail},
          std::pair{ClapFixtureCommand::processQuiet, ClapProcessStatus::continueIfNotQuiet}}) {
        command(c);
        ok(instance.process(call));
        require(ext.lastProcessStatus() == expected);
    }
    command(ClapFixtureCommand::processError);
    require(instance.process(call) == Status::io_error);
    require(ext.lastProcessStatus() == ClapProcessStatus::error &&
            output.block().channel(0)[5] == T{});
    command(ClapFixtureCommand::invalidOutput);
    ok(instance.process(call));
    std::uint32_t flags = 0;
    ok(instance.pumpMainThread(flags));
    require((flags & pluginEventsDropped) != 0 && host.logs > 0 && !host.wrongThread);
    ok(instance.pumpMainThread(flags));
    require(!(flags & pluginEventsDropped));
    command(ClapFixtureCommand::normal);
    ok(ext.setAudioPortActive(true, 0, false, options.useFloat64 ? 64 : 32));
    ok(instance.process(call));
    require(output.block().channel(0)[5] == T{1} && input.block().channel(0)[5] == T{2});
    ok(instance.stop());
    ok(instance.deactivate());
    ok(instance.refresh());
    ok(instance.activate(options));
    ok(instance.start());
    ok(instance.process(call));
    require(output.block().channel(0)[5] == T{1}); // Refresh preserves port activation.
    ok(ext.setAudioPortActive(false, 0, false, 0));
    for (unsigned ch = 0; ch < 2; ++ch)
        std::fill_n(output.block().channel(ch).data() + 5, 64, T{99});
    ok(instance.process(call));
    require(output.block().channel(0)[5] == T{} && output.block().channel(1)[21] == T{});
    ok(ext.setAudioPortActive(true, 0, true, 0));
    ok(ext.setAudioPortActive(false, 0, true, 0));
    require(ext.setAudioPortActive(true, 8, false, 0) == Status::invalid_argument);
    require(ext.setAudioPortActive(true, 0, false, 24) == Status::invalid_argument);
    require(instance.flushAudio(events, result) == Status::invalid_argument);
    PluginEventBuffer parameters;
    ok(parameters.prepare(8, 0));
    ok(parameters.add(*events.event(0)));
    ok(parameters.add(*events.event(1)));
    ok(instance.flushAudio(parameters, result));
    require(result.size() == 2 && read().flushes > 0);
    require(instance.flushAudio(events, events) == Status::invalid_argument);
    double liveValue = 0;
    ok(instance.parameterValue(7, liveValue));
    require(liveValue == .25);
    ok(instance.stop());
    ok(instance.deactivate());
    host.logs = 0;
}
} // namespace

void clapExtensionsRegression(const std::filesystem::path& directory) {
    using namespace cyaudio;
    const auto path = std::filesystem::absolute(directory / "cyaudio_clap_advanced.clap");
    detail::SharedModule library;
    ok(library.open(path));
    const auto read = reinterpret_cast<ClapFixtureRead>(library.symbol("fixtureRead"));
    const auto command = reinterpret_cast<ClapFixtureControl>(library.symbol("fixtureControl"));
    require(read && command);
    command(ClapFixtureCommand::normal);
    std::unique_ptr<PluginProvider> provider;
    ok(createPluginProvider(provider));
    std::vector<PluginDescriptor> plugins;
    require(provider->scan("relative.clap", plugins) == Status::invalid_argument);
    ok(provider->scan(path, plugins));
    require(plugins.size() == 1);
    Host host;
    auto services = host.services();
    PluginHostCallbacks callbacks;
    callbacks.clap = &services;
    PluginInstance instance;
    ok(provider->create(path, plugins[0].id, callbacks, instance));
    auto* ext = instance.clapExtensions();
    require(ext);
    auto caps = ext->capabilities();
    require(caps.audioConfigurations && caps.configurationDetails && caps.configurablePorts &&
            caps.portActivation && caps.livePortActivation && caps.surround && caps.ambisonic &&
            caps.voiceInfo && caps.noteNames && caps.remoteControls && caps.parameterIndication &&
            caps.stateContext && caps.presetLoad && caps.contextMenu && !caps.hardRealtime);
    require(read().trackQueries == 1 && instance.description()->parameters[0].periodic);
    Status affinity = Status::ok;
    std::thread wrong([&] {
        ClapTrackInfo t;
        affinity = ext->setTrackInfo(t);
    });
    wrong.join();
    require(affinity == Status::invalid_argument);
    std::vector<ClapAudioConfiguration> configs;
    ok(ext->audioConfigurations(configs));
    require(configs.size() == 4 && configs[0].current && configs[0].detailedPortsKnown &&
            configs[1].inputs[0].channels == 1);
    bool spatialSupport = false;
    ok(ext->supportsSurround(3, spatialSupport));
    require(spatialSupport);
    ok(ext->supportsAmbisonic(1, 1, spatialSupport));
    require(spatialSupport);
    require(ext->supportsSurround(std::uint64_t{1} << 40, spatialSupport) ==
            Status::invalid_argument);
    command(ClapFixtureCommand::oversizeConfig);
    require(ext->audioConfigurations(configs) == Status::block_too_large);
    require(configs.size() == 4);
    command(ClapFixtureCommand::normal);
    ok(ext->selectAudioConfiguration(12));
    require(instance.description()->inputs[0].surroundMap ==
            std::vector<std::uint8_t>({0, 1, 2, 3, 9, 10}));
    command(ClapFixtureCommand::invalidSurround);
    require(instance.refresh() == Status::unsupported_format);
    require(instance.activate() == Status::not_prepared);
    command(ClapFixtureCommand::normal);
    ok(ext->selectAudioConfiguration(13));
    require(instance.description()->inputs[0].ambisonicKnown &&
            instance.description()->inputs[0].ambisonicOrdering == 1 &&
            instance.description()->inputs[0].ambisonicNormalization == 1);
    ok(ext->selectAudioConfiguration(10));
    std::array<ClapAudioPortRequest, 2> requested;
    requested[0].input = true;
    requested[1].input = false;
    for (auto& r : requested) {
        r.channels = 1;
        r.type = "mono";
    }
    bool supported = false;
    ok(ext->canConfigureAudioPorts(requested, supported));
    require(supported);
    ok(ext->configureAudioPorts(requested));
    require(instance.description()->outputs[0].channels == 1);
    requested[1].input = true;
    require(ext->configureAudioPorts(requested) == Status::invalid_argument);
    ok(ext->selectAudioConfiguration(10));
    std::vector<ClapNoteName> names;
    ok(ext->noteNames(names));
    require(names.size() == 1 && names[0].key == 36 && names[0].name == "Kick");
    std::vector<ClapRemoteControlsPage> pages;
    ok(ext->remoteControls(pages));
    require(pages.size() == 1 && pages[0].id == 99 && pages[0].parameters[0] == 7 &&
            pages[0].parameters[1] == 0xffffffffu);
    ClapTrackInfo track;
    track.name = "Updated";
    track.color = ClapColor{255, 1, 2, 3};
    track.bus = true;
    ok(ext->setTrackInfo(track));
    require(read().trackQueries == 2);
    double value = 0;
    ok(ext->parameterFromText(7, "0.75", value));
    require(value == .75);
    require(ext->parameterFromText(7, "nan", value) == Status::invalid_argument);
    require(ext->parameterFromText(7, "2", value) == Status::invalid_argument);
    ok(ext->indicateMapping(7, true, nullptr, "Knob 1", "Fixture control"));
    ok(ext->indicateAutomation(7, ClapAutomationIndication::recording, nullptr));
    require(read().mapping == 1 && read().automation == 3);
    std::vector<std::uint8_t> state;
    ok(ext->saveState(ClapStateContext::preset, state, 1024));
    ok(ext->loadState(ClapStateContext::duplicate, state));
    require(read().stateSaveContext == 1 && read().stateLoadContext == 2);
    require(ext->saveState(ClapStateContext::project, state, 1) == Status::io_error);
    require(state.size() == sizeof(double));
    require(ext->loadState(ClapStateContext::project, {}) == Status::io_error);
    ok(ext->loadPreset({ClapPresetLocationKind::plugin, {}, "warm"}));
    require(host.preset == 1);
    require(ext->loadPreset({ClapPresetLocationKind::plugin, {}, "bad"}) == Status::io_error);
    require(host.errors == 1);
    require(ext->loadPreset({ClapPresetLocationKind::file, "relative", "warm"}) ==
            Status::invalid_argument);
    std::vector<ClapMenuItem> menu;
    ok(ext->contextMenu({}, menu));
    require(menu.size() == 1 && menu[0].checked && menu[0].action == 55);
    ok(ext->performContextMenu({}, 55));
    require(host.actions == 1);
    PluginWindowApi guiApi{};
    bool floating = false;
    PluginEditorSize guiSize;
    ok(ext->preferredEditorApi(guiApi, floating));
    require(guiApi == PluginWindowApi::unspecified && floating);
    ok(instance.openEditor({guiApi, 0, floating, 1}, guiSize));
    require(guiSize.width == 420 && guiSize.height == 210);
    ok(ext->suggestEditorTitle("Fixture title"));
    require(read().guiTitles == 1);
    command(ClapFixtureCommand::guiDestroyed);
    std::uint32_t closedFlags = 0;
    ok(instance.pumpMainThread(closedFlags));
    std::uint32_t guiRequests = closedFlags;
    ok(instance.pumpMainThread(closedFlags));
    guiRequests |= closedFlags;
    require((closedFlags & pluginEditorClosed) && read().guiAcknowledged == 1 &&
            read().badGuiHide == 0);
    ok(instance.closeEditor());
    command(ClapFixtureCommand::normal);
    std::uint32_t flags = 0;
    ok(instance.pumpMainThread(flags));
    require(host.cleared == 6 && host.suggested == 99 && ((flags | guiRequests) & pluginPresetLoaded) &&
            ((flags | guiRequests) & pluginPresetError));
    ok(instance.pumpMainThread(flags));
    require((flags & pluginParameterFlush) && (flags & pluginAudioConfigurationsChanged) &&
            (flags & pluginVoiceInfoChanged) && (flags & pluginNoteNamesChanged) &&
            (flags & pluginRemoteControlsChanged));
    audio<float>(instance, command, read, host);
    audio<double>(instance, command, read, host);
    ok(instance.pumpMainThread(flags));
    host.logs = 0;
    command(ClapFixtureCommand::concurrentLogs);
    ok(instance.pumpMainThread(flags));
    ok(instance.pumpMainThread(flags));
    require(host.logs == 40 && !host.wrongThread && !(flags & pluginLogsDropped));
    host.logs = 0;
    command(ClapFixtureCommand::floodLogs);
    ok(instance.pumpMainThread(flags));
    ok(instance.pumpMainThread(flags));
    require((flags & pluginLogsDropped) && host.logs == 64 && !host.wrongThread);
    command(ClapFixtureCommand::invalidateCookie);
    ok(instance.pumpMainThread(flags));
    host.logs = 0;
    require(host.rescan & 8); // Full invalidation must stop passing cached foreign cookies.
    // Keep the old metadata intentionally; null cookies remain a valid CLAP fallback.
    audio<float>(instance, command, read, host);
    command(ClapFixtureCommand::normal);
    ok(instance.refresh());
    command(ClapFixtureCommand::failActivation);
    require(instance.activate() == Status::invalid_spec);
    require(instance.lifecycle() == PluginLifecycle::initialized);
    command(ClapFixtureCommand::normal);
    ok(instance.activate());
    ok(instance.deactivate());

    const auto before = read();
    ClapPresetDiscovery discovery;
    std::vector<ClapPresetProviderInfo> providers;
    ok(ClapPresetDiscovery::providers(path, providers));
    require(providers.size() == 1);
    ok(discovery.open(path, providers[0].id));
    require(discovery.catalogue()->locations.size() == 1 &&
            discovery.catalogue()->fileTypes[0].extension == "fixture" &&
            discovery.catalogue()->soundpacks[0].releaseTime == 0);
    std::vector<ClapPresetMetadata> presets;
    ok(discovery.metadata(ClapPresetLocationKind::plugin, {}, presets));
    require(presets.size() == 1 && presets[0].name == "Warm" && presets[0].loadKey == "warm" &&
            presets[0].pluginIds[0].first == "clap" && presets[0].creationTime == 0 &&
            presets[0].modificationTime == 123 && presets[0].creators.size() == 1 &&
            presets[0].features[0] == "instrument" && presets[0].extra[0].second == "value");
    command(ClapFixtureCommand::invalidMetadata);
    require(discovery.metadata(ClapPresetLocationKind::plugin, {}, presets) ==
            Status::unsupported_format);
    require(presets.size() == 1); // Failed metadata reception is transactional.
    command(ClapFixtureCommand::failProviderInit);
    require(discovery.open(path, providers[0].id) == Status::io_error);
    require(discovery.catalogue() && read().providerDestroyed == before.providerDestroyed + 1);
    command(ClapFixtureCommand::normal);
    discovery.close();
    require(read().providerCreated == before.providerCreated + 2 &&
            read().providerDestroyed == before.providerDestroyed + 2);
    ok(instance.close());
    require(read().created == read().destroyed && read().entryInit == read().entryDeinit);
    // No executor/track supplied: optional services remain honestly unavailable.
    PluginInstance minimal;
    ok(provider->create(path, plugins[0].id, {}, minimal));
    ok(minimal.close());
    PluginWorker floatingWorker;
    auto workerExecutable = directory / "cydsp_plugin_worker_fixture";
#if defined(_WIN32)
    workerExecutable += ".exe";
#endif
    ok(floatingWorker.launch(std::filesystem::absolute(workerExecutable), PluginFormat::clap, path,
                             plugins[0].id));
    PluginEditorSize floatingSize;
    ok(floatingWorker.openEditor({PluginWindowApi::unspecified, 0, true, 1}, floatingSize));
    require(floatingSize.width == 420 && floatingSize.height == 210);
    ok(floatingWorker.closeEditor());
    floatingWorker.close();
    command(ClapFixtureCommand::hardRealtime);
    ok(provider->create(path, plugins[0].id, {}, minimal));
    PluginPrepareOptions offline;
    offline.offline = true;
    require(minimal.activate(offline) == Status::unsupported_format);
    ok(minimal.close());
    command(ClapFixtureCommand::normal);
    std::cout
        << "PASS CLAP stable extensions/ports/polyphony/presets/services/status/cleanup/realtime\n";
}
