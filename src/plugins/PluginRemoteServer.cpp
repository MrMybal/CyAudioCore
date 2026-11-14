#include "PluginRemoteServer.h"
#include <chrono>
#include <thread>
namespace cyaudio::detail {
using namespace rpc;
RemoteHost::RemoteHost(wire::Header& h, HostSnapshot s) : header(h), snapshot(std::move(s)) {
    services.context = this;
    services.name = snapshot.name;
    services.vendor = snapshot.vendor;
    services.url = snapshot.url;
    services.version = snapshot.version;
    services.track = snapshot.track;
    services.log = [](void* p, std::int32_t code, std::string_view text) noexcept {
        try {
            Notice n;
            n.kind = NoticeKind::log;
            n.code = code;
            n.text = std::string(text.substr(0, 511));
            static_cast<RemoteHost*>(p)->push(std::move(n));
        } catch (...) {
        }
    };
    services.parameterRescan = [](void* p, std::uint32_t flags) noexcept {
        Notice n;
        n.kind = NoticeKind::rescan;
        n.a = flags;
        static_cast<RemoteHost*>(p)->push(std::move(n));
    };
    services.parameterClear = [](void* p, std::uint32_t id, std::uint32_t flags) noexcept {
        Notice n;
        n.kind = NoticeKind::clear;
        n.a = id;
        n.b = flags;
        static_cast<RemoteHost*>(p)->push(std::move(n));
    };
    services.suggestRemotePage = [](void* p, std::uint32_t id) noexcept {
        Notice n;
        n.kind = NoticeKind::suggest;
        n.a = id;
        static_cast<RemoteHost*>(p)->push(std::move(n));
    };
    services.presetLoaded = [](void* p, const ClapPresetLocation& location) noexcept {
        try {
            Notice n;
            n.kind = NoticeKind::loaded;
            n.location = location;
            static_cast<RemoteHost*>(p)->push(std::move(n));
        } catch (...) {
        }
    };
    services.presetError = [](void* p, const ClapPresetLocation& location, std::int32_t code,
                              std::string_view text) noexcept {
        try {
            Notice n;
            n.kind = NoticeKind::error;
            n.location = location;
            n.code = code;
            n.text = std::string(text.substr(0, 511));
            static_cast<RemoteHost*>(p)->push(std::move(n));
        } catch (...) {
        }
    };
    if (snapshot.threadPool)
        services.executeTasks = [](void*, std::uint32_t count,
                                   void (*run)(void*, std::uint32_t) noexcept,
                                   void* context) noexcept {
            for (std::uint32_t i = 0; i < count; ++i)
                run(context, i);
            return true;
        };
    if (snapshot.populateMenu)
        services.populateMenu = [](void* p, ClapMenuTarget target,
                                   std::vector<ClapMenuItem>& out) noexcept {
            try {
                Writer w;
                w(std::uint32_t(0), target);
                std::vector<std::uint8_t> reply;
                if (!static_cast<RemoteHost*>(p)->reverse(w.bytes, reply))
                    return false;
                Reader r(reply);
                bool ok;
                std::vector<ClapMenuItem> next;
                r(ok, next);
                r.done();
                if (ok)
                    out.swap(next);
                return ok;
            } catch (...) {
                return false;
            }
        };
    if (snapshot.performMenu)
        services.performMenu = [](void* p, ClapMenuTarget target, std::uint32_t action) noexcept {
            try {
                Writer w;
                w(std::uint32_t(1), target, action);
                std::vector<std::uint8_t> reply;
                if (!static_cast<RemoteHost*>(p)->reverse(w.bytes, reply))
                    return false;
                Reader r(reply);
                bool ok;
                r(ok);
                r.done();
                return ok;
            } catch (...) {
                return false;
            }
        };
    if (snapshot.popupMenu)
        services.popupMenu = [](void* p, ClapMenuTarget target, std::int32_t screen, std::int32_t x,
                                std::int32_t y) noexcept {
            try {
                Writer w;
                w(std::uint32_t(2), target, screen, x, y);
                std::vector<std::uint8_t> reply;
                if (!static_cast<RemoteHost*>(p)->reverse(w.bytes, reply))
                    return false;
                Reader r(reply);
                bool ok;
                r(ok);
                r.done();
                return ok;
            } catch (...) {
                return false;
            }
        };
    callbacks.context = this;
    callbacks.clap = &services;
    callbacks.resizeEditor = [](void* p, PluginEditorSize size) noexcept {
        auto& h = static_cast<RemoteHost*>(p)->header;
        h.requestedWidth.store(size.width, std::memory_order_relaxed);
        h.requestedHeight.store(size.height, std::memory_order_release);
        h.requests.fetch_or(pluginEditorResize, std::memory_order_release);
        return false;
    };
    callbacks.edited = [](void* p, const PluginEvent& e) noexcept {
        Notice n;
        n.kind = NoticeKind::edited;
        n.a = e.id;
        n.b = e.flags;
        n.value = e.value;
        n.eventKind = e.kind;
        static_cast<RemoteHost*>(p)->push(std::move(n));
    };
}
void RemoteHost::push(Notice n) noexcept {
    if (notices.size() >= 256) {
        header.requests.fetch_or(pluginLogsDropped, std::memory_order_release);
        return;
    }
    try {
        notices.push_back(std::move(n));
        header.requests.fetch_or(pluginMainThread, std::memory_order_release);
    } catch (...) {
        header.requests.fetch_or(pluginLogsDropped, std::memory_order_release);
    }
}
bool RemoteHost::reverse(std::span<const std::uint8_t> request,
                         std::vector<std::uint8_t>& out) noexcept {
    auto& h = header;
    if (request.size() > h.reversePayload.size())
        return false;
    auto state = h.reverseState.load(std::memory_order_acquire);
    // Consume a response to an earlier timed-out request before reusing its storage.
    if (state == 2) {
        h.reverseState.store(0, std::memory_order_release);
        state = 0;
    }
    if (state)
        return false;
    std::copy(request.begin(), request.end(), h.reversePayload.begin());
    h.reverseBytes = std::uint32_t(request.size());
    const auto timeout = std::min<unsigned>(h.reverseTimeoutMs, 60000);
    h.reverseState.store(1, std::memory_order_release);
    h.requests.fetch_or(pluginMainThread, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    while (h.reverseState.load(std::memory_order_acquire) != 2) {
        if (h.shutdown.load(std::memory_order_acquire) ||
            std::chrono::steady_clock::now() >= deadline) {
            std::uint32_t pending = 1;
            (void)h.reverseState.compare_exchange_strong(pending, 0, std::memory_order_acq_rel);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    bool ok = h.reverseStatus == std::uint32_t(Status::ok);
    const auto bytes = h.reverseBytes;
    if (bytes > h.reversePayload.size())
        ok = false;
    if (ok)
        try {
            out.assign(h.reversePayload.begin(), h.reversePayload.begin() + bytes);
        } catch (...) {
            ok = false;
        }
    h.reverseState.store(0, std::memory_order_release);
    return ok;
}
Status dispatchRemote(Reader& r, Writer& w, Op op, PluginInstance& plugin,
                      ClapPresetDiscovery& discovery, PluginProvider& provider,
                      const std::filesystem::path& path, RemoteHost& host) noexcept {
    try {
        auto* ext = plugin.clapExtensions();
        switch (op) {
        case Op::scan: {
            r.done();
            std::vector<PluginDescriptor> list;
            const auto s = provider.scan(path, list);
            if (s == Status::ok)
                w(list);
            return s;
        }
        case Op::describe: {
            r.done();
            const auto* d = plugin.description();
            if (!d)
                return Status::not_prepared;
            w(*d);
            return Status::ok;
        }
        case Op::refresh: {
            r.done();
            return plugin.refresh();
        }
        case Op::parameterValue: {
            std::uint32_t id;
            r(id);
            r.done();
            double value = 0;
            const auto s = plugin.parameterValue(id, value);
            if (s == Status::ok)
                w(value);
            return s;
        }
        case Op::parameterText: {
            std::uint32_t id;
            double value;
            r(id, value);
            r.done();
            std::string text;
            const auto s = plugin.parameterText(id, value, text);
            if (s == Status::ok)
                w(text);
            return s;
        }
        case Op::flush: {
            Events in;
            r(in);
            r.done();
            PluginEventBuffer input, output;
            auto s = decodeEvents(in, input);
            if (s == Status::ok)
                s = output.prepare(65536, 65536);
            if (s == Status::ok)
                s = plugin.flush(input, output);
            if (s == Status::ok)
                w(encodeEvents(output));
            return s;
        }
        case Op::save: {
            std::uint32_t limit;
            r(limit);
            r.done();
            std::vector<std::uint8_t> state;
            const auto s = PluginInstanceAccess::saveParked(plugin, state, limit);
            if (s == Status::ok)
                w(state);
            return s;
        }
        case Op::load: {
            std::vector<std::uint8_t> state;
            r(state);
            r.done();
            return plugin.loadState(state);
        }
        case Op::pump: {
            r.done();
            std::uint32_t flags = 0;
            auto s = plugin.pumpMainThread(flags);
            if (s != Status::ok)
                return s;
            w(flags, host.notices);
            host.notices.clear();
            return Status::ok;
        }
        case Op::presetProviders: {
            r.done();
            std::vector<ClapPresetProviderInfo> list;
            const auto s = ClapPresetDiscovery::providers(path, list);
            if (s == Status::ok)
                w(list);
            return s;
        }
        case Op::presetOpen: {
            std::string id;
            r(id);
            r.done();
            const auto s = discovery.open(path, id);
            if (s == Status::ok)
                w(*discovery.catalogue());
            return s;
        }
        case Op::presetMetadata: {
            ClapPresetLocationKind kind;
            std::string location;
            r(kind, location);
            r.done();
            std::vector<ClapPresetMetadata> list;
            const auto s = discovery.metadata(kind, location, list);
            if (s == Status::ok)
                w(list);
            return s;
        }
        default:
            break;
        }
        if (!ext)
            return Status::unsupported_format;
        switch (op) {
        case Op::capabilities: {
            r.done();
            w(ext->capabilities());
            return Status::ok;
        }
        case Op::configurations: {
            r.done();
            std::vector<ClapAudioConfiguration> v;
            const auto s = ext->audioConfigurations(v);
            if (s == Status::ok)
                w(v);
            return s;
        }
        case Op::selectConfiguration: {
            std::uint32_t id;
            r(id);
            r.done();
            return ext->selectAudioConfiguration(id);
        }
        case Op::canConfigure: {
            std::vector<ClapAudioPortRequest> ports;
            r(ports);
            r.done();
            bool yes = false;
            const auto s = ext->canConfigureAudioPorts(ports, yes);
            if (s == Status::ok)
                w(yes);
            return s;
        }
        case Op::configure: {
            std::vector<ClapAudioPortRequest> ports;
            r(ports);
            r.done();
            return ext->configureAudioPorts(ports);
        }
        case Op::portActive: {
            bool input, active;
            std::uint32_t index, bits;
            r(input, index, active, bits);
            r.done();
            return ext->setAudioPortActive(input, index, active, bits);
        }
        case Op::surround: {
            std::uint64_t mask;
            r(mask);
            r.done();
            bool yes = false;
            const auto s = ext->supportsSurround(mask, yes);
            if (s == Status::ok)
                w(yes);
            return s;
        }
        case Op::ambisonic: {
            std::uint32_t order, norm;
            r(order, norm);
            r.done();
            bool yes = false;
            const auto s = ext->supportsAmbisonic(order, norm, yes);
            if (s == Status::ok)
                w(yes);
            return s;
        }
        case Op::voices: {
            r.done();
            ClapVoiceInfo v;
            const auto s = ext->voiceInfo(v);
            if (s == Status::ok)
                w(v);
            return s;
        }
        case Op::noteNames: {
            r.done();
            std::vector<ClapNoteName> v;
            const auto s = ext->noteNames(v);
            if (s == Status::ok)
                w(v);
            return s;
        }
        case Op::remotes: {
            r.done();
            std::vector<ClapRemoteControlsPage> v;
            const auto s = ext->remoteControls(v);
            if (s == Status::ok)
                w(v);
            return s;
        }
        case Op::track: {
            ClapTrackInfo v;
            r(v);
            r.done();
            return ext->setTrackInfo(v);
        }
        case Op::fromText: {
            std::uint32_t id;
            std::string text;
            r(id, text);
            r.done();
            double v = 0;
            const auto s = ext->parameterFromText(id, text, v);
            if (s == Status::ok)
                w(v);
            return s;
        }
        case Op::mapping: {
            std::uint32_t id;
            bool mapped;
            std::optional<ClapColor> color;
            std::string label, description;
            r(id, mapped, color, label, description);
            r.done();
            return ext->indicateMapping(id, mapped, color ? &*color : nullptr, label, description);
        }
        case Op::automation: {
            std::uint32_t id;
            ClapAutomationIndication indication;
            std::optional<ClapColor> color;
            r(id, indication, color);
            r.done();
            return ext->indicateAutomation(id, indication, color ? &*color : nullptr);
        }
        case Op::saveContext: {
            ClapStateContext context;
            std::uint32_t limit;
            r(context, limit);
            r.done();
            std::vector<std::uint8_t> v;
            const auto s = ext->saveState(context, v, limit);
            if (s == Status::ok)
                w(v);
            return s;
        }
        case Op::loadContext: {
            ClapStateContext context;
            std::vector<std::uint8_t> v;
            r(context, v);
            r.done();
            return ext->loadState(context, v);
        }
        case Op::preset: {
            ClapPresetLocation v;
            r(v);
            r.done();
            return ext->loadPreset(v);
        }
        case Op::menu: {
            ClapMenuTarget target;
            r(target);
            r.done();
            std::vector<ClapMenuItem> v;
            const auto s = ext->contextMenu(target, v);
            if (s == Status::ok)
                w(v);
            return s;
        }
        case Op::menuAction: {
            ClapMenuTarget target;
            std::uint32_t action;
            r(target, action);
            r.done();
            return ext->performContextMenu(target, action);
        }
        case Op::hints: {
            r.done();
            ClapGuiHints v;
            const auto s = ext->editorHints(v);
            if (s == Status::ok)
                w(v);
            return s;
        }
        case Op::preferredApi: {
            r.done();
            PluginWindowApi api;
            bool floating = false;
            const auto s = ext->preferredEditorApi(api, floating);
            if (s == Status::ok)
                w(api, floating);
            return s;
        }
        case Op::title: {
            std::string v;
            r(v);
            r.done();
            return ext->suggestEditorTitle(v);
        }
        default:
            return Status::unsupported_format;
        }
    } catch (const std::bad_alloc&) {
        return Status::allocation_failed;
    } catch (...) {
        return Status::invalid_argument;
    }
}
} // namespace cyaudio::detail
