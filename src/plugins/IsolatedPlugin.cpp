#include "PluginInternal.h"
#include "PluginRpc.h"
#include <atomic>
namespace cyaudio {
namespace {
using namespace detail;
using namespace detail::rpc;
template <class Write, class Read>
Status call(PluginWorker& worker, Op op, Write&& write, Read&& read) noexcept {
    try {
        Writer request;
        request(op);
        write(request);
        std::vector<std::uint8_t> response;
        const auto s = PluginRemoteAccess::request(worker, request.bytes, response);
        if (s != Status::ok)
            return s;
        Reader reader(response);
        read(reader);
        reader.done();
        return Status::ok;
    } catch (const std::bad_alloc&) {
        return Status::allocation_failed;
    } catch (...) {
        return Status::invalid_argument;
    }
}
template <class... A> Status command(PluginWorker& worker, Op op, const A&... a) noexcept {
    return call(worker, op, [&](Writer& w) { w(a...); }, [](Reader&) {});
}
template <class T, class... A>
Status query(PluginWorker& worker, Op op, T& out, const A&... a) noexcept {
    try {
        T next{};
        const auto s = call(worker, op, [&](Writer& w) { w(a...); }, [&](Reader& r) { r(next); });
        if (s == Status::ok)
            out = std::move(next);
        return s;
    } catch (const std::bad_alloc&) {
        return Status::allocation_failed;
    } catch (...) {
        return Status::invalid_argument;
    }
}
class IsolatedBackend final : public PluginBackend, public ClapExtensions {
    PluginWorker worker;
    PluginDescription description_;
    ClapCapabilities caps;
    PluginIsolationOptions isolation;
    PluginHostCallbacks callbacks;
    ClapHostServices services;
    std::atomic<std::uint32_t> failure{0};
    bool active = false, processing = false;
    bool clap = false;
    Status describe() noexcept {
        PluginDescription next;
        auto s = query(worker, Op::describe, next);
        if (s == Status::ok)
            s = PluginRemoteAccess::shape(worker, next);
        if (s == Status::ok)
            description_ = std::move(next);
        return s;
    }
    Status changed(Status s) noexcept {
        if (active)
            return s;
        const auto d = describe();
        return s == Status::ok ? d : s;
    }
    void deliver(const Notice& n) noexcept {
        auto* c = services.context;
        switch (n.kind) {
        case NoticeKind::log:
            if (services.log)
                services.log(c, n.code, n.text);
            break;
        case NoticeKind::rescan:
            if (services.parameterRescan)
                services.parameterRescan(c, n.a);
            break;
        case NoticeKind::clear:
            if (services.parameterClear)
                services.parameterClear(c, n.a, n.b);
            break;
        case NoticeKind::suggest:
            if (services.suggestRemotePage)
                services.suggestRemotePage(c, n.a);
            break;
        case NoticeKind::loaded:
            if (services.presetLoaded)
                services.presetLoaded(c, n.location);
            break;
        case NoticeKind::error:
            if (services.presetError)
                services.presetError(c, n.location, n.code, n.text);
            break;
        case NoticeKind::edited:
            if (callbacks.edited) {
                PluginEvent e;
                e.kind = n.eventKind;
                e.id = n.a;
                e.flags = n.b;
                e.value = n.value;
                callbacks.edited(callbacks.context, e);
            }
            break;
        }
    }

