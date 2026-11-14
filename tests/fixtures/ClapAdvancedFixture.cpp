// Original MIT fixture. Pinned CLAP declarations: third_party/licenses/clap.txt
#include "ClapAdvancedContract.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <clap/clap.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <thread>
#include <type_traits>

namespace {
ClapFixtureStats stats;
ClapFixtureCommand command = ClapFixtureCommand::normal; // Serialized test control.
const char* const features[]{CLAP_PLUGIN_FEATURE_INSTRUMENT, nullptr};
const clap_plugin_descriptor_t descriptor{CLAP_VERSION,
                                          "org.cyaudio.advanced",
                                          "Advanced CLAP fixture",
                                          "Example",
                                          "",
                                          "",
                                          "",
                                          "1",
                                          "Original acceptance fixture",
                                          features};
struct Plugin;
Plugin* livePlugin = nullptr;
struct Plugin {
    clap_plugin_t api{};
    const clap_host_t* host;
    bool active = false, processing = false, editor = false, guiLost = false;
    std::uint32_t configuration = 10, channels = 2;
    std::array<bool, 2> enabled{true, true};
    double value = .5;
    int cookie = 123;
    template <class T> const T* ext(const char* id) const noexcept {
        return static_cast<const T*>(host->get_extension(host, id));
    }
    bool thread(bool audio) const noexcept {
        const auto* t = ext<clap_host_thread_check_t>(CLAP_EXT_THREAD_CHECK);
        return t && (audio ? t->is_audio_thread(host) : t->is_main_thread(host));
    }
    static Plugin& self(const clap_plugin_t* p) noexcept {
        return *static_cast<Plugin*>(p->plugin_data);
    }
    void changed() noexcept {
        ext<clap_host_audio_ports_config_t>(CLAP_EXT_AUDIO_PORTS_CONFIG)->rescan(host);
        ext<clap_host_note_name_t>(CLAP_EXT_NOTE_NAME)->changed(host);
        ext<clap_host_voice_info_t>(CLAP_EXT_VOICE_INFO)->changed(host);
        const auto* remote = ext<clap_host_remote_controls_t>(CLAP_EXT_REMOTE_CONTROLS);
        remote->changed(host);
        remote->suggest_page(host, 99);
        const auto* params = ext<clap_host_params_t>(CLAP_EXT_PARAMS);
        params->rescan(host, command == ClapFixtureCommand::invalidateCookie
                                 ? CLAP_PARAM_RESCAN_ALL
                                 : CLAP_PARAM_RESCAN_VALUES);
        params->clear(host, 7, CLAP_PARAM_CLEAR_AUTOMATIONS | CLAP_PARAM_CLEAR_MODULATIONS);
        params->request_flush(host);
        ext<clap_host_surround_t>(CLAP_EXT_SURROUND)->changed(host);
        ext<clap_host_ambisonic_t>(CLAP_EXT_AMBISONIC)->changed(host);
    }
    static bool port(std::uint32_t count, clap_audio_port_info_t* info) noexcept {
        if (!info)
            return false;
        *info = {};
        info->id = 1;
        info->channel_count = count;
        info->in_place_pair = CLAP_INVALID_ID;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN | CLAP_AUDIO_PORT_SUPPORTS_64BITS |
                      CLAP_AUDIO_PORT_PREFERS_64BITS | CLAP_AUDIO_PORT_REQUIRES_COMMON_SAMPLE_SIZE;
        info->port_type = count == 1   ? CLAP_PORT_MONO
                          : count == 2 ? CLAP_PORT_STEREO
                          : count == 4 ? CLAP_PORT_AMBISONIC
                                       : CLAP_PORT_SURROUND;
        std::snprintf(info->name, sizeof(info->name), "Main");
        return true;
    }
    template <class T> clap_process_status process(const clap_process_t* call) noexcept {
        if (!active || !processing || !thread(true) || call->audio_inputs_count != 1 ||
            call->audio_outputs_count != 1 || call->audio_inputs[0].channel_count != channels)
            return CLAP_PROCESS_ERROR;
        ext<clap_host_log_t>(CLAP_EXT_LOG)->log(host, CLAP_LOG_INFO, "audio fixture log");
        if (const auto* pool = ext<clap_host_thread_pool_t>(CLAP_EXT_THREAD_POOL)) {
            if (!pool->request_exec(host, 4))
                return CLAP_PROCESS_ERROR;
        }
        for (std::uint32_t i = 0; i < call->in_events->size(call->in_events); ++i) {
            const auto* event = call->in_events->get(call->in_events, i);
            if (event->type == CLAP_EVENT_PARAM_VALUE) {
                const auto& e = *reinterpret_cast<const clap_event_param_value_t*>(event);
                if (e.cookie && e.cookie != &cookie)
                    return CLAP_PROCESS_ERROR;
                if (command == ClapFixtureCommand::invalidateCookie && e.cookie)
                    return CLAP_PROCESS_ERROR;
                value = e.value;
            }
            if (event->type == CLAP_EVENT_NOTE_CHOKE) {
                auto end = *reinterpret_cast<const clap_event_note_t*>(event);
                end.header.type = CLAP_EVENT_NOTE_END;
                end.velocity = std::numeric_limits<double>::quiet_NaN(); // Ignored native field.
                if (call->out_events->try_push(call->out_events, &end.header))
                    ++stats.noteEnds;
            } else
                (void)call->out_events->try_push(call->out_events, event);
        }
        if (command == ClapFixtureCommand::invalidOutput) {
            clap_event_header_t bad{sizeof(bad), 0, 400, 1, 0};
            if (call->out_events->try_push(call->out_events, &bad))
                return CLAP_PROCESS_ERROR;
        }
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            const auto* input = [&] {
                if constexpr (std::is_same_v<T, float>)
                    return call->audio_inputs[0].data32[ch];
                else
                    return call->audio_inputs[0].data64[ch];
            }();
            auto* output = [&] {
                if constexpr (std::is_same_v<T, float>)
                    return call->audio_outputs[0].data32[ch];
                else
                    return call->audio_outputs[0].data64[ch];
            }();
            if (!enabled[0])
                for (std::uint32_t f = 0; f < call->frames_count; ++f)
                    if (input[f] != T{} ||
                        !(call->audio_inputs[0].constant_mask & (std::uint64_t{1} << ch)))
                        return CLAP_PROCESS_ERROR;
            // Deliberately leave a disabled output untouched: the host must zero it.
            if (enabled[1])
                for (std::uint32_t f = 0; f < call->frames_count; ++f)
                    output[f] = input[f] + T{1};
        }
        switch (command) {
        case ClapFixtureCommand::processSleep:
            return CLAP_PROCESS_SLEEP;
        case ClapFixtureCommand::processTail:
            return CLAP_PROCESS_TAIL;
        case ClapFixtureCommand::processQuiet:
            return CLAP_PROCESS_CONTINUE_IF_NOT_QUIET;
        case ClapFixtureCommand::processError:
            return CLAP_PROCESS_ERROR;
        default:
            return CLAP_PROCESS_CONTINUE;
        }
    }
    explicit Plugin(const clap_host_t* h) : host(h) {
        ++stats.created;
        livePlugin = this;
        api = {&descriptor,
               this,
               [](const clap_plugin_t* p) noexcept {
                   auto& s = self(p);
                   if (!s.thread(false) || std::strcmp(s.host->name, "CyAudioCore") != 0)
                       return false;
                   for (const char* id :
                        {CLAP_EXT_LOG, CLAP_EXT_EVENT_REGISTRY, CLAP_EXT_AUDIO_PORTS_CONFIG,
                         CLAP_EXT_SURROUND, CLAP_EXT_AMBISONIC, CLAP_EXT_VOICE_INFO,
                         CLAP_EXT_NOTE_NAME, CLAP_EXT_TRACK_INFO, CLAP_EXT_REMOTE_CONTROLS,
                         CLAP_EXT_PRESET_LOAD})
                       if (!s.host->get_extension(s.host, id))
                           return false;
                   auto* registry = s.ext<clap_host_event_registry_t>(CLAP_EXT_EVENT_REGISTRY);
                   std::uint16_t space = 4;
                   if (registry->query(s.host, "org.example.unknown", &space) ||
                       space != UINT16_MAX)
                       return false;
                   if (const auto* pool = s.ext<clap_host_thread_pool_t>(CLAP_EXT_THREAD_POOL))
                       if (pool->request_exec(s.host, 4))
                           return false; // Main-thread execution is rejected.
                   clap_track_info_t info{};
                   if (s.ext<clap_host_track_info_t>(CLAP_EXT_TRACK_INFO)->get(s.host, &info))
                       ++stats.trackQueries;
                   s.host->request_callback(s.host);
                   return true;
               },
               [](const clap_plugin_t* p) noexcept {
                   ++stats.destroyed;
                   delete &self(p);
               },
               [](const clap_plugin_t* p, double, std::uint32_t, std::uint32_t) noexcept {
                   auto& s = self(p);
                   s.active = s.thread(false) && command != ClapFixtureCommand::failActivation;
                   return s.active;
               },
               [](const clap_plugin_t* p) noexcept { self(p).active = false; },
               [](const clap_plugin_t* p) noexcept {
                   auto& s = self(p);
                   s.processing = s.active && s.thread(true);
                   return s.processing;
               },
               [](const clap_plugin_t* p) noexcept { self(p).processing = false; },
               [](const clap_plugin_t*) noexcept {},
               [](const clap_plugin_t* p, const clap_process_t* call) noexcept {
                   auto& s = self(p);
                   return call->audio_outputs[0].data64 ? s.process<double>(call)
                                                        : s.process<float>(call);
               },
               &extension,
               [](const clap_plugin_t* p) noexcept {
                   auto& s = self(p);
                   if (!s.thread(false))
                       return;
                   s.changed();
                   if (command == ClapFixtureCommand::guiDestroyed && s.editor) {
                       s.guiLost = true;
                       s.ext<clap_host_gui_t>(CLAP_EXT_GUI)->closed(s.host, true);
                   }
                   if (command == ClapFixtureCommand::concurrentLogs) {
                       std::array<std::thread, 4> threads;
                       for (auto& thread : threads)
                           thread = std::thread([&s] {
                               for (unsigned i = 0; i < 10; ++i)
                                   s.ext<clap_host_log_t>(CLAP_EXT_LOG)
                                       ->log(s.host, CLAP_LOG_INFO, "concurrent log");
                           });
                       for (auto& thread : threads)
                           thread.join();
                   }
                   if (command == ClapFixtureCommand::floodLogs)
                       for (unsigned i = 0; i < 100; ++i)
                           s.ext<clap_host_log_t>(CLAP_EXT_LOG)
                               ->log(s.host, CLAP_LOG_WARNING, "bounded log");
               }};
    }
    ~Plugin() {
        if (livePlugin == this)
            livePlugin = nullptr;
    }
    static const void* CLAP_ABI extension(const clap_plugin_t*, const char*) noexcept;
};
const void* CLAP_ABI Plugin::extension(const clap_plugin_t*, const char* id) noexcept {
    static const clap_plugin_audio_ports_t ports{
        [](const clap_plugin_t*, bool) noexcept { return 1u; },
        [](const clap_plugin_t* p, std::uint32_t i, bool, clap_audio_port_info_t* out) noexcept {
            return i == 0 && port(self(p).channels, out);
        }};
    static const clap_plugin_note_ports_t notes{
        [](const clap_plugin_t*, bool) noexcept { return 1u; },
        [](const clap_plugin_t*, std::uint32_t i, bool, clap_note_port_info_t* out) noexcept {
            if (i || !out)
                return false;
            *out = {};
            out->id = 3;
            out->supported_dialects = 15;
            out->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
            std::snprintf(out->name, sizeof(out->name), "Notes");
            return true;
        }};
    static const clap_plugin_params_t params{
        [](const clap_plugin_t*) noexcept { return 1u; },
        [](const clap_plugin_t* p, std::uint32_t i, clap_param_info_t* out) noexcept {
            if (i || !out)
                return false;
            *out = {};
            out->id = 7;
            out->cookie = &self(p).cookie;
            out->max_value = 1;
            out->default_value = .5;
            out->flags =
                CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE | CLAP_PARAM_IS_PERIODIC |
                CLAP_PARAM_IS_AUTOMATABLE_PER_NOTE_ID | CLAP_PARAM_IS_AUTOMATABLE_PER_PORT |
                CLAP_PARAM_IS_AUTOMATABLE_PER_CHANNEL | CLAP_PARAM_IS_AUTOMATABLE_PER_KEY |
                CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID | CLAP_PARAM_IS_MODULATABLE_PER_PORT |
                CLAP_PARAM_IS_MODULATABLE_PER_CHANNEL | CLAP_PARAM_IS_MODULATABLE_PER_KEY;
            std::snprintf(out->name, sizeof(out->name), "Amount");
            return true;
        },
        [](const clap_plugin_t* p, clap_id id, double* out) noexcept {
            if (id != 7 || !out)
                return false;
            *out = self(p).value;
            return true;
        },
        [](const clap_plugin_t*, clap_id id, double v, char* out, std::uint32_t n) noexcept {
            return id == 7 && out && std::snprintf(out, n, "%.2f", v) >= 0;
        },
        [](const clap_plugin_t*, clap_id id, const char* text, double* out) noexcept {
            if (id != 7 || !text || !out)
                return false;
            char* end = nullptr;
            *out = std::strtod(text, &end);
            return end != text && !*end;
        },
        [](const clap_plugin_t* p, const clap_input_events_t* in,
           const clap_output_events_t* out) noexcept {
            auto& s = self(p);
            if (!s.thread(s.active))
                return;
            ++stats.flushes;
            for (std::uint32_t i = 0; i < in->size(in); ++i)
                (void)out->try_push(out, in->get(in, i));
        }};
    static const clap_plugin_audio_ports_config_t configs{
        [](const clap_plugin_t*) noexcept {
            return command == ClapFixtureCommand::oversizeConfig ? 99999u : 4u;
        },
        [](const clap_plugin_t*, std::uint32_t i, clap_audio_ports_config_t* out) noexcept {
            if (i >= 4 || !out)
                return false;
            *out = {};
            out->id = 10 + i;
            out->input_port_count = out->output_port_count = 1;
            out->has_main_input = out->has_main_output = true;
            const std::uint32_t count[]{2, 1, 6, 4};
            out->main_input_channel_count = out->main_output_channel_count = count[i];
            clap_audio_port_info_t p{};
            port(count[i], &p);
            out->main_input_port_type = out->main_output_port_type = p.port_type;
            std::snprintf(out->name, sizeof(out->name), "Config %u", count[i]);
            return true;
        },
        [](const clap_plugin_t* p, clap_id id) noexcept {
            auto& s = self(p);
            if (s.active || id < 10 || id > 13)
                return false;
            const std::uint32_t count[]{2, 1, 6, 4};
            s.channels = count[id - 10];
            s.configuration = id;
            s.enabled = {true, true};
            return true;
        }};
    static const clap_plugin_audio_ports_config_info_t configInfo{
        [](const clap_plugin_t* p) noexcept { return self(p).configuration; },
        [](const clap_plugin_t*, clap_id id, std::uint32_t index, bool,
           clap_audio_port_info_t* out) noexcept {
            const std::uint32_t count[]{2, 1, 6, 4};
            return !index && id >= 10 && id <= 13 && port(count[id - 10], out);
        }};
    static const clap_plugin_configurable_audio_ports_t configurable{
        [](const clap_plugin_t*, const clap_audio_port_configuration_request_t* r,
           std::uint32_t n) noexcept {
            return n == 2 && r && r[0].port_index == 0 && r[1].port_index == 0 &&
                   r[0].is_input != r[1].is_input && r[0].channel_count == r[1].channel_count &&
                   (r[0].channel_count == 1 || r[0].channel_count == 2);
        },
        [](const clap_plugin_t* p, const clap_audio_port_configuration_request_t* r,
           std::uint32_t n) noexcept {
            if (self(p).active || n != 2 || !r || r[0].channel_count != r[1].channel_count)
                return false;
            self(p).channels = r[0].channel_count;
            self(p).configuration = CLAP_INVALID_ID;
            return true;
        }};
    static const clap_plugin_audio_ports_activation_t activation{
        [](const clap_plugin_t*) noexcept { return true; },
        [](const clap_plugin_t* p, bool input, std::uint32_t index, bool active,
           std::uint32_t) noexcept {
            auto& s = self(p);
            if (index || !s.thread(s.active))
                return false;
            s.enabled[input ? 0 : 1] = active;
            return true;
        }};
    static const clap_plugin_surround_t surround{
        [](const clap_plugin_t*, std::uint64_t) noexcept { return true; },
        [](const clap_plugin_t*, bool, std::uint32_t, std::uint8_t* map, std::uint32_t n) noexcept {
            if (n < 6)
                return 0u;
            const std::uint8_t ids[]{CLAP_SURROUND_FL,  CLAP_SURROUND_FR, CLAP_SURROUND_FC,
                                     CLAP_SURROUND_LFE, CLAP_SURROUND_SL, CLAP_SURROUND_SR};
            std::copy(std::begin(ids), std::end(ids), map);
            if (command == ClapFixtureCommand::invalidSurround)
                map[1] = 127;
            return 6u;
        }};
    static const clap_plugin_ambisonic_t ambisonic{
        [](const clap_plugin_t*, const clap_ambisonic_config_t*) noexcept { return true; },
        [](const clap_plugin_t*, bool, std::uint32_t, clap_ambisonic_config_t* out) noexcept {
            *out = {CLAP_AMBISONIC_ORDERING_ACN, CLAP_AMBISONIC_NORMALIZATION_SN3D};
            return true;
        }};
    static const clap_plugin_voice_info_t voices{
        [](const clap_plugin_t* p, clap_voice_info_t* out) noexcept {
            if (!self(p).active)
                return false;
            *out = {8, 32, CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES};
            return true;
        }};
    static const clap_plugin_note_name_t names{
        [](const clap_plugin_t*) noexcept { return 1u; },
        [](const clap_plugin_t*, std::uint32_t i, clap_note_name_t* out) noexcept {
            if (i)
                return false;
            *out = {};
            out->port = 0;
            out->channel = -1;
            out->key = 36;
            std::snprintf(out->name, sizeof(out->name), "Kick");
            return true;
        }};
    static const clap_plugin_remote_controls_t remotes{
        [](const clap_plugin_t*) noexcept { return 1u; },
        [](const clap_plugin_t*, std::uint32_t i, clap_remote_controls_page_t* out) noexcept {
            if (i)
                return false;
            *out = {};
            out->page_id = 99;
            out->is_for_preset = true;
            std::fill(std::begin(out->param_ids), std::end(out->param_ids), CLAP_INVALID_ID);
            out->param_ids[0] = 7;
            std::snprintf(out->section_name, sizeof(out->section_name), "Main");
            std::snprintf(out->page_name, sizeof(out->page_name), "Controls");
            return true;
        }};
    static const clap_plugin_track_info_t track{[](const clap_plugin_t* p) noexcept {
        auto& s = self(p);
        clap_track_info_t out{};
        if (s.ext<clap_host_track_info_t>(CLAP_EXT_TRACK_INFO)->get(s.host, &out))
            ++stats.trackQueries;
    }};
    static const clap_plugin_param_indication_t indication{
        [](const clap_plugin_t*, clap_id id, bool mapped, const clap_color_t*, const char*,
           const char*) noexcept {
            if (id == 7 && mapped)
                ++stats.mapping;
        },
        [](const clap_plugin_t*, clap_id id, std::uint32_t value, const clap_color_t*) noexcept {
            if (id == 7)
                stats.automation = value;
        }};
    static const clap_plugin_state_t state{
        [](const clap_plugin_t* p, const clap_ostream_t* out) noexcept {
            const auto& v = self(p).value;
            return out->write(out, &v, sizeof(v)) == sizeof(v);
        },
        [](const clap_plugin_t* p, const clap_istream_t* in) noexcept {
            double v = 0;
            if (in->read(in, &v, sizeof(v)) != sizeof(v))
                return false;
            self(p).value = v;
            return true;
        }};
    static const clap_plugin_state_context_t stateContext{
        [](const clap_plugin_t* p, const clap_ostream_t* out, std::uint32_t context) noexcept {
            stats.stateSaveContext = context;
            const auto* s = static_cast<const clap_plugin_state_t*>(extension(p, CLAP_EXT_STATE));
            return s->save(p, out);
        },
        [](const clap_plugin_t* p, const clap_istream_t* in, std::uint32_t context) noexcept {
            stats.stateLoadContext = context;
            const auto* s = static_cast<const clap_plugin_state_t*>(extension(p, CLAP_EXT_STATE));
            return s->load(p, in);
        }};
    static const clap_plugin_preset_load_t preset{[](const clap_plugin_t* p, std::uint32_t kind,
                                                     const char* location,
                                                     const char* key) noexcept {
        auto& s = self(p);
        const auto* h = s.ext<clap_host_preset_load_t>(CLAP_EXT_PRESET_LOAD);
        if (!key || std::strcmp(key, "warm") != 0) {
            h->on_error(s.host, kind, location, key, 23, "Unknown fixture preset");
            return false;
        }
        s.value = .75;
        h->loaded(s.host, kind, location, key);
        return true;
    }};
    static const clap_plugin_context_menu_t menu{
        [](const clap_plugin_t*, const clap_context_menu_target_t*,
           const clap_context_menu_builder_t* b) noexcept {
            const clap_context_menu_check_entry_t entry{"Fixture action", true, true, 55};
            return b->supports(b, CLAP_CONTEXT_MENU_ITEM_CHECK_ENTRY) &&
                   b->add_item(b, CLAP_CONTEXT_MENU_ITEM_CHECK_ENTRY, &entry);
        },
        [](const clap_plugin_t* p, const clap_context_menu_target_t*, clap_id action) noexcept {
            auto& s = self(p);
            const auto* hostMenu = s.ext<clap_host_context_menu_t>(CLAP_EXT_CONTEXT_MENU);
            if (action != 55 || !hostMenu || hostMenu->can_popup(s.host))
                return false;
            unsigned entries = 0;
            const clap_context_menu_builder_t builder{
                &entries,
                [](const clap_context_menu_builder_t* b, std::uint32_t kind, const void*) noexcept {
                    if (kind == CLAP_CONTEXT_MENU_ITEM_ENTRY)
                        ++*static_cast<unsigned*>(b->ctx);
                    return true;
                },
                [](const clap_context_menu_builder_t*, std::uint32_t k) noexcept {
                    return k <= CLAP_CONTEXT_MENU_ITEM_TITLE;
                }};
            return hostMenu->populate(s.host, nullptr, &builder) && entries == 1 &&
                   hostMenu->perform(s.host, nullptr, 88);
        }};
    static const clap_plugin_render_t render{
        [](const clap_plugin_t*) noexcept { return command == ClapFixtureCommand::hardRealtime; },
        [](const clap_plugin_t*, clap_plugin_render_mode mode) noexcept {
            stats.renderMode = std::uint32_t(mode);
            return true;
        }};
    static const clap_plugin_gui_t gui = [] {
        clap_plugin_gui_t g{};
        g.is_api_supported = [](const clap_plugin_t*, const char* api, bool floating) noexcept {
            return floating && (!api || !*api);
        };
        g.get_preferred_api = [](const clap_plugin_t*, const char** api, bool* floating) noexcept {
            *api = nullptr;
            *floating = true;
            return true;
        };
        g.create = [](const clap_plugin_t* p, const char* api, bool floating) noexcept {
            return self(p).editor = !api && floating;
        };
        g.destroy = [](const clap_plugin_t* p) noexcept {
            auto& s = self(p);
            if (s.guiLost)
                ++stats.guiAcknowledged;
            s.guiLost = s.editor = false;
        };
        g.get_size = [](const clap_plugin_t*, std::uint32_t* w, std::uint32_t* h) noexcept {
            *w = 420;
            *h = 210;
            return true;
        };
        g.show = [](const clap_plugin_t* p) noexcept { return self(p).editor; };
        g.hide = [](const clap_plugin_t* p) noexcept {
            if (self(p).guiLost)
                ++stats.badGuiHide;
            return self(p).editor;
        };
        g.suggest_title = [](const clap_plugin_t*, const char* title) noexcept {
            if (title && std::strcmp(title, "Fixture title") == 0)
                ++stats.guiTitles;
        };
        return g;
    }();
    static const clap_plugin_thread_pool_t pool{[](const clap_plugin_t* p, std::uint32_t) noexcept {
        auto& s = self(p);
        if (s.thread(true) &&
            !s.ext<clap_host_thread_pool_t>(CLAP_EXT_THREAD_POOL)->request_exec(s.host, 1))
            ++stats.poolTasks;
    }};
#define EXT(name, object)                                                                          \
    if (std::strcmp(id, name) == 0)                                                                \
    return &object
    EXT(CLAP_EXT_AUDIO_PORTS, ports);
    EXT(CLAP_EXT_NOTE_PORTS, notes);
    EXT(CLAP_EXT_PARAMS, params);
    EXT(CLAP_EXT_AUDIO_PORTS_CONFIG, configs);
    EXT(CLAP_EXT_AUDIO_PORTS_CONFIG_INFO, configInfo);
    EXT(CLAP_EXT_CONFIGURABLE_AUDIO_PORTS, configurable);
    EXT(CLAP_EXT_AUDIO_PORTS_ACTIVATION, activation);
    EXT(CLAP_EXT_SURROUND, surround);
    EXT(CLAP_EXT_AMBISONIC, ambisonic);
    EXT(CLAP_EXT_VOICE_INFO, voices);
    EXT(CLAP_EXT_NOTE_NAME, names);
    EXT(CLAP_EXT_REMOTE_CONTROLS, remotes);
    EXT(CLAP_EXT_TRACK_INFO, track);
    EXT(CLAP_EXT_PARAM_INDICATION, indication);
    EXT(CLAP_EXT_STATE, state);
    EXT(CLAP_EXT_STATE_CONTEXT, stateContext);
    EXT(CLAP_EXT_PRESET_LOAD, preset);
    EXT(CLAP_EXT_CONTEXT_MENU, menu);
    EXT(CLAP_EXT_RENDER, render);
    EXT(CLAP_EXT_THREAD_POOL, pool);
    EXT(CLAP_EXT_GUI, gui);
#undef EXT
    return nullptr;
}

