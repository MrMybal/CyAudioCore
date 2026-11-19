#include "ClapModule.h"
#include "PluginRpc.h"
#include "cyaudio/plugins/ClapExtensions.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace cyaudio {
namespace {
// All foreign strings and aggregate metadata are bounded, including declarations
// produced during provider init. No exception crosses a CLAP callback.
struct Budget {
    std::size_t remaining = 64 * 1024 * 1024;
    Status status = Status::ok;
    std::string copy(const char* value) {
        if (!value)
            return {};
        std::size_t n = 0;
        for (; n < 65536 && value[n]; ++n) {
        }
        if (n == 65536 || n > remaining) {
            status = Status::block_too_large;
            throw std::length_error("CLAP preset metadata limit");
        }
        remaining -= n;
        return {value, n};
    }
    template <class F> bool receive(F&& f) noexcept {
        if (status != Status::ok)
            return false;
        try {
            f();
            return true;
        } catch (const std::bad_alloc&) {
            status = Status::allocation_failed;
        } catch (...) {
            if (status == Status::ok)
                status = Status::unsupported_format;
        }
        return false;
    }
};
bool validInput(std::string_view s) noexcept {
    return s.size() < 65536 && s.find(char{}) == std::string_view::npos;
}
bool validPresetLocation(ClapPresetLocationKind kind, std::string_view value) {
    if (!validInput(value))
        return false;
    if (kind == ClapPresetLocationKind::plugin)
        return value.empty();
    return kind == ClapPresetLocationKind::file && !value.empty() &&
           std::filesystem::path(
               std::u8string_view(reinterpret_cast<const char8_t*>(value.data()), value.size()))
               .is_absolute();
}
const clap_preset_discovery_factory_t* presetFactory(const detail::ClapModule& module) noexcept {
    const auto* factory = static_cast<const clap_preset_discovery_factory_t*>(
        module.entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID));
    if (!factory)
        factory = static_cast<const clap_preset_discovery_factory_t*>(
            module.entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT));
    return factory && factory->count && factory->get_descriptor && factory->create ? factory
                                                                                   : nullptr;
}
struct MetadataReceiver : Budget {
    std::vector<ClapPresetMetadata> presets;
    ClapPresetLocationKind kind;
    explicit MetadataReceiver(ClapPresetLocationKind k) : kind(k) {}
    ClapPresetMetadata& current() {
        if (presets.empty())
            throw std::invalid_argument("CLAP metadata before begin_preset");
        return presets.back();
    }
    template <class T> void append(std::vector<T>& values, T value) {
        if (values.size() == 4096) {
            status = Status::block_too_large;
            throw std::length_error("CLAP preset list limit");
        }
        values.push_back(std::move(value));
    }
    clap_preset_discovery_metadata_receiver_t interface() noexcept {
        return {this,
                [](const auto* r, std::int32_t, const char*) noexcept {
                    static_cast<MetadataReceiver*>(r->receiver_data)->status = Status::io_error;
                },
                [](const auto* r, const char* name, const char* key) noexcept {
                    auto& s = *static_cast<MetadataReceiver*>(r->receiver_data);
                    return s.receive([&] {
                        if (s.presets.size() == 65536) {
                            s.status = Status::block_too_large;
                            throw std::length_error("presets");
                        }
                        ClapPresetMetadata p;
                        p.name = s.copy(name);
                        p.loadKey = s.copy(key);
                        if ((s.kind == ClapPresetLocationKind::plugin || !s.presets.empty()) &&
                            (p.name.empty() || p.loadKey.empty() ||
                             (!s.presets.empty() && (s.presets.front().name.empty() ||
                                                     s.presets.front().loadKey.empty()))))
                            throw std::invalid_argument("CLAP container preset key");
                        s.presets.push_back(std::move(p));
                    });
                },
                [](const auto* r, const clap_universal_plugin_id_t* id) noexcept {
                    auto& s = *static_cast<MetadataReceiver*>(r->receiver_data);
                    s.receive([&] {
                        if (!id || !id->abi || !id->id)
                            throw std::invalid_argument("CLAP preset plugin id");
                        s.append(s.current().pluginIds, std::pair{s.copy(id->abi), s.copy(id->id)});
                    });
                },
                [](const auto* r, const char* id) noexcept {
                    auto& s = *static_cast<MetadataReceiver*>(r->receiver_data);
                    s.receive([&] { s.current().soundpackId = s.copy(id); });
                },
                [](const auto* r, std::uint32_t flags) noexcept {
                    auto& s = *static_cast<MetadataReceiver*>(r->receiver_data);
                    s.receive([&] {
                        s.current().flags = flags;
                        s.current().flagsKnown = true;
                    });
                },
                [](const auto* r, const char* creator) noexcept {
                    auto& s = *static_cast<MetadataReceiver*>(r->receiver_data);
                    s.receive([&] { s.append(s.current().creators, s.copy(creator)); });
                },
                [](const auto* r, const char* description) noexcept {
                    auto& s = *static_cast<MetadataReceiver*>(r->receiver_data);
                    s.receive([&] { s.current().description = s.copy(description); });
                },
                [](const auto* r, clap_timestamp creation, clap_timestamp modification) noexcept {
                    auto& s = *static_cast<MetadataReceiver*>(r->receiver_data);
                    s.receive([&] {
                        s.current().creationTime = creation;
                        s.current().modificationTime = modification;
                    });
                },
                [](const auto* r, const char* feature) noexcept {
                    auto& s = *static_cast<MetadataReceiver*>(r->receiver_data);
                    s.receive([&] { s.append(s.current().features, s.copy(feature)); });
                },
                [](const auto* r, const char* key, const char* value) noexcept {
                    auto& s = *static_cast<MetadataReceiver*>(r->receiver_data);
                    s.receive([&] {
                        s.append(s.current().extra, std::pair{s.copy(key), s.copy(value)});
                    });
                }};
    }
};
} // namespace

