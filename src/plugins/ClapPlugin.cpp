#include "cyaudio/core/DspTypes.h"
// Original adapter, pinned MIT interfaces: third_party/licenses/clap.txt
#include "ClapEvents.h"
#include "ClapModule.h"
#include "cyaudio/plugins/ClapExtensions.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#if !defined(_WIN32)
#include <poll.h>
#endif
namespace cyaudio::detail {
namespace {
std::recursive_mutex& moduleMutex() {
    static std::recursive_mutex m;
    return m;
}
} // namespace
ClapModule::~ClapModule() {
    if (initialized) {
        std::lock_guard lock(moduleMutex());
        entry->deinit();
    }
}
struct ModuleRecord {
    std::filesystem::path path;
    std::weak_ptr<ClapModule> module;
};
Status loadClapModule(const std::filesystem::path& path, std::shared_ptr<ClapModule>& out,
                      bool requirePluginFactory) {
    if (!path.is_absolute())
        return Status::invalid_argument;
    std::error_code error;
    const auto canonical = std::filesystem::canonical(path, error);
    if (error || !canonical.is_absolute())
        return Status::io_error;
    static std::vector<ModuleRecord> modules;
    std::unique_lock lock(moduleMutex());
    for (auto it = modules.begin(); it != modules.end();) {
        if (auto m = it->module.lock()) {
            if (it->path == canonical) {
                if (requirePluginFactory && !m->factory)
                    return Status::unsupported_format;
                out = std::move(m);
                return Status::ok;
            }
            ++it;
        } else
            it = modules.erase(it);
    }
    auto next = std::make_shared<ClapModule>();
    auto r = next->library.open(canonical);
    if (r == Status::ok) {
        next->entry = static_cast<const clap_plugin_entry_t*>(next->library.symbol("clap_entry"));
        const auto* e = next->entry;
        if (!e || e->clap_version.major != 1 || !e->init || !e->deinit || !e->get_factory)
            r = Status::incompatible_version;
        else {
            const auto utf8 = canonical.u8string();
            next->initialized = e->init(reinterpret_cast<const char*>(utf8.c_str()));
            if (!next->initialized)
                r = Status::io_error;
            else {
                next->factory = static_cast<const clap_plugin_factory_t*>(
                    e->get_factory(CLAP_PLUGIN_FACTORY_ID));
                if (next->factory &&
                    (!next->factory->create_plugin || !next->factory->get_plugin_count ||
                     !next->factory->get_plugin_descriptor))
                    next->factory = nullptr;
                if (requirePluginFactory && !next->factory)
                    r = Status::unsupported_format;
            }
        }
    }
    if (r == Status::ok) {
        modules.push_back({canonical, next});
        out = next;
    }
    lock.unlock(); // Last reference/deinit may acquire the registry mutex.
    return r;
}
namespace {
std::string text(const char* p) {
    if (!p)
        return {};
    std::size_t n = 0;
    while (n < 65536 && p[n])
        ++n;
    if (n == 65536)
        throw std::length_error("plugin string limit");
    return {p, n};
}
template <std::size_t N> std::string textFixed(const char (&p)[N]) {
    return {p, std::find(p, p + N, char{})};
}
PluginDescriptor descriptor(const clap_plugin_descriptor_t* d) {
    if (!d || d->clap_version.major != 1)
        throw std::invalid_argument("CLAP version");
    PluginDescriptor result{PluginFormat::clap, text(d->id),      text(d->name),
                            text(d->vendor),    text(d->version), text(d->description)};
    if (result.id.empty() || result.name.empty())
        throw std::invalid_argument("CLAP descriptor");
    return result;
}
std::uint64_t milliseconds() noexcept {
    const auto n = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
    return n > 0 ? std::uint64_t(n) : 0;
}
thread_local bool clapPoolTask = false;
bool validString(std::string_view, std::size_t maximum = 65535) noexcept;
bool validTrack(const ClapTrackInfo&) noexcept;
class ClapBackend final : public PluginBackend, public ClapExtensions {
    std::shared_ptr<ClapModule> module_;
    const clap_plugin_t* plugin_ = nullptr;
    clap_host_t host_{};
    PluginDescription description_;
    PluginHostCallbacks callbacks_;
    ClapHostServices services_;
    std::optional<ClapTrackInfo> track_;
    std::thread::id main_ = std::this_thread::get_id();
    std::atomic<std::uint32_t> requests_{0};
    std::atomic<std::uint32_t> wakeGeneration_{0};
    const clap_plugin_audio_ports_t* audio_ = nullptr;
    const clap_plugin_note_ports_t* notes_ = nullptr;
    const clap_plugin_params_t* params_ = nullptr;
    const clap_plugin_state_t* state_ = nullptr;
    const clap_plugin_gui_t* gui_ = nullptr;
    const clap_plugin_latency_t* latency_ = nullptr;
    const clap_plugin_tail_t* tail_ = nullptr;
    const clap_plugin_render_t* render_ = nullptr;
    const clap_plugin_timer_support_t* timers_ = nullptr;
    const clap_plugin_posix_fd_support_t* fds_ = nullptr;
    const clap_plugin_audio_ports_config_t* configs_ = nullptr;
    const clap_plugin_audio_ports_config_info_t* configInfo_ = nullptr;
    const clap_plugin_configurable_audio_ports_t* configurable_ = nullptr;
    const clap_plugin_audio_ports_activation_t* portActivation_ = nullptr;
    const clap_plugin_surround_t* surround_ = nullptr;
    const clap_plugin_ambisonic_t* ambisonic_ = nullptr;
    const clap_plugin_voice_info_t* voices_ = nullptr;
    const clap_plugin_note_name_t* names_ = nullptr;
    const clap_plugin_remote_controls_t* remotes_ = nullptr;
    const clap_plugin_track_info_t* trackInfo_ = nullptr;
    const clap_plugin_param_indication_t* indication_ = nullptr;
    const clap_plugin_state_context_t* stateContext_ = nullptr;
    const clap_plugin_preset_load_t* preset_ = nullptr;
    const clap_plugin_context_menu_t* menu_ = nullptr;
    const clap_plugin_thread_pool_t* pool_ = nullptr;
    ClapCapabilities capabilities_;
    std::atomic<std::uint32_t> processStatus_{0}, cookiesInvalid_{0};
    std::atomic<std::uint32_t> guiWasDestroyed_{0};
    std::atomic<std::uint32_t> logTicket_{0}, logsDropped_{0};
    struct LogSlot {
        std::atomic<std::uint32_t> state{0}; // free/writing/ready/reading
        std::int32_t severity = 0;
        std::array<char, 512> message{};
    };
    std::array<LogSlot, 64> logs_;
    std::vector<std::uint8_t> inputActive_, outputActive_;
    AudioBuffer<float> silent32_;
    AudioBuffer<double> silent64_;
    bool inProcess_ = false, poolExecuting_ = false;
    bool portStatesInvalid_ = true;
    bool descriptionValid_ = false;
    std::uint32_t selectedStateContext_ = 0;
    struct ParameterCookie {
        std::uint32_t id;
        void* cookie;
    };
    std::vector<ParameterCookie> cookies_;
    std::vector<clap_audio_buffer_t> inputs_, outputs_;
    std::vector<float*> pointers32_;
    std::vector<double*> pointers64_;
    std::vector<ClapEventStorage> inputEvents_;
    std::uint32_t inputCount_ = 0, frames_ = 0;
    PluginEventBuffer* outputEvents_ = nullptr;
    clap_input_events_t in_{};
    clap_output_events_t out_{};
    struct Timer {
        clap_id id = 0;
        std::uint32_t period = 0;
        std::uint64_t next = 0;
    };
    std::array<Timer, 64> timerSlots_{};
    struct Fd {
        int fd = -1;
        std::uint32_t flags = 0;
    };
    std::array<Fd, 64> fdSlots_{};
    clap_id nextTimer_ = 1;
    bool active_ = false, editor_ = false, floating_ = false, cocoa_ = false;
    DroppedEvents dropped_;
    static ClapBackend& self(const clap_host_t* h) noexcept {
        return *static_cast<ClapBackend*>(h->host_data);
    }
    void request(std::uint32_t flag) noexcept {
        // Publish associated callback state before the main-thread acquire/exchange.
        requests_.fetch_or(flag, std::memory_order_release);
        wakeGeneration_.fetch_add(1, std::memory_order_release);
        if (callbacks_.wakeAudio)
            callbacks_.wakeAudio(callbacks_.context);
    }
    bool onMain() const noexcept {
        return main_ == std::this_thread::get_id() && currentAudioPlugin != this;
    }
    Status control(bool inactive = false) const noexcept {
        if (!onMain())
            return Status::invalid_argument;
        return inactive && active_ ? Status::transition_in_progress : Status::ok;
    }
    template <class T> const T* extension(const char* id, const char* compat) noexcept {
        const auto* p = extension<T>(id);
        return p ? p : extension<T>(compat);
    }
    void queueLog(std::int32_t severity, const char* message) noexcept {
        if (!message)
            return;
        auto& slot = logs_[logTicket_.fetch_add(1, std::memory_order_relaxed) % logs_.size()];
        std::uint32_t expected = 0;
        if (!slot.state.compare_exchange_strong(expected, 1, std::memory_order_acquire)) {
            logsDropped_.store(1, std::memory_order_relaxed);
            return;
        }
        slot.severity = severity;
        std::size_t n = 0;
        for (; n + 1 < slot.message.size() && message[n]; ++n)
            slot.message[n] = message[n];
        slot.message[n] = 0;
        if (message[n])
            logsDropped_.store(1, std::memory_order_relaxed);
        slot.state.store(2, std::memory_order_release);
    }
    PluginAudioPort portInfo(const clap_audio_port_info_t& port, bool input, std::uint32_t index,
                             bool layout = true) {
        if (port.id == CLAP_INVALID_ID || !port.channel_count || port.channel_count > 256)
            throw std::invalid_argument("CLAP audio port");
        PluginAudioPort result{port.id,
                               port.channel_count,
                               port.in_place_pair,
                               textFixed(port.name),
                               text(port.port_type),
                               (port.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0,
                               (port.flags & CLAP_AUDIO_PORT_SUPPORTS_64BITS) != 0};
        result.prefers64 = (port.flags & CLAP_AUDIO_PORT_PREFERS_64BITS) != 0;
        result.commonSampleSize = (port.flags & CLAP_AUDIO_PORT_REQUIRES_COMMON_SAMPLE_SIZE) != 0;
        if (layout && result.type == CLAP_PORT_SURROUND && surround_ &&
            surround_->get_channel_map) {
            result.surroundMap.resize(port.channel_count);
            if (surround_->get_channel_map(plugin_, input, index, result.surroundMap.data(),
                                           port.channel_count) != port.channel_count ||
                std::any_of(result.surroundMap.begin(), result.surroundMap.end(),
                            [](auto ch) { return ch > CLAP_SURROUND_TSR; }))
                throw std::invalid_argument("CLAP surround map");
        }
        if (layout && result.type == CLAP_PORT_AMBISONIC && ambisonic_ && ambisonic_->get_config) {
            clap_ambisonic_config_t config{};
            if (!ambisonic_->get_config(plugin_, input, index, &config) ||
                config.ordering > CLAP_AMBISONIC_ORDERING_ACN ||
                config.normalization > CLAP_AMBISONIC_NORMALIZATION_N2D)
                throw std::invalid_argument("CLAP ambisonic config");
            result.ambisonicKnown = true;
            result.ambisonicOrdering = config.ordering;
            result.ambisonicNormalization = config.normalization;
        }
        return result;
    }
    template <class T> const T* extension(const char* id) noexcept {
        return static_cast<const T*>(plugin_->get_extension(plugin_, id));
    }
    static const void* CLAP_ABI getExtension(const clap_host_t*, const char* id) noexcept;
    Status convert(const PluginEventBuffer* in) noexcept {
        inputCount_ = 0;
        if (!in)
            return Status::ok;
        if (in->size() > inputEvents_.size())
            return Status::block_too_large;
        for (std::size_t i = 0; i < in->size(); ++i) {
            const auto& e = *in->event(i);
            void* cookie = nullptr;
            if (e.kind == PluginEventKind::parameterValue ||
                e.kind == PluginEventKind::parameterModulation ||
                e.kind == PluginEventKind::gestureBegin || e.kind == PluginEventKind::gestureEnd) {
                const auto found = std::lower_bound(cookies_.begin(), cookies_.end(), e.id,
                                                    [](auto a, auto b) { return a.id < b; });
                if (found == cookies_.end() || found->id != e.id)
                    return Status::invalid_argument;
                cookie = cookiesInvalid_.load(std::memory_order_acquire) ? nullptr : found->cookie;
                const auto& p = *std::lower_bound(description_.parameters.begin(),
                                                  description_.parameters.end(), e.id,
                                                  [](const auto& a, auto b) { return a.id < b; });
                if (e.kind == PluginEventKind::parameterValue &&
                    (p.readOnly || e.value < p.minimum || e.value > p.maximum))
                    return Status::invalid_argument;
                if (e.kind == PluginEventKind::parameterModulation && !p.modulatable)
                    return Status::unsupported_format;
                if (e.port >= 0 && std::size_t(e.port) >= description_.noteInputs.size())
                    return Status::invalid_argument;
                const auto targets = e.kind == PluginEventKind::parameterModulation
                                         ? p.modulationTargets
                                         : p.automationTargets;
                if ((e.noteId >= 0 && !(targets & parameterNoteId)) ||
                    (e.port >= 0 && !(targets & parameterPort)) ||
                    (e.channel >= 0 && !(targets & parameterChannel)) ||
                    (e.key >= 0 && !(targets & parameterKey)))
                    return Status::unsupported_format;
            }
            std::uint32_t dialect = 0;
            switch (e.kind) {
            case PluginEventKind::noteEnd:
                return Status::invalid_argument;
            case PluginEventKind::noteOn:
            case PluginEventKind::noteOff:
            case PluginEventKind::noteChoke:
            case PluginEventKind::noteExpression:
                dialect = CLAP_NOTE_DIALECT_CLAP;
                break;
            case PluginEventKind::midi1:
            case PluginEventKind::sysex:
                dialect = CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE;
                break;
            case PluginEventKind::ump:
                dialect = CLAP_NOTE_DIALECT_MIDI2;
                break;
            default:
                break;
            }
            if (dialect) {
                if (e.port >= 0) {
                    if (std::size_t(e.port) >= description_.noteInputs.size() ||
                        !(description_.noteInputs[std::size_t(e.port)].dialects & dialect))
                        return Status::unsupported_format;
                } else if (std::none_of(
                               description_.noteInputs.begin(), description_.noteInputs.end(),
                               [dialect](const auto& p) { return (p.dialects & dialect) != 0; }))
                    return Status::unsupported_format;
            }
            encodeClapEvent(e, inputEvents_[inputCount_++], cookie);
        }
        return Status::ok;
    }
    template <Sample T> Status processing(const PluginProcess<T>& p) noexcept {
        const auto converted = convert(p.inputEvents);
        if (converted != Status::ok)
            return converted;
        outputEvents_ = p.outputEvents;
        frames_ = p.frames;
        std::size_t offset = 0;
        const auto buffers = [&](const auto& blocks, auto& native, bool input) {
            for (std::size_t i = 0; i < blocks.size(); ++i) {
                auto& b = native[i];
                b = {};
                b.channel_count = std::uint32_t(blocks[i].channels());
                const bool enabled = (input ? inputActive_ : outputActive_)[i] != 0;
                if (!enabled)
                    b.constant_mask = b.channel_count >= 64
                                          ? ~std::uint64_t{}
                                          : (std::uint64_t{1} << b.channel_count) - 1;
                for (std::size_t ch = 0; ch < blocks[i].channels(); ++ch) {
                    auto* ptr = const_cast<T*>(blocks[i].channel(ch).data());
                    if (!enabled) {
                        if (input) {
                            if constexpr (std::same_as<T, float>)
                                ptr = silent32_.block().channel(0).data();
                            else
                                ptr = silent64_.block().channel(0).data();
                        } else
                            std::fill_n(ptr, p.frames, T{});
                    }
                    if constexpr (std::same_as<T, float>)
                        pointers32_[offset + ch] = ptr;
                    else
                        pointers64_[offset + ch] = ptr;
                }
                if constexpr (std::same_as<T, float>)
                    b.data32 = pointers32_.data() + offset;
                else
                    b.data64 = pointers64_.data() + offset;
                offset += blocks[i].channels();
            }
        };
        buffers(p.inputs, inputs_, true);
        buffers(p.outputs, outputs_, false);
        clap_event_transport_t transport{};
        if (p.transport)
            transport = clapTransport(*p.transport);
        clap_process_t call{p.steadyFrame,
                            p.frames,
                            p.transport ? &transport : nullptr,
                            inputs_.data(),
                            outputs_.data(),
                            std::uint32_t(inputs_.size()),
                            std::uint32_t(outputs_.size()),
                            &in_,
                            &out_};
        inProcess_ = true;
        const auto r = plugin_->process(plugin_, &call);
        inProcess_ = false;
        processStatus_.store(
            r >= CLAP_PROCESS_CONTINUE && r <= CLAP_PROCESS_SLEEP ? std::uint32_t(r) : 0u,
            std::memory_order_release);
        for (std::size_t bus = 0; bus < p.outputs.size(); ++bus)
            if (!outputActive_[bus])
                for (std::size_t ch = 0; ch < p.outputs[bus].channels(); ++ch)
                    std::fill_n(p.outputs[bus].channel(ch).data(), p.frames, T{});
        outputEvents_ = nullptr;
        return r >= CLAP_PROCESS_CONTINUE && r <= CLAP_PROCESS_SLEEP ? Status::ok
                                                                     : Status::io_error;
    }

  public:
    explicit ClapBackend(std::shared_ptr<ClapModule> module, const PluginHostCallbacks& cb)
        : module_(std::move(module)), callbacks_(cb) {
        if (cb.clap)
            services_ = *cb.clap;
        track_ = services_.track;
        callbacks_.clap = nullptr;
        host_ = {CLAP_VERSION,
                 this,
                 services_.name.c_str(),
                 services_.vendor.c_str(),
                 services_.url.c_str(),
                 services_.version.c_str(),
                 &getExtension,
                 [](const clap_host_t* h) noexcept { self(h).request(pluginRestart); },
                 [](const clap_host_t* h) noexcept { self(h).request(pluginProcess); },
                 [](const clap_host_t* h) noexcept { self(h).request(pluginMainThread); }};
        in_ = {this,
               [](const clap_input_events_t* l) noexcept {
                   return static_cast<ClapBackend*>(l->ctx)->inputCount_;
               },
               [](const clap_input_events_t* l,
                  std::uint32_t i) noexcept -> const clap_event_header_t* {
                   auto& s = *static_cast<ClapBackend*>(l->ctx);
                   return i < s.inputCount_ ? &s.inputEvents_[i].note.header : nullptr;
               }};
        out_ = {this, [](const clap_output_events_t* l, const clap_event_header_t* e) noexcept {
                    auto& s = *static_cast<ClapBackend*>(l->ctx);
                    if (!s.outputEvents_)
                        return false;
                    if (decodeClapEvent(e, *s.outputEvents_, s.frames_) == Status::ok)
                        return true;
                    s.dropped_.add(); // Rejected to the plugin and reported to the host.
                    return false;
                }};
    }
    ~ClapBackend() override {
        if (plugin_) {
            closeEditor();
            if (active_)
                plugin_->deactivate(plugin_);
            plugin_->destroy(plugin_);
        }
    }
    ClapExtensions* clapExtensions() noexcept override {
        return this;
    }
    ClapCapabilities capabilities() const noexcept override {
        return capabilities_;
    }
    ClapProcessStatus lastProcessStatus() const noexcept override {
        return ClapProcessStatus(processStatus_.load(std::memory_order_acquire));
    }
    PluginSleepHint sleepHint() const noexcept override {
        switch (lastProcessStatus()) {
        case ClapProcessStatus::sleep:
            return PluginSleepHint::sleep;
        case ClapProcessStatus::tail:
            return requests_.load(std::memory_order_acquire) &
                           (pluginRestart | pluginLatencyChanged | pluginTailChanged)
                       ? PluginSleepHint::keepProcessing
                       : PluginSleepHint::afterTail;
        case ClapProcessStatus::continueIfNotQuiet:
            return PluginSleepHint::whenQuiet;
        default:
            return PluginSleepHint::keepProcessing;
        }
    }
    std::uint32_t wakeGeneration() const noexcept override {
        return wakeGeneration_.load(std::memory_order_acquire);
    }
    Status audioConfigurations(std::vector<ClapAudioConfiguration>&) noexcept override;
    Status selectAudioConfiguration(std::uint32_t) noexcept override;
    Status canConfigureAudioPorts(std::span<const ClapAudioPortRequest>, bool&) noexcept override;
    Status configureAudioPorts(std::span<const ClapAudioPortRequest>) noexcept override;
    Status setAudioPortActive(bool, std::uint32_t, bool, std::uint32_t) noexcept override;
    Status supportsSurround(std::uint64_t, bool&) noexcept override;
    Status supportsAmbisonic(std::uint32_t, std::uint32_t, bool&) noexcept override;
    Status voiceInfo(ClapVoiceInfo&) noexcept override;
    Status noteNames(std::vector<ClapNoteName>&) noexcept override;
    Status remoteControls(std::vector<ClapRemoteControlsPage>&) noexcept override;
    Status setTrackInfo(const ClapTrackInfo&) noexcept override;
    Status parameterFromText(std::uint32_t, std::string_view, double&) noexcept override;
    Status indicateMapping(std::uint32_t, bool, const ClapColor*, std::string_view,
                           std::string_view) noexcept override;
    Status indicateAutomation(std::uint32_t, ClapAutomationIndication,
                              const ClapColor*) noexcept override;
    Status saveState(ClapStateContext, std::vector<std::uint8_t>&, std::size_t) noexcept override;
    Status loadState(ClapStateContext, std::span<const std::uint8_t>) noexcept override;
    Status loadPreset(const ClapPresetLocation&) noexcept override;
    Status contextMenu(ClapMenuTarget, std::vector<ClapMenuItem>&) noexcept override;
    Status performContextMenu(ClapMenuTarget, std::uint32_t) noexcept override;
    Status editorHints(ClapGuiHints&) noexcept override;
    Status preferredEditorApi(PluginWindowApi&, bool&) noexcept override;
    Status suggestEditorTitle(std::string_view) noexcept override;
    Status initialize(std::string_view id) {
        if (services_.name.empty() || !validString(services_.name) ||
            !validString(services_.vendor) || !validString(services_.url) ||
            !validString(services_.version) || (track_ && !validTrack(*track_)))
            return Status::invalid_argument;
        if (id.empty() || id.size() > 65536 || id.find(char{}) != std::string_view::npos)
            return Status::invalid_argument;
        const std::string identifier(id);
        plugin_ = module_->factory->create_plugin(module_->factory, &host_, identifier.c_str());
        if (!plugin_)
            return Status::unsupported_format;
        if (!plugin_->destroy) {
            plugin_ = nullptr;
            return Status::unsupported_format;
        }
        if (!plugin_->init || !plugin_->activate || !plugin_->deactivate ||
            !plugin_->start_processing || !plugin_->stop_processing || !plugin_->reset ||
            !plugin_->process || !plugin_->get_extension || !plugin_->on_main_thread)
            return Status::unsupported_format;
        if (!plugin_->init(plugin_))
            return Status::io_error;
        audio_ = extension<clap_plugin_audio_ports_t>(CLAP_EXT_AUDIO_PORTS);
        notes_ = extension<clap_plugin_note_ports_t>(CLAP_EXT_NOTE_PORTS);
        params_ = extension<clap_plugin_params_t>(CLAP_EXT_PARAMS);
        state_ = extension<clap_plugin_state_t>(CLAP_EXT_STATE);
        gui_ = extension<clap_plugin_gui_t>(CLAP_EXT_GUI);
        latency_ = extension<clap_plugin_latency_t>(CLAP_EXT_LATENCY);
        tail_ = extension<clap_plugin_tail_t>(CLAP_EXT_TAIL);
        render_ = extension<clap_plugin_render_t>(CLAP_EXT_RENDER);
        timers_ = extension<clap_plugin_timer_support_t>(CLAP_EXT_TIMER_SUPPORT);
        fds_ = extension<clap_plugin_posix_fd_support_t>(CLAP_EXT_POSIX_FD_SUPPORT);
        configs_ = extension<clap_plugin_audio_ports_config_t>(CLAP_EXT_AUDIO_PORTS_CONFIG);
        configInfo_ = extension<clap_plugin_audio_ports_config_info_t>(
            CLAP_EXT_AUDIO_PORTS_CONFIG_INFO, CLAP_EXT_AUDIO_PORTS_CONFIG_INFO_COMPAT);
        configurable_ = extension<clap_plugin_configurable_audio_ports_t>(
            CLAP_EXT_CONFIGURABLE_AUDIO_PORTS, CLAP_EXT_CONFIGURABLE_AUDIO_PORTS_COMPAT);
        portActivation_ = extension<clap_plugin_audio_ports_activation_t>(
            CLAP_EXT_AUDIO_PORTS_ACTIVATION, CLAP_EXT_AUDIO_PORTS_ACTIVATION_COMPAT);
        surround_ = extension<clap_plugin_surround_t>(CLAP_EXT_SURROUND, CLAP_EXT_SURROUND_COMPAT);
        ambisonic_ =
            extension<clap_plugin_ambisonic_t>(CLAP_EXT_AMBISONIC, CLAP_EXT_AMBISONIC_COMPAT);
        voices_ = extension<clap_plugin_voice_info_t>(CLAP_EXT_VOICE_INFO);
        names_ = extension<clap_plugin_note_name_t>(CLAP_EXT_NOTE_NAME);
        remotes_ = extension<clap_plugin_remote_controls_t>(CLAP_EXT_REMOTE_CONTROLS,
                                                            CLAP_EXT_REMOTE_CONTROLS_COMPAT);
        trackInfo_ =
            extension<clap_plugin_track_info_t>(CLAP_EXT_TRACK_INFO, CLAP_EXT_TRACK_INFO_COMPAT);
        indication_ = extension<clap_plugin_param_indication_t>(CLAP_EXT_PARAM_INDICATION,
                                                                CLAP_EXT_PARAM_INDICATION_COMPAT);
        stateContext_ = extension<clap_plugin_state_context_t>(CLAP_EXT_STATE_CONTEXT);
        preset_ =
            extension<clap_plugin_preset_load_t>(CLAP_EXT_PRESET_LOAD, CLAP_EXT_PRESET_LOAD_COMPAT);
        menu_ = extension<clap_plugin_context_menu_t>(CLAP_EXT_CONTEXT_MENU,
                                                      CLAP_EXT_CONTEXT_MENU_COMPAT);
        pool_ = extension<clap_plugin_thread_pool_t>(CLAP_EXT_THREAD_POOL);
        capabilities_ = {configs_ && configs_->count && configs_->get && configs_->select,
                         configInfo_ && configInfo_->current_config && configInfo_->get,
                         configurable_ && configurable_->can_apply_configuration &&
                             configurable_->apply_configuration,
                         portActivation_ && portActivation_->set_active,
                         portActivation_ && portActivation_->can_activate_while_processing &&
                             portActivation_->can_activate_while_processing(plugin_),
                         surround_ && surround_->get_channel_map,
                         ambisonic_ && ambisonic_->get_config,
                         stateContext_ && stateContext_->save && stateContext_->load,
                         preset_ && preset_->from_location,
                         remotes_ && remotes_->count && remotes_->get,
                         names_ && names_->count && names_->get,
                         voices_ && voices_->get,
                         indication_ && indication_->set_mapping && indication_->set_automation,
                         menu_ && menu_->populate && menu_->perform,
                         render_ && render_->has_hard_realtime_requirement &&
                             render_->has_hard_realtime_requirement(plugin_)};
        inputEvents_.resize(1024);
        return refresh();
    }
    const PluginDescription& description() const noexcept override {
        return description_;
    }
    Status refresh() noexcept override {
        if (active_)
            return Status::transition_in_progress;
        descriptionValid_ = false;
        try {
            PluginDescription next;
            next.plugin = descriptor(plugin_->desc);
            next.state = state_ && state_->save && state_->load;
            next.editor = gui_ && gui_->create && gui_->destroy;
            next.float64 = true;
            std::size_t channels = 0;
            for (bool input : {true, false}) {
                auto& list = input ? next.inputs : next.outputs;
                const auto count =
                    audio_ && audio_->count && audio_->get ? audio_->count(plugin_, input) : 0;
                if (count > 64)
                    return Status::block_too_large;
                for (std::uint32_t i = 0; i < count; ++i) {
                    clap_audio_port_info_t port{};
                    if (!audio_->get(plugin_, i, input, &port) || !port.channel_count ||
                        port.channel_count > 256)
                        return Status::unsupported_format;
                    channels += port.channel_count;
                    list.push_back(portInfo(port, input, i));
                    for (std::size_t old = 0; old + 1 < list.size(); ++old)
                        if (list[old].id == port.id)
                            return Status::unsupported_format;
                    next.float64 = next.float64 && list.back().supports64;
                }
                auto& noteList = input ? next.noteInputs : next.noteOutputs;
                const auto noteCount =
                    notes_ && notes_->count && notes_->get ? notes_->count(plugin_, input) : 0;
                if (noteCount > 64)
                    return Status::block_too_large;
                for (std::uint32_t i = 0; i < noteCount; ++i) {
                    clap_note_port_info_t port{};
                    if (!notes_->get(plugin_, i, input, &port) || port.id == CLAP_INVALID_ID)
                        return Status::unsupported_format;
                    for (const auto& existing : noteList)
                        if (existing.id == port.id)
                            return Status::unsupported_format;
                    noteList.push_back({port.id, port.supported_dialects, port.preferred_dialect,
                                        textFixed(port.name)});
                }
            }
            std::vector<ParameterCookie> cookies;
            const auto count =
                params_ && params_->count && params_->get_info ? params_->count(plugin_) : 0;
            if (count > 65536)
                return Status::block_too_large;
            for (std::uint32_t i = 0; i < count; ++i) {
                clap_param_info_t p{};
                if (!params_->get_info(plugin_, i, &p) || p.id == CLAP_INVALID_ID ||
                    !isFinite(p.min_value) || !isFinite(p.max_value) ||
                    !isFinite(p.default_value) || p.min_value > p.max_value ||
                    p.default_value < p.min_value || p.default_value > p.max_value)
                    return Status::unsupported_format;
                next.parameters.push_back({p.id, textFixed(p.name), textFixed(p.module),
                                           p.min_value, p.max_value, p.default_value,
                                           (p.flags & CLAP_PARAM_IS_STEPPED) != 0,
                                           (p.flags & CLAP_PARAM_IS_READONLY) != 0,
                                           (p.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0,
                                           (p.flags & CLAP_PARAM_IS_MODULATABLE) != 0,
                                           (p.flags & CLAP_PARAM_IS_HIDDEN) != 0});
                auto& parameter = next.parameters.back();
                parameter.periodic = (p.flags & CLAP_PARAM_IS_PERIODIC) != 0;
                parameter.bypass = (p.flags & CLAP_PARAM_IS_BYPASS) != 0;
                parameter.requiresProcess = (p.flags & CLAP_PARAM_REQUIRES_PROCESS) != 0;
                parameter.enumeration = (p.flags & CLAP_PARAM_IS_ENUM) != 0;
                parameter.automationTargets =
                    ((p.flags & CLAP_PARAM_IS_AUTOMATABLE_PER_NOTE_ID) ? parameterNoteId : 0u) |
                    ((p.flags & CLAP_PARAM_IS_AUTOMATABLE_PER_PORT) ? parameterPort : 0u) |
                    ((p.flags & CLAP_PARAM_IS_AUTOMATABLE_PER_CHANNEL) ? parameterChannel : 0u) |
                    ((p.flags & CLAP_PARAM_IS_AUTOMATABLE_PER_KEY) ? parameterKey : 0u);
                parameter.modulationTargets =
                    ((p.flags & CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID) ? parameterNoteId : 0u) |
                    ((p.flags & CLAP_PARAM_IS_MODULATABLE_PER_PORT) ? parameterPort : 0u) |
                    ((p.flags & CLAP_PARAM_IS_MODULATABLE_PER_CHANNEL) ? parameterChannel : 0u) |
                    ((p.flags & CLAP_PARAM_IS_MODULATABLE_PER_KEY) ? parameterKey : 0u);
                cookies.push_back({p.id, p.cookie});
            }
            std::sort(next.parameters.begin(), next.parameters.end(),
                      [](const auto& a, const auto& b) { return a.id < b.id; });
            std::sort(cookies.begin(), cookies.end(), [](auto a, auto b) { return a.id < b.id; });
            for (std::size_t i = 1; i < cookies.size(); ++i)
                if (cookies[i - 1].id == cookies[i].id)
                    return Status::unsupported_format;
            std::vector<clap_audio_buffer_t> ins(next.inputs.size()), outs(next.outputs.size());
            std::vector<float*> p32(channels);
            std::vector<double*> p64(channels);
            std::vector<std::uint8_t> nextInputs(next.inputs.size(), 1),
                nextOutputs(next.outputs.size(), 1);
            if (!portStatesInvalid_)
                for (bool input : {true, false}) {
                    const auto& oldPorts = input ? description_.inputs : description_.outputs;
                    const auto& newPorts = input ? next.inputs : next.outputs;
                    const auto& oldStates = input ? inputActive_ : outputActive_;
                    auto& newStates = input ? nextInputs : nextOutputs;
                    for (std::size_t i = 0; i < newPorts.size(); ++i)
                        for (std::size_t j = 0; j < oldPorts.size(); ++j)
                            if (newPorts[i].id == oldPorts[j].id && j < oldStates.size())
                                newStates[i] = oldStates[j];
                }
            description_ = std::move(next);
            cookies_.swap(cookies);
            inputs_.swap(ins);
            outputs_.swap(outs);
            pointers32_.swap(p32);
            pointers64_.swap(p64);
            inputActive_.swap(nextInputs);
            outputActive_.swap(nextOutputs);
            portStatesInvalid_ = false;
            cookiesInvalid_.store(0, std::memory_order_release);
            descriptionValid_ = true;
            return Status::ok;
        } catch (const std::bad_alloc&) {
            return Status::allocation_failed;
        } catch (...) {
            return Status::unsupported_format;
        }
    }
    Status activate(const PluginPrepareOptions& o) noexcept override {
        if (!descriptionValid_)
            return Status::not_prepared;
        try {
            inputEvents_.resize(o.maximumEvents);
        } catch (...) {
            return Status::allocation_failed;
        }
        const auto silence = o.useFloat64 ? silent64_.resize(1, o.maximumFrames)
                                          : silent32_.resize(1, o.maximumFrames);
        if (silence != Status::ok)
            return silence;
        if (o.useFloat64)
            std::fill_n(silent64_.block().channel(0).data(), o.maximumFrames, 0.0);
        else
            std::fill_n(silent32_.block().channel(0).data(), o.maximumFrames, 0.0f);
        capabilities_.hardRealtime = render_ && render_->has_hard_realtime_requirement &&
                                     render_->has_hard_realtime_requirement(plugin_);
        if (o.offline && capabilities_.hardRealtime)
            return Status::unsupported_format;
        if (render_ && render_->set &&
            !render_->set(plugin_, o.offline ? CLAP_RENDER_OFFLINE : CLAP_RENDER_REALTIME))
            return Status::unsupported_format;
        active_ = plugin_->activate(plugin_, o.sampleRate, o.minimumFrames, o.maximumFrames);
        return active_ ? Status::ok : Status::invalid_spec;
    }
    Status deactivate() noexcept override {
        if (active_)
            plugin_->deactivate(plugin_);
        active_ = false;
        return Status::ok;
    }
    Status start() noexcept override {
        const auto started = plugin_->start_processing(plugin_);
        return started ? Status::ok : Status::io_error;
    }
    void stop() noexcept override {
        plugin_->stop_processing(plugin_);
    }
    Status reset() noexcept override {
        plugin_->reset(plugin_);
        return Status::ok;
    }
    Status process(const PluginProcess<float>& p) noexcept override {
        return processing(p);
    }
    Status process(const PluginProcess<double>& p) noexcept override {
        return processing(p);
    }
    PluginTiming timing() const noexcept override {
        PluginTiming t;
        t.latencyKnown = active_ && latency_ && latency_->get;
        t.tailKnown = tail_ && tail_->get;
        if (t.latencyKnown)
            t.latencyFrames = latency_->get(plugin_);
        if (t.tailKnown) {
            t.tailFrames = tail_->get(plugin_);
            t.infiniteTail = t.tailFrames >= 0x7fffffffu;
        }
        return t;
    }
    Status parameterValue(std::uint32_t id, double& value) noexcept override {
        if (!params_ || !params_->get_value)
            return Status::unsupported_format;
        double v = 0;
        if (!params_->get_value(plugin_, id, &v) || !isFinite(v))
            return Status::invalid_argument;
        value = v;
        return Status::ok;
    }
    Status parameterText(std::uint32_t id, double v, std::string& out) noexcept override {
        if (!params_ || !params_->value_to_text)
            return Status::unsupported_format;
        std::array<char, 4096> buffer{};
        if (!params_->value_to_text(plugin_, id, v, buffer.data(), std::uint32_t(buffer.size())))
            return Status::invalid_argument;
        try {
            const auto end = std::find(buffer.begin(), buffer.end(), char{});
            if (end == buffer.end())
                return Status::invalid_argument;
            out.assign(buffer.data(), std::size_t(end - buffer.begin()));
            return Status::ok;
        } catch (...) {
            return Status::allocation_failed;
        }
    }
    Status flush(const PluginEventBuffer& in, PluginEventBuffer& out) noexcept override {
        if (!params_ || !params_->flush)
            return Status::unsupported_format;
        for (std::size_t i = 0; i < in.size(); ++i) {
            const auto kind = in.event(i)->kind;
            if (kind != PluginEventKind::parameterValue &&
                kind != PluginEventKind::parameterModulation &&
                kind != PluginEventKind::gestureBegin && kind != PluginEventKind::gestureEnd)
                return Status::invalid_argument;
        }
        const auto r = convert(&in);
        if (r != Status::ok)
            return r;
        outputEvents_ = &out;
        frames_ = std::numeric_limits<std::uint32_t>::max();
        params_->flush(plugin_, &in_, &out_);
        outputEvents_ = nullptr;
        return Status::ok;
    }
    Status saveState(std::vector<std::uint8_t>& result, std::size_t limit) noexcept override {
        if (!state_ || !state_->save)
            return Status::unsupported_format;
        struct Output {
            std::vector<std::uint8_t> bytes;
            std::size_t limit;
            bool failed = false;
        } output{{}, limit};
        clap_ostream_t stream{&output,
                              [](const clap_ostream_t* s, const void* data,
                                 std::uint64_t size) noexcept -> std::int64_t {
                                  auto& o = *static_cast<Output*>(s->ctx);
                                  if (!size)
                                      return 0;
                                  if (!data || size > o.limit - o.bytes.size()) {
                                      o.failed = true;
                                      return -1;
                                  }
                                  try {
                                      const auto* p = static_cast<const std::uint8_t*>(data);
                                      o.bytes.insert(o.bytes.end(), p, p + std::size_t(size));
                                      return std::int64_t(size);
                                  } catch (...) {
                                      o.failed = true;
                                      return -1;
                                  }
                              }};
        const auto saved = selectedStateContext_ && stateContext_ && stateContext_->save
                               ? stateContext_->save(plugin_, &stream, selectedStateContext_)
                               : state_->save(plugin_, &stream);
        if (!saved || output.failed)
            return Status::io_error;
        result.swap(output.bytes);
        return Status::ok;
    }
    Status loadState(std::span<const std::uint8_t> data) noexcept override {
        if (!state_ || !state_->load)
            return Status::unsupported_format;
        struct Input {
            std::span<const std::uint8_t> bytes;
            std::size_t at = 0;
        } input{data};
        clap_istream_t stream{
            &input,
            [](const clap_istream_t* s, void* target, std::uint64_t size) noexcept -> std::int64_t {
                auto& in = *static_cast<Input*>(s->ctx);
                if (!size)
                    return 0;
                if (!target)
                    return -1;
                const auto n = std::size_t(std::min<std::uint64_t>(size, in.bytes.size() - in.at));
                if (n)
                    std::memcpy(target, in.bytes.data() + in.at, n);
                in.at += n;
                return std::int64_t(n);
            }};
        const auto loaded = selectedStateContext_ && stateContext_ && stateContext_->load
                                ? stateContext_->load(plugin_, &stream, selectedStateContext_)
                                : state_->load(plugin_, &stream);
        // Even a failed foreign restore may have mutated its ports/parameters.
        // Refresh on control before another activation can use the description.
        const auto refreshed = refresh();
        request(pluginPortsChanged | pluginParametersChanged);
        return loaded ? refreshed : Status::io_error;
    }
    Status openEditor(const PluginWindow& window, PluginEditorSize& size) noexcept override {
        if (!gui_ || !gui_->is_api_supported || !gui_->create || !gui_->destroy ||
            !gui_->get_size || !gui_->show || !gui_->hide)
            return Status::unsupported_format;
        const char* api = nullptr;
        switch (window.api) {
        case PluginWindowApi::win32:
            api = CLAP_WINDOW_API_WIN32;
            break;
        case PluginWindowApi::cocoa:
            api = CLAP_WINDOW_API_COCOA;
            break;
        case PluginWindowApi::x11:
            api = CLAP_WINDOW_API_X11;
            break;
        case PluginWindowApi::wayland:
            api = CLAP_WINDOW_API_WAYLAND;
            break;
        case PluginWindowApi::unspecified:
            if (!window.floating || window.handle)
                return Status::invalid_argument;
            break;
        }
        if ((!api && !window.floating) ||
            (!window.floating && window.api == PluginWindowApi::wayland) ||
            !gui_->is_api_supported(plugin_, api, window.floating))
            return Status::unsupported_backend;
        if (!gui_->create(plugin_, api, window.floating))
            return Status::io_error;
        editor_ = true;
        floating_ = window.floating;
        cocoa_ = window.api == PluginWindowApi::cocoa || window.api == PluginWindowApi::unspecified;
        if (!cocoa_ && api && gui_->set_scale)
            (void)gui_->set_scale(plugin_, window.scale);
        clap_window_t parent{};
        parent.api = api;
        if (window.api == PluginWindowApi::x11)
            parent.x11 = static_cast<unsigned long>(window.handle);
        else
            parent.ptr = reinterpret_cast<void*>(window.handle);
        if (!window.floating && (!gui_->set_parent || !gui_->set_parent(plugin_, &parent))) {
            closeEditor();
            return Status::io_error;
        }
        if (window.floating && window.handle && gui_->set_transient &&
            !gui_->set_transient(plugin_, &parent)) {
            closeEditor();
            return Status::io_error;
        }
        PluginEditorSize actual;
        if (!gui_->get_size(plugin_, &actual.width, &actual.height) || !actual.width ||
            !actual.height || actual.width > 32768 || actual.height > 32768 ||
            !gui_->show(plugin_)) {
            closeEditor();
            return Status::io_error;
        }
        size = actual;
        return Status::ok;
    }
    Status resizeEditor(PluginEditorSize& size) noexcept override {
        if (!editor_ || floating_ || !gui_->can_resize || !gui_->can_resize(plugin_) ||
            !gui_->adjust_size || !gui_->set_size)
            return Status::unsupported_format;
        auto adjusted = size;
        if (!gui_->adjust_size(plugin_, &adjusted.width, &adjusted.height) || !adjusted.width ||
            !adjusted.height || adjusted.width > 32768 || adjusted.height > 32768 ||
            !gui_->set_size(plugin_, adjusted.width, adjusted.height))
            return Status::io_error;
        size = adjusted;
        return Status::ok;
    }
    Status scaleEditor(double scale) noexcept override {
        if (cocoa_)
            return Status::unsupported_format;
        return gui_->set_scale && gui_->set_scale(plugin_, scale) ? Status::ok
                                                                  : Status::unsupported_format;
    }
    Status showEditor(bool show) noexcept override {
        if (guiWasDestroyed_.load(std::memory_order_acquire))
            return Status::not_prepared;
        return (show ? gui_->show(plugin_) : gui_->hide(plugin_)) ? Status::ok : Status::io_error;
    }
    void closeEditor() noexcept override {
        const bool destroyed = guiWasDestroyed_.exchange(0, std::memory_order_acq_rel) != 0;
        if (editor_) {
            if (!destroyed)
                (void)gui_->hide(plugin_);
            gui_->destroy(plugin_);
            editor_ = false;
        }
    }
    std::uint32_t pumpMainThread() noexcept override {
        auto flags = requests_.exchange(0, std::memory_order_acq_rel) | dropped_.take();
        if (logsDropped_.exchange(0, std::memory_order_acq_rel))
            flags |= pluginLogsDropped;
        for (auto& slot : logs_) {
            std::uint32_t expected = 2;
            if (slot.state.compare_exchange_strong(expected, 3, std::memory_order_acquire)) {
                if (services_.log)
                    services_.log(services_.context, slot.severity, slot.message.data());
                slot.state.store(0, std::memory_order_release);
            }
        }
        if (flags & pluginMainThread)
            plugin_->on_main_thread(plugin_);
        if (flags & pluginEditorClosed)
            closeEditor();
        else if (editor_) {
            if (flags & pluginEditorShow)
                (void)gui_->show(plugin_);
            if (flags & pluginEditorHide)
                (void)gui_->hide(plugin_);
        }
        const auto now = milliseconds();
        if (timers_ && timers_->on_timer)
            for (auto& t : timerSlots_)
                if (t.id && now >= t.next) {
                    const auto id = t.id;
                    t.next = now + t.period;
                    timers_->on_timer(plugin_, id);
                }
#if !defined(_WIN32)
        if (fds_ && fds_->on_fd) {
            std::array<pollfd, 64> fds{};
            std::size_t count = 0;
            for (const auto& fd : fdSlots_)
                if (fd.fd >= 0)
                    fds[count++] = {fd.fd,
                                    short(((fd.flags & CLAP_POSIX_FD_READ) ? POLLIN : 0) |
                                          ((fd.flags & CLAP_POSIX_FD_WRITE) ? POLLOUT : 0)),
                                    0};
            if (::poll(fds.data(), static_cast<nfds_t>(count), 0) > 0)
                for (std::size_t i = 0; i < count; ++i)
                    if (fds[i].revents) {
                        std::uint32_t ready = 0;
                        if (fds[i].revents & POLLIN)
                            ready |= CLAP_POSIX_FD_READ;
                        if (fds[i].revents & POLLOUT)
                            ready |= CLAP_POSIX_FD_WRITE;
                        if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL))
                            ready |= CLAP_POSIX_FD_ERROR;
                        bool registered = false;
                        for (const auto& fd : fdSlots_)
                            if (fd.fd == fds[i].fd)
                                registered = true;
                        if (registered)
                            fds_->on_fd(plugin_, fds[i].fd, ready);
                    }
        }
#endif
        // Requests raised by callbacks/timers remain pending for the next pump.
        if (callbacks_.requests && flags)
            callbacks_.requests(callbacks_.context, flags);
        return flags;
    }
};
bool validString(std::string_view value, std::size_t maximum) noexcept {
    return value.size() <= maximum && value.find(char{}) == std::string_view::npos;
}
bool validTrack(const ClapTrackInfo& track) noexcept {
    return (!track.name || validString(*track.name, CLAP_NAME_SIZE - 1)) && track.channels <= 256 &&
           validString(track.portType, 255) && (track.channels || track.portType.empty());
}
bool validLocation(const ClapPresetLocation& location) {
    if (!validString(location.location) || !validString(location.loadKey))
        return false;
    if (location.kind == ClapPresetLocationKind::plugin)
        return location.location.empty();
    return location.kind == ClapPresetLocationKind::file && !location.location.empty() &&
           std::filesystem::path(
               std::u8string_view(reinterpret_cast<const char8_t*>(location.location.data()),
                                  location.location.size()))
               .is_absolute();
}
clap_color_t nativeColor(const ClapColor& c) noexcept {
    return {c.alpha, c.red, c.green, c.blue};
}
clap_context_menu_target_t nativeTarget(ClapMenuTarget t) noexcept {
    return {t.parameter ? std::uint32_t(CLAP_CONTEXT_MENU_TARGET_KIND_PARAM)
                        : std::uint32_t(CLAP_CONTEXT_MENU_TARGET_KIND_GLOBAL),
            t.id};
}
bool menuTarget(const clap_context_menu_target_t* t, ClapMenuTarget& out) noexcept {
    if (t && t->kind > CLAP_CONTEXT_MENU_TARGET_KIND_PARAM)
        return false;
    out = {t && t->kind == CLAP_CONTEXT_MENU_TARGET_KIND_PARAM, t ? t->id : CLAP_INVALID_ID};
    return !out.parameter || out.id != CLAP_INVALID_ID;
}
struct MenuReceiver {
    std::vector<ClapMenuItem> items;
    unsigned depth = 0;
    bool failed = false;
    bool add(std::uint32_t kind, const void* data) noexcept {
        try {
            if (failed || kind > CLAP_CONTEXT_MENU_ITEM_TITLE || items.size() >= 4096) {
                failed = true;
                return false;
            }
            ClapMenuItem item;
            item.kind = ClapMenuItemKind(kind);
            switch (kind) {
            case CLAP_CONTEXT_MENU_ITEM_ENTRY: {
                if (!data) {
                    failed = true;
                    return false;
                }
                const auto& e = *static_cast<const clap_context_menu_entry_t*>(data);
                item.label = text(e.label);
                item.enabled = e.is_enabled;
                item.action = e.action_id;
                break;
            }
            case CLAP_CONTEXT_MENU_ITEM_CHECK_ENTRY: {
                if (!data) {
                    failed = true;
                    return false;
                }
                const auto& e = *static_cast<const clap_context_menu_check_entry_t*>(data);
                item.label = text(e.label);
                item.enabled = e.is_enabled;
                item.action = e.action_id;
                item.checked = e.is_checked;
                break;
            }
            case CLAP_CONTEXT_MENU_ITEM_BEGIN_SUBMENU: {
                if (!data || depth == 32) {
                    failed = true;
                    return false;
                }
                const auto& e = *static_cast<const clap_context_menu_submenu_t*>(data);
                item.label = text(e.label);
                item.enabled = e.is_enabled;
                ++depth;
                break;
            }
            case CLAP_CONTEXT_MENU_ITEM_END_SUBMENU:
                if (!depth) {
                    failed = true;
                    return false;
                }
                --depth;
                break;
            case CLAP_CONTEXT_MENU_ITEM_TITLE: {
                if (!data) {
                    failed = true;
                    return false;
                }
                const auto& e = *static_cast<const clap_context_menu_item_title_t*>(data);
                item.label = text(e.title);
                item.enabled = e.is_enabled;
                break;
            }
            default:
                break;
            }
            items.push_back(std::move(item));
            return true;
        } catch (...) {
            failed = true;
            return false;
        }
    }
    clap_context_menu_builder_t builder() noexcept {
        return {this,
                [](const clap_context_menu_builder_t* b, auto k, const void* d) noexcept {
                    return static_cast<MenuReceiver*>(b->ctx)->add(k, d);
                },
                [](const clap_context_menu_builder_t*, auto k) noexcept {
                    return k <= CLAP_CONTEXT_MENU_ITEM_TITLE;
                }};
    }
};
bool sendMenu(const std::vector<ClapMenuItem>& items,
              const clap_context_menu_builder_t* b) noexcept {
    if (!b || !b->supports || !b->add_item || items.size() > 4096)
        return false;
    // Validate the whole menu before forwarding anything to a foreign builder.
    unsigned depth = 0;
    for (const auto& i : items) {
        if (std::uint32_t(i.kind) > CLAP_CONTEXT_MENU_ITEM_TITLE || !validString(i.label) ||
            !b->supports(b, std::uint32_t(i.kind)))
            return false;
        if (i.kind == ClapMenuItemKind::beginSubmenu && ++depth > 32)
            return false;
        if (i.kind == ClapMenuItemKind::endSubmenu) {
            if (!depth)
                return false;
            --depth;
        }
    }
    if (depth)
        return false;
    for (const auto& i : items) {
        const clap_context_menu_entry_t entry{i.label.c_str(), i.enabled, i.action};
        const clap_context_menu_check_entry_t check{i.label.c_str(), i.enabled, i.checked,
                                                    i.action};
        const clap_context_menu_submenu_t submenu{i.label.c_str(), i.enabled};
        const clap_context_menu_item_title_t title{i.label.c_str(), i.enabled};
        const void* data = nullptr;
        switch (i.kind) {
        case ClapMenuItemKind::entry:
            data = &entry;
            break;
        case ClapMenuItemKind::checkEntry:
            data = &check;
            break;
        case ClapMenuItemKind::beginSubmenu:
            data = &submenu;
            break;
        case ClapMenuItemKind::title:
            data = &title;
            break;
        default:
            break;
        }
        if (!b->add_item(b, std::uint32_t(i.kind), data))
            return false;
    }
    return true;
}
struct PortRequests {
    std::vector<clap_audio_port_configuration_request_t> native;
    std::vector<clap_ambisonic_config_t> ambisonics;
    Status prepare(std::span<const ClapAudioPortRequest> requests, const PluginDescription& desc) {
        if (requests.empty() || requests.size() > 128)
            return Status::invalid_argument;
        native.resize(requests.size());
        ambisonics.resize(requests.size());
        for (std::size_t i = 0; i < requests.size(); ++i) {
            const auto& r = requests[i];
            const auto& ports = r.input ? desc.inputs : desc.outputs;
            if (r.index >= ports.size() || !r.channels || r.channels > 256 ||
                !validString(r.type, 255))
                return Status::invalid_argument;
            for (std::size_t j = 0; j < i; ++j)
                if (requests[j].input == r.input && requests[j].index == r.index)
                    return Status::invalid_argument;
            const void* details = nullptr;
            if (r.type == CLAP_PORT_MONO && r.channels != 1)
                return Status::invalid_argument;
            if (r.type == CLAP_PORT_STEREO && r.channels != 2)
                return Status::invalid_argument;
            if (r.type == CLAP_PORT_SURROUND) {
                if (r.surroundMap.size() != r.channels ||
                    std::any_of(r.surroundMap.begin(), r.surroundMap.end(),
                                [](auto ch) { return ch > CLAP_SURROUND_TSR; }))
                    return Status::invalid_argument;
                details = r.surroundMap.data();
            } else if (r.type == CLAP_PORT_AMBISONIC) {
                if (r.ambisonicOrdering > CLAP_AMBISONIC_ORDERING_ACN ||
                    r.ambisonicNormalization > CLAP_AMBISONIC_NORMALIZATION_N2D)
                    return Status::invalid_argument;
                ambisonics[i] = {r.ambisonicOrdering, r.ambisonicNormalization};
                details = &ambisonics[i];
            } else if (!r.type.empty() && r.type != CLAP_PORT_MONO && r.type != CLAP_PORT_STEREO)
                return Status::unsupported_format; // Unknown port_details has no portable schema.
            native[i] = {r.input, r.index, r.channels, r.type.empty() ? nullptr : r.type.c_str(),
                         details};
        }
        return Status::ok;
    }
};
Status ClapBackend::audioConfigurations(std::vector<ClapAudioConfiguration>& result) noexcept {
    if (const auto s = control(true); s != Status::ok)
        return s;
    if (!capabilities_.audioConfigurations)
        return Status::unsupported_format;
    try {
        const auto count = configs_->count(plugin_);
        if (count > 4096)
            return Status::block_too_large;
        std::vector<ClapAudioConfiguration> next;
        const auto current = capabilities_.configurationDetails
                                 ? configInfo_->current_config(plugin_)
                                 : CLAP_INVALID_ID;
        for (std::uint32_t i = 0; i < count; ++i) {
            clap_audio_ports_config_t c{};
            if (!configs_->get(plugin_, i, &c) || c.id == CLAP_INVALID_ID ||
                c.input_port_count > 64 || c.output_port_count > 64 ||
                (c.has_main_input &&
                 (!c.main_input_channel_count || c.main_input_channel_count > 256)) ||
                (c.has_main_output &&
                 (!c.main_output_channel_count || c.main_output_channel_count > 256)))
                return Status::unsupported_format;
            for (const auto& other : next)
                if (other.id == c.id)
                    return Status::unsupported_format;
            ClapAudioConfiguration item;
            item.id = c.id;
            item.name = textFixed(c.name);
            item.current = c.id == current;
            item.inputCount = c.input_port_count;
            item.outputCount = c.output_port_count;
            if (c.has_main_input) {
                item.mainInputChannels = c.main_input_channel_count;
                item.mainInputType = text(c.main_input_port_type);
            }
            if (c.has_main_output) {
                item.mainOutputChannels = c.main_output_channel_count;
                item.mainOutputType = text(c.main_output_port_type);
            }
            item.detailedPortsKnown = capabilities_.configurationDetails;
            if (item.detailedPortsKnown)
                for (bool input : {true, false}) {
                    auto& ports = input ? item.inputs : item.outputs;
                    for (std::uint32_t j = 0; j < (input ? item.inputCount : item.outputCount);
                         ++j) {
                        clap_audio_port_info_t p{};
                        if (!configInfo_->get(plugin_, c.id, j, input, &p))
                            return Status::unsupported_format;
                        ports.push_back(portInfo(p, input, j, false));
                    }
                }
            next.push_back(std::move(item));
        }
        result.swap(next);
        return Status::ok;
    } catch (const std::bad_alloc&) {
        return Status::allocation_failed;
    } catch (...) {
        return Status::unsupported_format;
    }
}
Status ClapBackend::selectAudioConfiguration(std::uint32_t id) noexcept {
    if (const auto s = control(true); s != Status::ok)
        return s;
    if (!capabilities_.audioConfigurations)
        return Status::unsupported_format;
    if (id == CLAP_INVALID_ID || !configs_->select(plugin_, id))
        return Status::invalid_argument;
    portStatesInvalid_ = true;
    request(pluginPortsChanged);
    return refresh();
}
Status ClapBackend::canConfigureAudioPorts(std::span<const ClapAudioPortRequest> requests,
                                           bool& supported) noexcept {
    if (const auto s = control(true); s != Status::ok)
        return s;
    if (!capabilities_.configurablePorts)
        return Status::unsupported_format;
    try {
        PortRequests storage;
        const auto s = storage.prepare(requests, description_);
        if (s != Status::ok)
            return s;
        supported = configurable_->can_apply_configuration(plugin_, storage.native.data(),
                                                           std::uint32_t(storage.native.size()));
        return Status::ok;
    } catch (...) {
        return Status::allocation_failed;
    }
}
Status ClapBackend::configureAudioPorts(std::span<const ClapAudioPortRequest> requests) noexcept {
    if (const auto s = control(true); s != Status::ok)
        return s;
    if (!capabilities_.configurablePorts)
        return Status::unsupported_format;
    try {
        PortRequests storage;
        const auto s = storage.prepare(requests, description_);
        if (s != Status::ok)
            return s;
        if (!configurable_->apply_configuration(plugin_, storage.native.data(),
                                                std::uint32_t(storage.native.size())))
            return Status::unsupported_format;
        request(pluginPortsChanged);
        return refresh();
    } catch (...) {
        return Status::allocation_failed;
    }
}
Status ClapBackend::setAudioPortActive(bool input, std::uint32_t index, bool enabled,
                                       std::uint32_t bits) noexcept {
    if (!capabilities_.portActivation)
        return Status::unsupported_format;
    if (bits && bits != 32 && bits != 64)
        return Status::invalid_argument;
    if (active_) {
        if (!capabilities_.livePortActivation)
            return Status::transition_in_progress;
        if (inProcess_)
            return Status::transition_in_progress;
    } else if (!onMain())
        return Status::invalid_argument;
    auto& ports = input ? inputActive_ : outputActive_;
    if (index >= ports.size())
        return Status::invalid_argument;
    if (active_) {
        PluginAudioScope audio(this);
        if (!portActivation_->set_active(plugin_, input, index, enabled, bits))
            return Status::invalid_argument;
    } else if (!portActivation_->set_active(plugin_, input, index, enabled, bits))
        return Status::invalid_argument;
    ports[index] = enabled ? 1 : 0;
    return Status::ok;
}
Status ClapBackend::supportsSurround(std::uint64_t mask, bool& result) noexcept {
    if (const auto s = control(); s != Status::ok)
        return s;
    if (!surround_ || !surround_->is_channel_mask_supported)
        return Status::unsupported_format;
    if (!mask || (mask >> 20))
        return Status::invalid_argument;
    result = surround_->is_channel_mask_supported(plugin_, mask);
    return Status::ok;
}
Status ClapBackend::supportsAmbisonic(std::uint32_t ordering, std::uint32_t normalization,
                                      bool& result) noexcept {
    if (const auto s = control(); s != Status::ok)
        return s;
    if (!ambisonic_ || !ambisonic_->is_config_supported)
        return Status::unsupported_format;
    if (ordering > CLAP_AMBISONIC_ORDERING_ACN || normalization > CLAP_AMBISONIC_NORMALIZATION_N2D)
        return Status::invalid_argument;
    const clap_ambisonic_config_t config{ordering, normalization};
    result = ambisonic_->is_config_supported(plugin_, &config);
    return Status::ok;
}
Status ClapBackend::voiceInfo(ClapVoiceInfo& result) noexcept {
    if (const auto s = control(); s != Status::ok)
        return s;
    if (!capabilities_.voiceInfo)
        return Status::unsupported_format;
    if (!active_)
        return Status::not_prepared;
    clap_voice_info_t v{};
    if (!voices_->get(plugin_, &v))
        return Status::io_error;
    if (!v.voice_count || v.voice_count > v.voice_capacity)
        return Status::invalid_spec;
    result = {v.voice_count, v.voice_capacity,
              (v.flags & CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES) != 0};
    return Status::ok;
}
Status ClapBackend::noteNames(std::vector<ClapNoteName>& result) noexcept {
    if (const auto s = control(); s != Status::ok)
        return s;
    if (!capabilities_.noteNames)
        return Status::unsupported_format;
    try {
        const auto count = names_->count(plugin_);
        if (count > 65536)
            return Status::block_too_large;
        std::vector<ClapNoteName> next;
        for (std::uint32_t i = 0; i < count; ++i) {
            clap_note_name_t n{};
            if (!names_->get(plugin_, i, &n) || n.port < -1 || n.key < -1 || n.key > 127 ||
                n.channel < -1 || n.channel > 15)
                return Status::unsupported_format;
            next.push_back({textFixed(n.name), n.port, n.key, n.channel});
        }
        result.swap(next);
        return Status::ok;
    } catch (...) {
        return Status::allocation_failed;
    }
}
Status ClapBackend::remoteControls(std::vector<ClapRemoteControlsPage>& result) noexcept {
    if (const auto s = control(); s != Status::ok)
        return s;
    if (!capabilities_.remoteControls)
        return Status::unsupported_format;
    try {
        const auto count = remotes_->count(plugin_);
        if (count > 4096)
            return Status::block_too_large;
        std::vector<ClapRemoteControlsPage> next;
        for (std::uint32_t i = 0; i < count; ++i) {
            clap_remote_controls_page_t p{};
            if (!remotes_->get(plugin_, i, &p) || p.page_id == CLAP_INVALID_ID)
                return Status::unsupported_format;
            for (const auto& other : next)
                if (other.id == p.page_id)
                    return Status::unsupported_format;
            ClapRemoteControlsPage page;
            page.id = p.page_id;
            page.section = textFixed(p.section_name);
            page.name = textFixed(p.page_name);
            page.presetSpecific = p.is_for_preset;
            std::copy(std::begin(p.param_ids), std::end(p.param_ids), page.parameters.begin());
            next.push_back(std::move(page));
        }
        result.swap(next);
        return Status::ok;
    } catch (...) {
        return Status::allocation_failed;
    }
}
Status ClapBackend::setTrackInfo(const ClapTrackInfo& info) noexcept {
    if (const auto s = control(); s != Status::ok)
        return s;
    if (!validTrack(info))
        return Status::invalid_argument;
    try {
        track_ = info;
    } catch (...) {
        return Status::allocation_failed;
    }
    if (trackInfo_ && trackInfo_->changed)
        trackInfo_->changed(plugin_);
    return Status::ok;
}
Status ClapBackend::parameterFromText(std::uint32_t id, std::string_view value,
                                      double& result) noexcept {
    if (const auto s = control(); s != Status::ok)
        return s;
    if (!params_ || !params_->text_to_value)
        return Status::unsupported_format;
    if (!validString(value))
        return Status::invalid_argument;
    try {
        const std::string terminated(value);
        double v = 0;
        if (!params_->text_to_value(plugin_, id, terminated.c_str(), &v) || !isFinite(v))
            return Status::invalid_argument;
        const auto it =
            std::lower_bound(description_.parameters.begin(), description_.parameters.end(), id,
                             [](const auto& p, auto key) { return p.id < key; });
        if (it == description_.parameters.end() || it->id != id || v < it->minimum ||
            v > it->maximum)
            return Status::invalid_argument;
        result = v;
        return Status::ok;
    } catch (...) {
        return Status::allocation_failed;
    }
}
Status ClapBackend::indicateMapping(std::uint32_t id, bool mapped, const ClapColor* color,
                                    std::string_view label, std::string_view description) noexcept {
    if (const auto s = control(); s != Status::ok)
        return s;
    if (!capabilities_.parameterIndication)
        return Status::unsupported_format;
    if (!validString(label) || !validString(description) || id == CLAP_INVALID_ID)
        return Status::invalid_argument;
    try {
        const std::string l(label), d(description);
        const auto c = color ? nativeColor(*color) : clap_color_t{};
        indication_->set_mapping(plugin_, id, mapped, color ? &c : nullptr,
                                 label.empty() ? nullptr : l.c_str(),
                                 description.empty() ? nullptr : d.c_str());
        return Status::ok;
    } catch (...) {
        return Status::allocation_failed;
    }
}
Status ClapBackend::indicateAutomation(std::uint32_t id, ClapAutomationIndication indication,
                                       const ClapColor* color) noexcept {
    if (const auto s = control(); s != Status::ok)
        return s;
    if (!capabilities_.parameterIndication)
        return Status::unsupported_format;
    if (std::uint32_t(indication) > CLAP_PARAM_INDICATION_AUTOMATION_OVERRIDING ||
        id == CLAP_INVALID_ID)
        return Status::invalid_argument;
    const auto c = color ? nativeColor(*color) : clap_color_t{};
    indication_->set_automation(plugin_, id, std::uint32_t(indication), color ? &c : nullptr);
    return Status::ok;
}
Status ClapBackend::saveState(ClapStateContext context, std::vector<std::uint8_t>& out,
                              std::size_t limit) noexcept {
    if (const auto s = control(true); s != Status::ok)
        return s;
    if (std::uint32_t(context) < 1 || std::uint32_t(context) > 3 || !limit ||
        limit > 64 * 1024 * 1024)
        return Status::invalid_argument;
    selectedStateContext_ = std::uint32_t(context);
    const auto s = saveState(out, limit);
    selectedStateContext_ = 0;
    return s;
}
Status ClapBackend::loadState(ClapStateContext context,
                              std::span<const std::uint8_t> data) noexcept {
    if (const auto s = control(true); s != Status::ok)
        return s;
    if (std::uint32_t(context) < 1 || std::uint32_t(context) > 3 || data.size() > 64 * 1024 * 1024)
        return Status::invalid_argument;
    selectedStateContext_ = std::uint32_t(context);
    const auto s = loadState(data);
    selectedStateContext_ = 0;
    return s;
}
Status ClapBackend::loadPreset(const ClapPresetLocation& location) noexcept {
    if (const auto s = control(true); s != Status::ok)
        return s;
    if (!capabilities_.presetLoad)
        return Status::unsupported_format;
    try {
        if (!validLocation(location))
            return Status::invalid_argument;
        const auto loaded = preset_->from_location(
            plugin_, std::uint32_t(location.kind),
            location.kind == ClapPresetLocationKind::plugin ? nullptr : location.location.c_str(),
            location.loadKey.empty() ? nullptr : location.loadKey.c_str());
        const auto refreshed = refresh();
        request(pluginPortsChanged | pluginParametersChanged);
        if (loaded)
            request(pluginStateDirty);
        return loaded ? refreshed : Status::io_error;
    } catch (...) {
        return Status::invalid_argument;
    }
}
Status ClapBackend::contextMenu(ClapMenuTarget target, std::vector<ClapMenuItem>& result) noexcept {
    if (const auto s = control(); s != Status::ok)
        return s;
    if (!capabilities_.contextMenu)
        return Status::unsupported_format;
    if (target.parameter && target.id == CLAP_INVALID_ID)
        return Status::invalid_argument;
    MenuReceiver receiver;
    const auto builder = receiver.builder();
    const auto t = nativeTarget(target);
    if (!menu_->populate(plugin_, &t, &builder) || receiver.failed || receiver.depth)
        return Status::io_error;
    result.swap(receiver.items);
    return Status::ok;
}
Status ClapBackend::performContextMenu(ClapMenuTarget target, std::uint32_t action) noexcept {
    if (const auto s = control(); s != Status::ok)
        return s;
    if (!capabilities_.contextMenu)
        return Status::unsupported_format;
    if (target.parameter && target.id == CLAP_INVALID_ID)
        return Status::invalid_argument;
    const auto t = nativeTarget(target);
    return menu_->perform(plugin_, &t, action) ? Status::ok : Status::invalid_argument;
}
Status ClapBackend::editorHints(ClapGuiHints& result) noexcept {
    if (const auto s = control(); s != Status::ok)
        return s;
    if (!editor_ || floating_)
        return Status::not_prepared;
    if (!gui_ || !gui_->get_resize_hints)
        return Status::unsupported_format;
    clap_gui_resize_hints_t hints{};
    if (!gui_->get_resize_hints(plugin_, &hints))
        return Status::unsupported_format;
    const bool preserve =
        hints.can_resize_horizontally && hints.can_resize_vertically && hints.preserve_aspect_ratio;
    if (preserve && (!hints.aspect_ratio_width || !hints.aspect_ratio_height))
        return Status::invalid_spec;
    result = {hints.can_resize_horizontally, hints.can_resize_vertically, preserve,
              preserve ? hints.aspect_ratio_width : 0u, preserve ? hints.aspect_ratio_height : 0u};
    return Status::ok;
}
Status ClapBackend::preferredEditorApi(PluginWindowApi& api, bool& floating) noexcept {
    if (const auto s = control(); s != Status::ok)
        return s;
    if (!gui_ || !gui_->get_preferred_api)
        return Status::unsupported_format;
    const char* name = nullptr;
    bool f = false;
    if (!gui_->get_preferred_api(plugin_, &name, &f))
        return Status::unsupported_format;
    if (!name || !*name) {
        if (!f)
            return Status::unsupported_format;
        api = PluginWindowApi::unspecified;
        floating = true;
        return Status::ok;
    }
    if (std::strcmp(name, CLAP_WINDOW_API_WIN32) == 0)
        api = PluginWindowApi::win32;
    else if (std::strcmp(name, CLAP_WINDOW_API_COCOA) == 0)
        api = PluginWindowApi::cocoa;
    else if (std::strcmp(name, CLAP_WINDOW_API_X11) == 0)
        api = PluginWindowApi::x11;
    else if (std::strcmp(name, CLAP_WINDOW_API_WAYLAND) == 0)
        api = PluginWindowApi::wayland;
    else
        return Status::unsupported_backend;
    floating = f;
    return Status::ok;
}
Status ClapBackend::suggestEditorTitle(std::string_view title) noexcept {
    if (const auto s = control(); s != Status::ok)
        return s;
    if (!editor_ || !floating_)
        return Status::not_prepared;
    if (!gui_ || !gui_->suggest_title)
        return Status::unsupported_format;
    if (!validString(title))
        return Status::invalid_argument;
    try {
        const std::string t(title);
        gui_->suggest_title(plugin_, t.c_str());
        return Status::ok;
    } catch (...) {
        return Status::allocation_failed;
    }
}

const void* CLAP_ABI ClapBackend::getExtension(const clap_host_t* host, const char* id) noexcept {
    if (!id)
        return nullptr;
    static const clap_host_thread_check_t thread{
        [](const clap_host_t* h) noexcept { return self(h).onMain(); },
        [](const clap_host_t* h) noexcept { return currentAudioPlugin == &self(h); }};
    static const clap_host_latency_t latency{
        [](const clap_host_t* h) noexcept { self(h).request(pluginLatencyChanged); }};
    static const clap_host_tail_t tail{
        [](const clap_host_t* h) noexcept { self(h).request(pluginTailChanged); }};
    static const clap_host_state_t state{
        [](const clap_host_t* h) noexcept { self(h).request(pluginStateDirty); }};
    static const clap_host_params_t params{
        [](const clap_host_t* h, std::uint32_t flags) noexcept {
            auto& s = self(h);
            if (!s.onMain())
                return;
            if (flags & CLAP_PARAM_RESCAN_ALL)
                s.cookiesInvalid_.store(1, std::memory_order_release);
            s.request(pluginParametersChanged);
            if (s.services_.parameterRescan)
                s.services_.parameterRescan(s.services_.context, flags);
        },
        [](const clap_host_t* h, clap_id id, std::uint32_t flags) noexcept {
            auto& s = self(h);
            if (!s.onMain())
                return;
            s.request(pluginParametersChanged);
            if (s.services_.parameterClear)
                s.services_.parameterClear(s.services_.context, id, flags);
        },
        [](const clap_host_t* h) noexcept { self(h).request(pluginParameterFlush); }};
    static const clap_host_audio_ports_t audio{
        [](const clap_host_t*, std::uint32_t flags) noexcept {
            return flags && !(flags & ~std::uint32_t(63));
        },
        [](const clap_host_t* h, std::uint32_t flags) noexcept {
            auto& s = self(h);
            if (!s.onMain())
                return;
            if (flags & CLAP_AUDIO_PORTS_RESCAN_LIST)
                s.portStatesInvalid_ = true;
            s.request(pluginPortsChanged);
        }};
    static const clap_host_note_ports_t notes{
        [](const clap_host_t*) noexcept {
            return std::uint32_t(CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI |
                                 CLAP_NOTE_DIALECT_MIDI_MPE | CLAP_NOTE_DIALECT_MIDI2);
        },
        [](const clap_host_t* h, std::uint32_t) noexcept { self(h).request(pluginPortsChanged); }};
    static const clap_host_gui_t gui{
        [](const clap_host_t* h) noexcept { self(h).request(pluginEditorResize); },
        [](const clap_host_t* h, std::uint32_t width, std::uint32_t height) noexcept {
            auto& s = self(h);
            if (!s.onMain() || !s.editor_ || s.floating_ || !width || !height || width > 32768 ||
                height > 32768 || !s.callbacks_.resizeEditor)
                return false;
            return s.callbacks_.resizeEditor(s.callbacks_.context, {width, height});
        },
        [](const clap_host_t* h) noexcept {
            self(h).request(pluginEditorShow);
            return true;
        },
        [](const clap_host_t* h) noexcept {
            self(h).request(pluginEditorHide);
            return true;
        },
        [](const clap_host_t* h, bool destroyed) noexcept {
            auto& s = self(h);
            if (destroyed)
                s.guiWasDestroyed_.store(1, std::memory_order_release);
            s.request(pluginEditorClosed);
        }};
    static const clap_host_timer_support_t timers{
        [](const clap_host_t* h, std::uint32_t period, clap_id* id) noexcept {
            auto& s = self(h);
            if (!s.onMain() || !period || !id || !s.nextTimer_ || s.nextTimer_ == CLAP_INVALID_ID)
                return false;
            for (auto& timer : s.timerSlots_)
                if (!timer.id) {
                    timer = {s.nextTimer_++, std::max(period, 1u), milliseconds() + period};
                    *id = timer.id;
                    return true;
                }
            return false;
        },
        [](const clap_host_t* h, clap_id id) noexcept {
            auto& s = self(h);
            if (!s.onMain() || !id)
                return false;
            for (auto& timer : s.timerSlots_)
                if (timer.id == id) {
                    timer = {};
                    return true;
                }
            return false;
        }};
    static const clap_host_audio_ports_config_t configs{
        [](const clap_host_t* h) noexcept { self(h).request(pluginAudioConfigurationsChanged); }};
    static const clap_host_surround_t surround{
        [](const clap_host_t* h) noexcept { self(h).request(pluginPortsChanged); }};
    static const clap_host_ambisonic_t ambisonic{
        [](const clap_host_t* h) noexcept { self(h).request(pluginPortsChanged); }};
    static const clap_host_voice_info_t voices{
        [](const clap_host_t* h) noexcept { self(h).request(pluginVoiceInfoChanged); }};
    static const clap_host_note_name_t names{
        [](const clap_host_t* h) noexcept { self(h).request(pluginNoteNamesChanged); }};
    static const clap_host_remote_controls_t remotes{
        [](const clap_host_t* h) noexcept { self(h).request(pluginRemoteControlsChanged); },
        [](const clap_host_t* h, clap_id page) noexcept {
            auto& s = self(h);
            if (s.onMain() && s.services_.suggestRemotePage)
                s.services_.suggestRemotePage(s.services_.context, page);
        }};
    static const clap_host_track_info_t track{
        [](const clap_host_t* h, clap_track_info_t* out) noexcept {
            auto& s = self(h);
            if (!out || !s.onMain() || !s.track_)
                return false;
            const auto& t = *s.track_;
            *out = {};
            if (t.name) {
                out->flags |= CLAP_TRACK_INFO_HAS_TRACK_NAME;
                const auto n = std::min(t.name->size(), sizeof(out->name) - 1);
                std::memcpy(out->name, t.name->data(), n);
            }
            if (t.color) {
                out->flags |= CLAP_TRACK_INFO_HAS_TRACK_COLOR;
                out->color = nativeColor(*t.color);
            }
            if (t.channels) {
                out->flags |= CLAP_TRACK_INFO_HAS_AUDIO_CHANNEL;
                out->audio_channel_count = std::int32_t(t.channels);
                out->audio_port_type = t.portType.empty() ? nullptr : t.portType.c_str();
            }
            if (t.returnTrack)
                out->flags |= CLAP_TRACK_INFO_IS_FOR_RETURN_TRACK;
            if (t.bus)
                out->flags |= CLAP_TRACK_INFO_IS_FOR_BUS;
            if (t.master)
                out->flags |= CLAP_TRACK_INFO_IS_FOR_MASTER;
            return true;
        }};
    static const clap_host_event_registry_t registry{
        [](const clap_host_t*, const char*, std::uint16_t* space) noexcept {
            if (space)
                *space = UINT16_MAX;
            // No application-specific event schemas are registered by this host.
            return false;
        }};
    static const clap_host_log_t log{
        [](const clap_host_t* h, clap_log_severity severity, const char* message) noexcept {
            self(h).queueLog(severity, message);
        }};
    static const clap_host_thread_pool_t pool{
        [](const clap_host_t* h, std::uint32_t count) noexcept {
            auto& s = self(h);
            if (currentAudioPlugin != &s || clapPoolTask || !s.inProcess_ || s.poolExecuting_ ||
                !count || count > 65536 || !s.pool_ || !s.pool_->exec || !s.services_.executeTasks)
                return false;
            s.poolExecuting_ = true;
            const bool done = s.services_.executeTasks(
                s.services_.context, count,
                [](void* context, std::uint32_t index) noexcept {
                    auto& plugin = *static_cast<ClapBackend*>(context);
                    PluginAudioScope taskScope(&plugin);
                    const auto previous = clapPoolTask;
                    clapPoolTask = true;
                    plugin.pool_->exec(plugin.plugin_, index);
                    clapPoolTask = previous;
                },
                &s);
            s.poolExecuting_ = false;
            return done;
        }};
    static const clap_host_preset_load_t preset{
        [](const clap_host_t* h, std::uint32_t kind, const char* location, const char* key,
           std::int32_t error, const char* message) noexcept {
            auto& s = self(h);
            if (!s.onMain())
                return;
            s.request(pluginPresetError);
            try {
                const ClapPresetLocation p{ClapPresetLocationKind(kind), text(location), text(key)};
                if (!validLocation(p))
                    return;
                if (s.services_.presetError)
                    s.services_.presetError(s.services_.context, p, error, text(message));
            } catch (...) {
                s.request(pluginPresetError);
            }
        },
        [](const clap_host_t* h, std::uint32_t kind, const char* location,
           const char* key) noexcept {
            auto& s = self(h);
            if (!s.onMain())
                return;
            s.request(pluginPresetLoaded);
            try {
                const ClapPresetLocation p{ClapPresetLocationKind(kind), text(location), text(key)};
                if (!validLocation(p))
                    return;
                if (s.services_.presetLoaded)
                    s.services_.presetLoaded(s.services_.context, p);
            } catch (...) {
                s.request(pluginPresetError);
            }
        }};
    static const clap_host_context_menu_t menu{
        [](const clap_host_t* h, const clap_context_menu_target_t* target,
           const clap_context_menu_builder_t* builder) noexcept {
            auto& s = self(h);
            ClapMenuTarget t;
            if (!s.onMain() || !s.services_.populateMenu || !menuTarget(target, t))
                return false;
            std::vector<ClapMenuItem> items;
            return s.services_.populateMenu(s.services_.context, t, items) &&
                   sendMenu(items, builder);
        },
        [](const clap_host_t* h, const clap_context_menu_target_t* target,
           clap_id action) noexcept {
            auto& s = self(h);
            ClapMenuTarget t;
            return s.onMain() && s.services_.performMenu && menuTarget(target, t) &&
                   s.services_.performMenu(s.services_.context, t, action);
        },
        [](const clap_host_t* h) noexcept {
            auto& s = self(h);
            return s.onMain() && s.editor_ && s.services_.popupMenu;
        },
        [](const clap_host_t* h, const clap_context_menu_target_t* target, std::int32_t screen,
           std::int32_t x, std::int32_t y) noexcept {
            auto& s = self(h);
            ClapMenuTarget t;
            return s.onMain() && s.editor_ && s.services_.popupMenu && menuTarget(target, t) &&
                   s.services_.popupMenu(s.services_.context, t, screen, x, y);
        }};
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS_CONFIG) == 0)
        return &configs;
    if (std::strcmp(id, CLAP_EXT_SURROUND) == 0 || std::strcmp(id, CLAP_EXT_SURROUND_COMPAT) == 0)
        return &surround;
    if (std::strcmp(id, CLAP_EXT_AMBISONIC) == 0 || std::strcmp(id, CLAP_EXT_AMBISONIC_COMPAT) == 0)
        return &ambisonic;
    if (std::strcmp(id, CLAP_EXT_VOICE_INFO) == 0)
        return &voices;
    if (std::strcmp(id, CLAP_EXT_NOTE_NAME) == 0)
        return &names;
    if (std::strcmp(id, CLAP_EXT_REMOTE_CONTROLS) == 0 ||
        std::strcmp(id, CLAP_EXT_REMOTE_CONTROLS_COMPAT) == 0)
        return &remotes;
    if (std::strcmp(id, CLAP_EXT_TRACK_INFO) == 0 ||
        std::strcmp(id, CLAP_EXT_TRACK_INFO_COMPAT) == 0)
        return &track;
    if (std::strcmp(id, CLAP_EXT_EVENT_REGISTRY) == 0)
        return &registry;
    if (std::strcmp(id, CLAP_EXT_LOG) == 0)
        return &log;
    if (std::strcmp(id, CLAP_EXT_THREAD_POOL) == 0 && self(host).services_.executeTasks)
        return &pool;
    if (std::strcmp(id, CLAP_EXT_PRESET_LOAD) == 0 ||
        std::strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT) == 0)
        return &preset;
    if ((std::strcmp(id, CLAP_EXT_CONTEXT_MENU) == 0 ||
         std::strcmp(id, CLAP_EXT_CONTEXT_MENU_COMPAT) == 0) &&
        (self(host).services_.populateMenu || self(host).services_.performMenu ||
         self(host).services_.popupMenu))
        return &menu;