const clap_preset_discovery_provider_descriptor_t presetDescriptor{
    CLAP_VERSION, "org.cyaudio.presets", "Fixture presets", "Example"};
struct Provider {
    clap_preset_discovery_provider_t api;
    const clap_preset_discovery_indexer_t* indexer;
    explicit Provider(const clap_preset_discovery_indexer_t* i) : indexer(i) {
        ++stats.providerCreated;
        api = {&presetDescriptor,
               this,
               [](const clap_preset_discovery_provider_t* p) noexcept {
                   auto* i = static_cast<Provider*>(p->provider_data)->indexer;
                   const clap_preset_discovery_filetype_t type{"Fixture", "Fixture native preset",
                                                               "fixture"};
                   const clap_preset_discovery_location_t location{
                       CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT, "Factory",
                       CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr};
                   const clap_preset_discovery_soundpack_t pack{
                       CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT,
                       "example.pack",
                       "Factory Pack",
                       "Fixture",
                       nullptr,
                       "Example",
                       nullptr,
                       CLAP_TIMESTAMP_UNKNOWN};
                   return i->declare_filetype(i, &type) && i->declare_location(i, &location) &&
                          i->declare_soundpack(i, &pack) &&
                          command != ClapFixtureCommand::failProviderInit;
               },
               [](const clap_preset_discovery_provider_t* p) noexcept {
                   ++stats.providerDestroyed;
                   delete static_cast<Provider*>(p->provider_data);
               },
               [](const clap_preset_discovery_provider_t*, std::uint32_t, const char*,
                  const clap_preset_discovery_metadata_receiver_t* r) noexcept {
                   if (command == ClapFixtureCommand::invalidMetadata) {
                       r->set_description(r, "Missing begin");
                       return true;
                   }
                   if (!r->begin_preset(r, "Warm", "warm"))
                       return false;
                   const clap_universal_plugin_id_t id{"clap", "org.cyaudio.advanced"};
                   r->add_plugin_id(r, &id);
                   r->set_soundpack_id(r, "example.pack");
                   r->set_flags(r, CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT);
                   r->add_creator(r, "Example");
                   r->set_description(r, "Fixture warm preset");
                   r->set_timestamps(r, CLAP_TIMESTAMP_UNKNOWN, 123);
                   r->add_feature(r, "instrument");
                   r->add_extra_info(r, "example", "value");
                   return true;
               },
               [](const clap_preset_discovery_provider_t*, const char*) noexcept -> const void* {
                   return nullptr;
               }};
    }
};
const clap_plugin_factory_t factory{[](const clap_plugin_factory_t*) noexcept { return 1u; },
                                    [](const clap_plugin_factory_t*, std::uint32_t i) noexcept {
                                        return i ? nullptr : &descriptor;
                                    },
                                    [](const clap_plugin_factory_t*, const clap_host_t* host,
                                       const char* id) noexcept -> const clap_plugin_t* {
                                        if (!host || !id || std::strcmp(id, descriptor.id))
                                            return nullptr;
                                        auto* p = new (std::nothrow) Plugin(host);
                                        return p ? &p->api : nullptr;
                                    }};