  public:
    Status open(const PluginIsolationOptions& o, PluginFormat format,
                const std::filesystem::path& path, std::string_view id,
                const PluginHostCallbacks& cb) noexcept {
        try {
            isolation = o;
            callbacks = cb;
            if (cb.clap)
                services = *cb.clap;
            callbacks.clap = nullptr;
            // An application function pointer cannot be invoked in another process.
            // Use the explicitly selected worker-local executor instead.
            if (services.executeTasks)
                return Status::unsupported_backend;
            HostSnapshot snapshot;
            snapshot.name = services.name;
            snapshot.vendor = services.vendor;
            snapshot.url = services.url;
            snapshot.version = services.version;
            snapshot.track = services.track;
            snapshot.threadPool = o.clapThreadPool;
            snapshot.populateMenu = bool(services.populateMenu);
            snapshot.performMenu = bool(services.performMenu);
            snapshot.popupMenu = bool(services.popupMenu);
            auto s = PluginRemoteAccess::launch(worker, o, format, path, id, 1, snapshot,
                                                &callbacks, &services);
            if (s != Status::ok)
                return s;
            s = PluginRemoteAccess::callbacks(worker, callbacks, services);
            if (s != Status::ok)
                return s;
            s = describe();
            if (s != Status::ok)
                return s;
            clap = format == PluginFormat::clap;
            if (clap)
                s = query(worker, Op::capabilities, caps);
            return s;
        } catch (const std::bad_alloc&) {
            return Status::allocation_failed;
        } catch (...) {
            return Status::invalid_argument;
        }
    }
    bool isolated() const noexcept override {
        return true;
    }
    PluginSleepInfo sleepInfo() const noexcept override {
        return worker.sleepInfo();
    }
    void wake() noexcept override {
        worker.wake();
    }
    Status status() const noexcept override {
        return Status(failure.load(std::memory_order_acquire));
    }
    ClapExtensions* clapExtensions() noexcept override {
        return clap ? this : nullptr;
    }
    const PluginDescription& description() const noexcept override {
        return description_;
    }
    Status activate(const PluginPrepareOptions& o) noexcept override {
        if (o.maximumFrames > isolation.maximumFrames || o.maximumEvents > isolation.maximumEvents)
            return Status::invalid_spec;
        auto s = command(worker, Op::activate, o);
        if (s == Status::ok)
            s = PluginRemoteAccess::preparation(worker, o);
        if (s == Status::ok)
            s = describe();
        if (s == Status::ok)
            active = true;
        else
            (void)command(worker, Op::deactivate);
        return s;
    }
    Status deactivate() noexcept override {
        const auto s = command(worker, Op::deactivate);
        if (s == Status::ok) {
            active = false;
            processing = false;
        }
        return s;
    }
    Status start() noexcept override {
        const auto s = PluginRemoteAccess::audioCommand(worker, 2);
        if (s == Status::ok)
            processing = true;
        return s;
    }
    void stop() noexcept override {
        (void)PluginRemoteAccess::audioCommand(worker, 1);
        processing = false;
    }
    Status reset() noexcept override {
        return PluginRemoteAccess::audioCommand(worker, 3);
    }
    Status process(const PluginProcess<float>& p) noexcept override {
        PluginWorkerBlockResult result;
        return worker.process(p, result);
    }
    Status process(const PluginProcess<double>& p) noexcept override {
        PluginWorkerBlockResult result;
        return worker.process(p, result);
    }
    Status submit(const PluginProcess<float>& p, std::uint64_t& t) noexcept override {
        return worker.submit(p, t);
    }
    Status submit(const PluginProcess<double>& p, std::uint64_t& t) noexcept override {
        return worker.submit(p, t);
    }
    Status collect(const PluginProcess<float>& p, std::uint64_t t) noexcept override {
        return worker.collect(p, t);
    }
    Status collect(const PluginProcess<double>& p, std::uint64_t t) noexcept override {
        return worker.collect(p, t);
    }
    Status discard(std::uint64_t t) noexcept override {
        return worker.discard(t);
    }
    Status refresh() noexcept override {
        return changed(command(worker, Op::refresh));
    }
    PluginTiming timing() const noexcept override {
        const auto* info = worker.info();
        if (!info)
            return {};
        auto t = info->pluginTiming;
        if (t.latencyKnown) {
            if (t.latencyFrames >
                std::numeric_limits<std::uint32_t>::max() - info->pipelineLatencyFrames)
                t.latencyKnown = false;
            else
                t.latencyFrames += info->pipelineLatencyFrames;
        }
        return t;
    }
    Status parameterValue(std::uint32_t id, double& out) noexcept override {
        return query(worker, Op::parameterValue, out, id);
    }
    Status parameterText(std::uint32_t id, double value, std::string& out) noexcept override {
        return query(worker, Op::parameterText, out, id, value);
    }
    Status flush(const PluginEventBuffer& in, PluginEventBuffer& out) noexcept override {
        if (active)
            return PluginRemoteAccess::flushAudio(worker, in, out);
        try {
            Events reply;
            const auto s = query(worker, Op::flush, reply, encodeEvents(in));
            if (s != Status::ok)
                return s;
            // Public output buffer is already prepared; never resize its capacity.
            return wire::readEvents(reply.records.data(), reply.payload.data(),
                                    std::uint32_t(reply.records.size()),
                                    std::uint32_t(reply.payload.size()), 65536, 65536, 65536, out);
        } catch (const std::bad_alloc&) {
            return Status::allocation_failed;
        } catch (...) {
            return Status::invalid_argument;
        }
    }
    Status saveState(std::vector<std::uint8_t>& out, std::size_t limit) noexcept override {
        if (limit > rpc::maximumBytes)
            return Status::invalid_argument;
        return query(worker, Op::save, out, std::uint32_t(limit));
    }
    Status loadState(std::span<const std::uint8_t> in) noexcept override {
        try {
            return changed(
                command(worker, Op::load, std::vector<std::uint8_t>(in.begin(), in.end())));
        } catch (...) {
            return Status::allocation_failed;
        }
    }
    Status openEditor(const PluginWindow& w, PluginEditorSize& s) noexcept override {
        return worker.openEditor(w, s);
    }
    Status resizeEditor(PluginEditorSize& s) noexcept override {
        return worker.resizeEditor(s);
    }
    Status scaleEditor(double s) noexcept override {
        return worker.scaleEditor(s);
    }
    Status showEditor(bool s) noexcept override {
        return worker.showEditor(s);
    }
    Status focusEditor(bool s) noexcept override {
        return worker.focusEditor(s);
    }
    Status keyEditor(const PluginEditorKey& k) noexcept override {
        return worker.keyEditor(k);
    }
    Status wheelEditor(float d) noexcept override {
        return worker.wheelEditor(d);
    }
    void closeEditor() noexcept override {
        if (worker.info())
            (void)worker.closeEditor();
    }
    std::uint32_t pumpMainThread() noexcept override {
        std::uint32_t requests = 0;
        auto s = worker.poll(requests);
        if (s == Status::ok) {
            std::uint32_t flags = 0;
            std::vector<Notice> notices;
            s = call(worker, Op::pump, [](Writer&) {}, [&](Reader& r) { r(flags, notices); });
            requests |= flags;
            if (s == Status::ok)
                for (const auto& n : notices)
                    deliver(n);
            if (requests & pluginEditorResize) {
                auto size = worker.requestedEditorSize();
                if (callbacks.resizeEditor && callbacks.resizeEditor(callbacks.context, size))
                    (void)worker.resizeEditor(size);
            }
            if (callbacks.requests && requests)
                callbacks.requests(callbacks.context, requests);
        }
        failure.store(std::uint32_t(s), std::memory_order_release);
        return requests;
    }
    ClapCapabilities capabilities() const noexcept override {
        return caps;
    }
    ClapProcessStatus lastProcessStatus() const noexcept override {
        return PluginRemoteAccess::processStatus(worker);
    }
    Status audioConfigurations(std::vector<ClapAudioConfiguration>& out) noexcept override {
        return query(worker, Op::configurations, out);
    }
    Status selectAudioConfiguration(std::uint32_t id) noexcept override {
        return changed(command(worker, Op::selectConfiguration, id));
    }
    Status canConfigureAudioPorts(std::span<const ClapAudioPortRequest> ports,
                                  bool& out) noexcept override {
        try {
            return query(worker, Op::canConfigure, out,
                         std::vector<ClapAudioPortRequest>(ports.begin(), ports.end()));
        } catch (...) {
            return Status::allocation_failed;
        }
    }
    Status configureAudioPorts(std::span<const ClapAudioPortRequest> ports) noexcept override {
        try {
            return changed(command(worker, Op::configure,
                                   std::vector<ClapAudioPortRequest>(ports.begin(), ports.end())));
        } catch (...) {
            return Status::allocation_failed;
        }
    }
    Status setAudioPortActive(bool input, std::uint32_t index, bool enabled,
                              std::uint32_t bits) noexcept override {
        if (active)
            return PluginRemoteAccess::audioPort(worker, input, index, enabled, bits);
        return command(worker, Op::portActive, input, index, enabled, bits);
    }
    Status supportsSurround(std::uint64_t mask, bool& out) noexcept override {
        return query(worker, Op::surround, out, mask);
    }
    Status supportsAmbisonic(std::uint32_t ordering, std::uint32_t normalization,
                             bool& out) noexcept override {
        return query(worker, Op::ambisonic, out, ordering, normalization);
    }
    Status voiceInfo(ClapVoiceInfo& out) noexcept override {
        return query(worker, Op::voices, out);
    }
    Status noteNames(std::vector<ClapNoteName>& out) noexcept override {
        return query(worker, Op::noteNames, out);
    }
    Status remoteControls(std::vector<ClapRemoteControlsPage>& out) noexcept override {
        return query(worker, Op::remotes, out);
    }
    Status setTrackInfo(const ClapTrackInfo& info) noexcept override {
        return command(worker, Op::track, info);
    }
    Status parameterFromText(std::uint32_t id, std::string_view text,
                             double& out) noexcept override {
        try {
            return query(worker, Op::fromText, out, id, std::string(text));
        } catch (...) {
            return Status::allocation_failed;
        }
    }
    Status indicateMapping(std::uint32_t id, bool mapped, const ClapColor* color,
                           std::string_view label, std::string_view description) noexcept override {
        try {
            return command(worker, Op::mapping, id, mapped,
                           color ? std::optional<ClapColor>(*color) : std::nullopt,
                           std::string(label), std::string(description));
        } catch (...) {
            return Status::allocation_failed;
        }
    }
    Status indicateAutomation(std::uint32_t id, ClapAutomationIndication value,
                              const ClapColor* color) noexcept override {
        return command(worker, Op::automation, id, value,
                       color ? std::optional<ClapColor>(*color) : std::nullopt);
    }
    Status saveState(ClapStateContext context, std::vector<std::uint8_t>& out,
                     std::size_t limit) noexcept override {
        if (limit > rpc::maximumBytes)
            return Status::invalid_argument;
        return query(worker, Op::saveContext, out, context, std::uint32_t(limit));
    }
    Status loadState(ClapStateContext context,
                     std::span<const std::uint8_t> state) noexcept override {
        try {
            return changed(command(worker, Op::loadContext, context,
                                   std::vector<std::uint8_t>(state.begin(), state.end())));
        } catch (...) {
            return Status::allocation_failed;
        }
    }
    Status loadPreset(const ClapPresetLocation& location) noexcept override {
        return changed(command(worker, Op::preset, location));
    }
    Status contextMenu(ClapMenuTarget target, std::vector<ClapMenuItem>& out) noexcept override {
        return query(worker, Op::menu, out, target);
    }
    Status performContextMenu(ClapMenuTarget target, std::uint32_t action) noexcept override {
        return command(worker, Op::menuAction, target, action);
    }
    Status editorHints(ClapGuiHints& out) noexcept override {
        return query(worker, Op::hints, out);
    }
    Status preferredEditorApi(PluginWindowApi& api, bool& floating) noexcept override {
        PluginWindowApi nextApi{};
        bool nextFloating = false;
        const auto s = call(
            worker, Op::preferredApi, [](Writer&) {}, [&](Reader& r) { r(nextApi, nextFloating); });
        if (s == Status::ok) {
            api = nextApi;
            floating = nextFloating;
        }
        return s;
    }
    Status suggestEditorTitle(std::string_view title) noexcept override {
        try {
            return command(worker, Op::title, std::string(title));
        } catch (...) {
            return Status::allocation_failed;
        }
    }
};
class IsolatedProvider final : public PluginProvider {
    PluginFormat format;
    PluginIsolationOptions isolation;