#if !defined(_WIN32)
    static const clap_host_posix_fd_support_t fds{
        [](const clap_host_t* h, int fd, std::uint32_t flags) noexcept {
            auto& s = self(h);
            if (!s.onMain() || fd < 0 || !flags || (flags & ~7u))
                return false;
            for (const auto& slot : s.fdSlots_)
                if (slot.fd == fd)
                    return false;
            for (auto& slot : s.fdSlots_)
                if (slot.fd < 0) {
                    slot = {fd, flags};
                    return true;
                }
            return false;
        },
        [](const clap_host_t* h, int fd, std::uint32_t flags) noexcept {
            auto& s = self(h);
            if (!s.onMain() || !flags || (flags & ~7u))
                return false;
            for (auto& slot : s.fdSlots_)
                if (slot.fd == fd) {
                    slot.flags = flags;
                    return true;
                }
            return false;
        },
        [](const clap_host_t* h, int fd) noexcept {
            auto& s = self(h);
            if (!s.onMain())
                return false;
            for (auto& slot : s.fdSlots_)
                if (slot.fd == fd) {
                    slot = {};
                    return true;
                }
            return false;
        }};
    if (std::strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT) == 0)
        return &fds;
#endif
    if (std::strcmp(id, CLAP_EXT_THREAD_CHECK) == 0)
        return &thread;
    if (std::strcmp(id, CLAP_EXT_LATENCY) == 0)
        return &latency;
    if (std::strcmp(id, CLAP_EXT_TAIL) == 0)
        return &tail;
    if (std::strcmp(id, CLAP_EXT_STATE) == 0)
        return &state;
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
        return &params;
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
        return &audio;
    if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0)
        return &notes;
    if (std::strcmp(id, CLAP_EXT_GUI) == 0)
        return &gui;
    if (std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0)
        return &timers;
    return nullptr;
}
class ClapProvider final : public PluginProvider {
  public:
    Status scan(const std::filesystem::path& path,
                std::vector<PluginDescriptor>& result) noexcept override {
        try {
            std::shared_ptr<ClapModule> module;
            const auto r = loadClapModule(path, module);
            if (r != Status::ok)
                return r;
            const auto count = module->factory->get_plugin_count(module->factory);
            if (count > 4096)
                return Status::block_too_large;
            std::vector<PluginDescriptor> next;
            next.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
                next.push_back(
                    descriptor(module->factory->get_plugin_descriptor(module->factory, i)));
            result.swap(next);
            return Status::ok;
        } catch (const std::bad_alloc&) {
            return Status::allocation_failed;
        } catch (...) {
            return Status::unsupported_format;
        }
    }
    Status create(const std::filesystem::path& path, std::string_view id,
                  const PluginHostCallbacks& cb, PluginInstance& out) noexcept override {
        try {
            std::shared_ptr<ClapModule> module;
            auto r = loadClapModule(path, module);
            if (r != Status::ok)
                return r;
            auto backend = std::make_unique<ClapBackend>(std::move(module), cb);
            r = backend->initialize(id);
            return r == Status::ok ? PluginInstance::adopt(std::move(backend), out) : r;
        } catch (const std::bad_alloc&) {
            return Status::allocation_failed;
        } catch (...) {
            return Status::unsupported_format;
        }
    }
};
} // namespace
std::unique_ptr<PluginProvider> makeClapProvider() {
    return std::make_unique<ClapProvider>();
}
} // namespace cyaudio::detail
