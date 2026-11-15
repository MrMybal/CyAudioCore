// Original CLAP fixture. No hardware or UI.
#include <atomic>
#include <clap/clap.h>
#include <cstdio>
#include <cstring>
#include <new>
namespace {
const char* const features[]{CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};
const clap_plugin_descriptor_t descriptor{
    CLAP_VERSION, "org.cyaudio.sleep", "Sleep fixture", "Example", "", "", "",
    "1",          "Sleep contracts",   features};
struct Fixture {
    clap_plugin_t api{};
    const clap_host_t* host;
    std::atomic<unsigned> blocks{0}, starts{0}, stops{0}, pulse{0};
    unsigned mode = 0;
    bool running = false;
    static Fixture& self(const clap_plugin_t* p) {
        return *static_cast<Fixture*>(p->plugin_data);
    }
    template <class T> void render(const clap_process_t& p) {
        for (unsigned ch = 0; ch < 2; ++ch) {
            const T* input;
            T* output;
            if constexpr (sizeof(T) == sizeof(float)) {
                input = p.audio_inputs[0].data32[ch];
                output = p.audio_outputs[0].data32[ch];
            } else {
                input = p.audio_inputs[0].data64[ch];
                output = p.audio_outputs[0].data64[ch];
            }
            for (unsigned i = 0; i < p.frames_count; ++i)
                output[i] = input[i];
            if (mode == 6)
                output[p.frames_count - 1] = T(.5);
        }
    }
    explicit Fixture(const clap_host_t* h) : host(h) {
        api.desc = &descriptor;
        api.plugin_data = this;
        api.init = [](const clap_plugin_t*) { return true; };
        api.destroy = [](const clap_plugin_t* p) { delete &self(p); };
        api.activate = [](const clap_plugin_t*, double, uint32_t, uint32_t) { return true; };
        api.deactivate = [](const clap_plugin_t*) {};
        api.start_processing = [](const clap_plugin_t* p) {
            auto& s = self(p);
            if (s.running)
                return false;
            s.running = true;
            ++s.starts;
            return true;
        };
        api.stop_processing = [](const clap_plugin_t* p) {
            auto& s = self(p);
            s.running = false;
            ++s.stops;
        };
        api.reset = [](const clap_plugin_t*) {};
        api.on_main_thread = [](const clap_plugin_t*) {};
        api.process = [](const clap_plugin_t* p, const clap_process_t* c) -> clap_process_status {
            auto& s = self(p);
            if (!s.running)
                return CLAP_PROCESS_ERROR;
            ++s.blocks;
            for (unsigned i = 0; i < c->in_events->size(c->in_events); ++i) {
                const auto* e = c->in_events->get(c->in_events, i);
                if (e->type == CLAP_EVENT_PARAM_VALUE)
                    s.mode = unsigned(reinterpret_cast<const clap_event_param_value_t*>(e)->value);
            }
            if (c->audio_outputs[0].data64)
                s.render<double>(*c);
            else
                s.render<float>(*c);
            if (s.pulse.exchange(0)) {
                if (c->audio_outputs[0].data64)
                    c->audio_outputs[0].data64[0][0] = .25;
                else
                    c->audio_outputs[0].data32[0][0] = .25f;
            }
            if (s.mode == 5) {
                clap_event_midi_t e{};
                e.header = {sizeof(e), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
                e.data[0] = 0x90;
                e.data[1] = 60;
                e.data[2] = 100;
                c->out_events->try_push(c->out_events, &e.header);
            }
            return s.mode == 1                  ? CLAP_PROCESS_CONTINUE
                   : s.mode == 2 || s.mode == 5 ? CLAP_PROCESS_CONTINUE_IF_NOT_QUIET
                   : s.mode == 3                ? CLAP_PROCESS_TAIL
                   : s.mode == 4                ? CLAP_PROCESS_ERROR
                                                : CLAP_PROCESS_SLEEP;
        };
        api.get_extension = [](const clap_plugin_t*, const char* id) -> const void* {
            static const clap_plugin_audio_ports_t ports{
                [](const clap_plugin_t*, bool) { return 1u; },
                [](const clap_plugin_t*, uint32_t i, bool, clap_audio_port_info_t* v) {
                    if (i)
                        return false;
                    *v = {};
                    v->id = 1;
                    v->channel_count = 2;
                    v->in_place_pair = CLAP_INVALID_ID;
                    v->flags = CLAP_AUDIO_PORT_IS_MAIN | CLAP_AUDIO_PORT_SUPPORTS_64BITS;
                    v->port_type = CLAP_PORT_STEREO;
                    std::snprintf(v->name, sizeof(v->name), "Main");
                    return true;
                }};
            static const clap_plugin_note_ports_t notes{
                [](const clap_plugin_t*, bool input) { return input ? 0u : 1u; },
                [](const clap_plugin_t*, uint32_t i, bool input, clap_note_port_info_t* v) {
                    if (input || i)
                        return false;
                    *v = {};
                    v->id = 2;
                    v->supported_dialects = v->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
                    std::snprintf(v->name, sizeof(v->name), "Events");
                    return true;
                }};
            static const clap_plugin_params_t params{
                [](const clap_plugin_t*) { return 5u; },
                [](const clap_plugin_t*, uint32_t i, clap_param_info_t* v) {
                    if (i >= 5)
                        return false;
                    *v = {};
                    v->id = i;
                    v->min_value = 0;
                    v->max_value = i == 0 ? 6 : 1e9;
                    v->flags = i == 0 ? CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_AUTOMATABLE
                                      : CLAP_PARAM_IS_READONLY;
                    std::snprintf(v->name, sizeof(v->name), "Control %u", i);
                    return true;
                },
                [](const clap_plugin_t* p, clap_id i, double* v) {
                    auto& s = self(p);
                    if (i > 4)
                        return false;
                    if (i == 3) {
                        s.pulse.store(1);
                        s.host->request_process(s.host);
                    }
                    *v = i == 1   ? double(s.starts.load())
                         : i == 2 ? double(s.blocks.load())
                         : i == 4 ? double(s.stops.load())
                                  : 0;
                    return true;
                },
                nullptr,
                nullptr,
                [](const clap_plugin_t*, const clap_input_events_t*, const clap_output_events_t*) {
                }};
            static const clap_plugin_latency_t latency{[](const clap_plugin_t*) { return 0u; }};
            static const clap_plugin_audio_ports_activation_t activation{
                [](const clap_plugin_t*) { return true; },
                [](const clap_plugin_t*, bool, uint32_t index, bool, uint32_t bits) {
                    return index == 0 && (bits == 0 || bits == 32 || bits == 64);
                }};
            static const clap_plugin_tail_t tail{[](const clap_plugin_t*) { return 16u; }};
            if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS))
                return &ports;
            if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS))
                return &notes;
            if (!std::strcmp(id, CLAP_EXT_PARAMS))
                return &params;
            if (!std::strcmp(id, CLAP_EXT_LATENCY))
                return &latency;
            if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS_ACTIVATION))
                return &activation;
            if (!std::strcmp(id, CLAP_EXT_TAIL))
                return &tail;
            return nullptr;
        };
    }
};
const clap_plugin_factory_t factory{
    [](const clap_plugin_factory_t*) { return 1u; },
    [](const clap_plugin_factory_t*, uint32_t i) { return i ? nullptr : &descriptor; },
    [](const clap_plugin_factory_t*, const clap_host_t* h, const char* id) -> const clap_plugin_t* {
        if (std::strcmp(id, descriptor.id))
            return nullptr;
        auto* p = new (std::nothrow) Fixture(h);
        return p ? &p->api : nullptr;
    }};
} // namespace
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry{
    CLAP_VERSION, [](const char*) { return true; }, []() {},
    [](const char* id) -> const void* {
        return !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &factory : nullptr;
    }};