  public:
    IsolatedProvider(PluginFormat f, const PluginIsolationOptions& o) : format(f), isolation(o) {}
    Status scan(const std::filesystem::path& path,
                std::vector<PluginDescriptor>& out) noexcept override {
        PluginWorker worker;
        auto s = PluginRemoteAccess::launch(worker, isolation, format, path, {}, 2);
        return s == Status::ok ? query(worker, Op::scan, out) : s;
    }
    Status create(const std::filesystem::path& path, std::string_view id,
                  const PluginHostCallbacks& cb, PluginInstance& out) noexcept override {
        if (out.lifecycle() == PluginLifecycle::processing)
            return Status::transition_in_progress;
        try {
            auto backend = std::make_unique<IsolatedBackend>();
            const auto s = backend->open(isolation, format, path, id, cb);
            if (s != Status::ok)
                return s;
            return PluginInstance::adopt(std::move(backend), out);
        } catch (...) {
            return Status::allocation_failed;
        }
    }
};
} // namespace
Status createPluginProvider(PluginFormat format, std::unique_ptr<PluginProvider>& out,
                            const PluginIsolationOptions& options) noexcept {
    if (detail::insidePluginWorker)
        return detail::createInProcessPluginProvider(format, out);
    if (format != PluginFormat::clap && format != PluginFormat::vst3 &&
        format != PluginFormat::lv2 && format != PluginFormat::jsfx)
        return Status::unsupported_format;
    if (!options.workerExecutable.is_absolute() || !options.maximumFrames ||
        options.maximumFrames > 65536 || !options.maximumEvents || options.maximumEvents > 65536 ||
        !options.maximumChannelsPerDirection || options.maximumChannelsPerDirection > 64 ||
        options.maximumPayloadBytes > 16 * 1024 * 1024 || !options.maximumStateBytes ||
        options.maximumStateBytes > 64 * 1024 * 1024 || !options.startupTimeoutMs ||
        options.startupTimeoutMs > 60000 || !options.controlTimeoutMs ||
        options.controlTimeoutMs > 60000 || !options.heartbeatTimeoutMs ||
        options.heartbeatTimeoutMs > 60000 || std::uint32_t(options.scheduling) > 1)
        return Status::invalid_argument;
    try {
        auto next = std::make_unique<IsolatedProvider>(format, options);
        out = std::move(next);
        return Status::ok;
    } catch (...) {
        return Status::allocation_failed;
    }
}
Status detail::PluginRemoteAccess::presetProviders(const std::filesystem::path& path,
                                                   std::vector<ClapPresetProviderInfo>& out,
                                                   const PluginIsolationOptions& options) noexcept {
    PluginWorker worker;
    const auto s = launch(worker, options, PluginFormat::clap, path, {}, 3);
    return s == Status::ok ? query(worker, Op::presetProviders, out) : s;
}
} // namespace cyaudio