struct ClapPresetDiscovery::State : Budget {
    std::unique_ptr<PluginWorker> worker;
    std::shared_ptr<detail::ClapModule> module;
    const clap_preset_discovery_provider_t* provider = nullptr;
    ClapPresetCatalogue catalogue;
    clap_preset_discovery_indexer_t indexer{};
    State() {
        indexer = {CLAP_VERSION,
                   "CyAudioCore",
                   "MrMybal CyberAlien",
                   "https://github.com/MrMybal/CyAudioCore",
                   "0.2.0",
                   this,
                   [](const auto* i, const clap_preset_discovery_filetype_t* f) noexcept {
                       auto& s = *static_cast<State*>(i->indexer_data);
                       return s.receive([&] {
                           if (!f || !f->name || s.catalogue.fileTypes.size() == 4096)
                               throw std::invalid_argument("CLAP preset filetype");
                           ClapPresetFileType t{s.copy(f->name), s.copy(f->description),
                                                s.copy(f->file_extension)};
                           if (t.name.empty() ||
                               t.extension.find_first_of("./\\") != std::string::npos)
                               throw std::invalid_argument("CLAP preset extension");
                           s.catalogue.fileTypes.push_back(std::move(t));
                       });
                   },
                   [](const auto* i, const clap_preset_discovery_location_t* l) noexcept {
                       auto& s = *static_cast<State*>(i->indexer_data);
                       return s.receive([&] {
                           if (!l || !l->name || s.catalogue.locations.size() == 4096)
                               throw std::invalid_argument("CLAP preset location");
                           ClapPresetSearchLocation location{l->flags, s.copy(l->name),
                                                             ClapPresetLocationKind(l->kind),
                                                             s.copy(l->location)};
                           if (location.name.empty() ||
                               !validPresetLocation(location.kind, location.location) ||
                               (location.kind == ClapPresetLocationKind::plugin && l->location))
                               throw std::invalid_argument("CLAP preset location");
                           s.catalogue.locations.push_back(std::move(location));
                       });
                   },
                   [](const auto* i, const clap_preset_discovery_soundpack_t* p) noexcept {
                       auto& s = *static_cast<State*>(i->indexer_data);
                       return s.receive([&] {
                           if (!p || !p->id || !p->name || s.catalogue.soundpacks.size() == 4096)
                               throw std::invalid_argument("CLAP soundpack");
                           ClapSoundpack pack{p->flags,
                                              s.copy(p->id),
                                              s.copy(p->name),
                                              s.copy(p->description),
                                              s.copy(p->homepage_url),
                                              s.copy(p->vendor),
                                              s.copy(p->image_path),
                                              p->release_timestamp};
                           if (pack.id.empty() || pack.name.empty())
                               throw std::invalid_argument("CLAP soundpack identity");
                           for (const auto& existing : s.catalogue.soundpacks)
                               if (existing.id == pack.id)
                                   throw std::invalid_argument("CLAP duplicate soundpack");
                           s.catalogue.soundpacks.push_back(std::move(pack));
                       });
                   },
                   [](const clap_preset_discovery_indexer_t*, const char*) noexcept -> const void* {
                       return nullptr;
                   }};
    }
    ~State() {
        if (provider && provider->destroy)
            provider->destroy(provider);
    }
};
ClapPresetDiscovery::ClapPresetDiscovery() = default;
ClapPresetDiscovery::~ClapPresetDiscovery() = default;
ClapPresetDiscovery::ClapPresetDiscovery(ClapPresetDiscovery&&) noexcept = default;
ClapPresetDiscovery& ClapPresetDiscovery::operator=(ClapPresetDiscovery&&) noexcept = default;
Status ClapPresetDiscovery::providers(const std::filesystem::path& path,
                                      std::vector<ClapPresetProviderInfo>& out,
                                      const PluginIsolationOptions& options) noexcept {
    if (!detail::insidePluginWorker)
        return detail::PluginRemoteAccess::presetProviders(path, out, options);
    try {
        std::shared_ptr<detail::ClapModule> module;
        const auto s = detail::loadClapModule(path, module, false);
        if (s != Status::ok)
            return s;
        const auto* f = presetFactory(*module);
        if (!f)
            return Status::unsupported_format;
        const auto count = f->count(f);
        if (count > 4096)
            return Status::block_too_large;
        Budget budget;
        std::vector<ClapPresetProviderInfo> next;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto* d = f->get_descriptor(f, i);
            if (!d || d->clap_version.major != 1 || !d->id || !d->name)
                return Status::incompatible_version;
            ClapPresetProviderInfo p{budget.copy(d->id), budget.copy(d->name),
                                     budget.copy(d->vendor)};
            if (p.id.empty() || p.name.empty())
                return Status::unsupported_format;
            for (const auto& previous : next)
                if (previous.id == p.id)
                    return Status::unsupported_format;
            next.push_back(std::move(p));
        }
        out.swap(next);
        return Status::ok;
    } catch (const std::bad_alloc&) {
        return Status::allocation_failed;
    } catch (...) {
        return Status::unsupported_format;
    }
}
Status ClapPresetDiscovery::open(const std::filesystem::path& path, std::string_view id,
                                 const PluginIsolationOptions& options) noexcept {
    if (id.empty() || !validInput(id))
        return Status::invalid_argument;
    try {
        auto next = std::make_unique<State>();
        if (!detail::insidePluginWorker) {
            next->worker = std::make_unique<PluginWorker>();
            auto status = detail::PluginRemoteAccess::launch(*next->worker, options,
                                                             PluginFormat::clap, path, {}, 3);
            if (status != Status::ok)
                return status;
            detail::rpc::Writer request;
            request(detail::rpc::Op::presetOpen, std::string(id));
            std::vector<std::uint8_t> response;
            status = detail::PluginRemoteAccess::request(*next->worker, request.bytes, response);
            if (status != Status::ok)
                return status;
            detail::rpc::Reader reader(response);
            reader(next->catalogue);
            reader.done();
            state_.swap(next);
            return Status::ok;
        }
        const auto s = detail::loadClapModule(path, next->module, false);
        if (s != Status::ok)
            return s;
        const auto* f = presetFactory(*next->module);
        if (!f)
            return Status::unsupported_format;
        const std::string identifier(id);
        next->provider = f->create(f, &next->indexer, identifier.c_str());
        const auto* p = next->provider;
        if (!p || !p->destroy || !p->init || !p->get_metadata || !p->get_extension || !p->desc ||
            p->desc->clap_version.major != 1)
            return Status::unsupported_format;
        if (next->copy(p->desc->id) != identifier || next->copy(p->desc->name).empty())
            return Status::unsupported_format;
        if (!p->init(p))
            return next->status == Status::ok ? Status::io_error : next->status;
        if (next->status != Status::ok)
            return next->status;
        state_.swap(next);
        return Status::ok;
    } catch (const std::bad_alloc&) {
        return Status::allocation_failed;
    } catch (...) {
        return Status::unsupported_format;
    }
}
const ClapPresetCatalogue* ClapPresetDiscovery::catalogue() const noexcept {
    return state_ ? &state_->catalogue : nullptr;
}
Status ClapPresetDiscovery::metadata(ClapPresetLocationKind kind, std::string_view location,
                                     std::vector<ClapPresetMetadata>& out) noexcept {
    if (!state_)
        return Status::not_prepared;
    try {
        if (!validPresetLocation(kind, location))
            return Status::invalid_argument;
        if (state_->worker) {
            detail::rpc::Writer request;
            request(detail::rpc::Op::presetMetadata, kind, std::string(location));
            std::vector<std::uint8_t> response;
            const auto status =
                detail::PluginRemoteAccess::request(*state_->worker, request.bytes, response);
            if (status != Status::ok)
                return status;
            detail::rpc::Reader reader(response);
            std::vector<ClapPresetMetadata> next;
            reader(next);
            reader.done();
            out.swap(next);
            return Status::ok;
        }
        MetadataReceiver receiver(kind);
        const auto callbacks = receiver.interface();
        const std::string path(location);
        const auto done = state_->provider->get_metadata(
            state_->provider, std::uint32_t(kind),
            kind == ClapPresetLocationKind::plugin ? nullptr : path.c_str(), &callbacks);
        if (receiver.status != Status::ok)
            return receiver.status;
        if (!done)
            return Status::io_error;
        out.swap(receiver.presets);
        return Status::ok;
    } catch (const std::bad_alloc&) {
        return Status::allocation_failed;
    } catch (...) {
        return Status::invalid_argument;
    }
}
void ClapPresetDiscovery::close() noexcept {
    state_.reset();
}
} // namespace cyaudio