const clap_preset_discovery_factory_t presetFactory{
    [](const clap_preset_discovery_factory_t*) noexcept { return 1u; },
    [](const clap_preset_discovery_factory_t*, std::uint32_t i) noexcept {
        return i ? nullptr : &presetDescriptor;
    },
    [](const clap_preset_discovery_factory_t*, const clap_preset_discovery_indexer_t* i,
       const char* id) noexcept -> const clap_preset_discovery_provider_t* {
        if (!i || !id || std::strcmp(id, presetDescriptor.id))
            return nullptr;
        auto* p = new (std::nothrow) Provider(i);
        return p ? &p->api : nullptr;
    }};
} // namespace
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry{
    CLAP_VERSION,
    [](const char*) noexcept {
        ++stats.entryInit;
        return true;
    },
    []() noexcept { ++stats.entryDeinit; },
    [](const char* id) noexcept -> const void* {
        if (id && std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0)
            return &factory;
        if (id && std::strcmp(id, CLAP_PRESET_DISCOVERY_FACTORY_ID) == 0)
            return &presetFactory;
        return nullptr;
    }};
extern "C" CLAP_EXPORT ClapFixtureStats fixtureRead() {
    return stats;
}
extern "C" CLAP_EXPORT void fixtureControl(ClapFixtureCommand c) {
    command = c;
    if (livePlugin)
        livePlugin->host->request_callback(livePlugin->host);
}
