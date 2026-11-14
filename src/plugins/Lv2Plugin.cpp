#include "cyaudio/core/DspTypes.h"
// Original CyDSP LV2 host; pinned ISC dependencies: third_party/licenses/lilv.txt
#include "PluginOutput.h"
#include "PluginStateBytes.h"
#include "cydsp/core/AudioBuffer.h"
#include "cydsp/core/SpscQueue.h"
#include "lilv/lilv.h"
#include "lv2/atom/forge.h"
#include "lv2/atom/util.h"
#include "lv2/buf-size/buf-size.h"
#include "lv2/data-access/data-access.h"
#include "lv2/instance-access/instance-access.h"
#include "lv2/midi/midi.h"
#include "lv2/options/options.h"
#include "lv2/parameters/parameters.h"
#include "lv2/state/state.h"
#include "lv2/time/time.h"
#include "lv2/ui/ui.h"
#include "lv2/urid/urid.h"
#include "lv2/worker/worker.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
namespace cyaudio::detail {
namespace {
struct World {
    LilvWorld* value = lilv_world_new();
    ~World() {
        lilv_world_free(value);
    }
};
struct Node {
    LilvNode* value = nullptr;
    Node(LilvWorld* w, const char* uri) : value(lilv_new_uri(w, uri)) {}
    explicit Node(LilvNode* n) : value(n) {}
    ~Node() {
        lilv_node_free(value);
    }
    operator const LilvNode*() const {
        return value;
    }
};
std::string text(const LilvNode* n) {
    return n ? lilv_node_as_string(n) : "";
}
std::string filePath(const LilvNode* n) {
    if (!n)
        return {};
    char* p = lilv_file_uri_parse(lilv_node_as_uri(n), nullptr);
    std::string s = p ? p : "";
    lilv_free(p);
    return s;
}
Status loadBundle(World& world, const std::filesystem::path& path) {
    if (!world.value)
        return Status::allocation_failed;
    if (!path.is_absolute() || !std::filesystem::is_directory(path))
        return Status::invalid_argument;
    auto name = pathUtf8(std::filesystem::canonical(path)) + "/";
    Node n(lilv_new_file_uri(world.value, nullptr, name.c_str()));
    if (!n.value)
        return Status::invalid_argument;
    lilv_world_load_bundle(world.value, n); // Explicit bundle, no global scan.
    return Status::ok;
}
PluginDescriptor descriptor(const LilvPlugin* p) {
    Node name(lilv_plugin_get_name(p)), author(lilv_plugin_get_author_name(p));
    return {PluginFormat::lv2, text(lilv_plugin_get_uri(p)), text(name), text(author), "", "LV2"};
}
// 4096 lock-free slots. Names (up to 1023 bytes each) are appended to one arena that
// is reserved but never pre-touched, so an instance pays only for the URIs it maps
// instead of a zero-filled kilobyte per slot.
class Urids {
    static constexpr std::uint32_t arenaBytes = 512 * 1024;
    struct Entry {
        std::atomic<std::uint32_t> state{0}; // 0 free, 1 being written, 2 published.
        std::uint32_t offset = 0;
    };
    std::unique_ptr<Entry[]> entries_ = std::make_unique<Entry[]>(4096);
    std::unique_ptr<char[]> arena_ = std::make_unique_for_overwrite<char[]>(arenaBytes);
    std::atomic<std::uint32_t> used_{0};

  public:
    LV2_URID_Map map{this, [](void* p, const char* s) { return static_cast<Urids*>(p)->get(s); }};
    LV2_URID_Unmap unmap{
        this, [](void* p, std::uint32_t id) { return static_cast<Urids*>(p)->name(id); }};
    std::uint32_t get(const char* s) noexcept {
        if (!s)
            return 0;
        std::size_t n = 0;
        std::uint32_t hash = 2166136261u;
        while (n < 1024 && s[n]) {
            hash = (hash ^ std::uint8_t(s[n++])) * 16777619u;
        }
        if (n == 1024)
            return 0;
        for (std::uint32_t i = 0; i < 4096; ++i) {
            auto index = (hash + i) % 4096;
            auto& e = entries_[index];
            auto state = e.state.load(std::memory_order_acquire);
            if (state == 1)
                return 0;
            if (state == 2) {
                if (std::strcmp(arena_.get() + e.offset, s) == 0)
                    return index + 1;
                continue;
            }
            std::uint32_t empty = 0;
            if (!e.state.compare_exchange_strong(empty, 1, std::memory_order_acq_rel))
                return 0;
            const auto bytes = std::uint32_t(n + 1);
            auto at = used_.load(std::memory_order_relaxed);
            do {
                if (bytes > arenaBytes - at) { // Arena exhausted: release the slot.
                    e.state.store(0, std::memory_order_release);
                    return 0;
                }
            } while (!used_.compare_exchange_weak(at, at + bytes, std::memory_order_acq_rel));
            std::memcpy(arena_.get() + at, s, bytes);
            e.offset = at;
            e.state.store(2, std::memory_order_release);
            return index + 1;
        }
        return 0;
    }
    const char* name(std::uint32_t id) const noexcept {
        if (!id || id > 4096 || entries_[id - 1].state.load(std::memory_order_acquire) != 2)
            return nullptr;
        return arena_.get() + entries_[id - 1].offset;
    }
};
struct UiControl {
    PluginEvent event;
};
// Single-producer/single-consumer ring of length-prefixed worker messages: at most
// 16 pending messages of at most 64 KiB each. The storage is reserved without being
// touched and a message costs one copy of its own bytes, not of a 64 KiB slot.
class WorkRing {
    static constexpr std::uint32_t maximumMessages = 16, maximumBytes = 65536, header = 8,
                                   capacity = maximumMessages * (maximumBytes + header);
    std::unique_ptr<std::uint8_t[]> data_, scratch_;
    std::uint32_t tail_ = 0; // Producer only.
    std::atomic<std::uint32_t> pushed_{0};
    std::uint32_t head_ = 0; // Consumer only.
    std::atomic<std::uint32_t> popped_{0};
    static std::uint32_t padded(std::uint32_t n) noexcept {
        return (n + 7u) & ~7u; // Payloads start 8-byte aligned.
    }

  public:
    void prepare() {
        auto data = std::make_unique_for_overwrite<std::uint8_t[]>(capacity);
        auto scratch = std::make_unique_for_overwrite<std::uint8_t[]>(maximumBytes);
        data_ = std::move(data);
        scratch_ = std::move(scratch);
        tail_ = head_ = 0;
        pushed_.store(0);
        popped_.store(0);
    }
    bool ready() const noexcept {
        return data_ != nullptr;
    }
    bool push(std::uint32_t n, const void* bytes) noexcept {
        if (!data_ || n > maximumBytes || (n && !bytes))
            return false;
        const auto pushed = pushed_.load(std::memory_order_relaxed);
        if (pushed - popped_.load(std::memory_order_acquire) >= maximumMessages)
            return false;
        std::memcpy(data_.get() + tail_, &n, sizeof(n));
        const auto at = (tail_ + header) % capacity;
        const auto first = std::min(n, capacity - at);
        if (first)
            std::memcpy(data_.get() + at, bytes, first);
        if (n > first)
            std::memcpy(data_.get(), static_cast<const std::uint8_t*>(bytes) + first, n - first);
        tail_ = (tail_ + header + padded(n)) % capacity;
        pushed_.store(pushed + 1, std::memory_order_release);
        return true;
    }
    // Oldest message, valid until this consumer calls discard(); null when empty.
    const std::uint8_t* peek(std::uint32_t& n) noexcept {
        if (!data_ ||
            popped_.load(std::memory_order_relaxed) == pushed_.load(std::memory_order_acquire))
            return nullptr;
        std::memcpy(&n, data_.get() + head_, sizeof(n));
        const auto at = (head_ + header) % capacity;
        if (n <= capacity - at)
            return data_.get() + at;
        const auto first = capacity - at; // Wrapped payload: hand out a contiguous copy.
        std::memcpy(scratch_.get(), data_.get() + at, first);
        std::memcpy(scratch_.get() + first, data_.get(), n - first);
        return scratch_.get();
    }
    void discard() noexcept {
        const auto popped = popped_.load(std::memory_order_relaxed);
        if (!data_ || popped == pushed_.load(std::memory_order_acquire))
            return;
        std::uint32_t n = 0;
        std::memcpy(&n, data_.get() + head_, sizeof(n));
        head_ = (head_ + header + padded(n)) % capacity;
        popped_.store(popped + 1, std::memory_order_release);
    }
};
class Lv2Backend final : public PluginBackend {
    World world_;
    const LilvPlugin* plugin_ = nullptr;
    LilvInstance* instance_ = nullptr;
    Urids urids_;
    PluginDescription desc_;
    PluginHostCallbacks callbacks_;
    PluginPrepareOptions options_;
    struct Port {
        enum Kind { audio, control, atom, optional } kind = optional;
        bool input = false, midi = false, time = false, rateScaled = false;
        std::uint32_t bus = 0, channel = 0, flat = 0;
        std::string symbol;
        float value = 0;
        std::vector<std::uint64_t> data;
        std::atomic<std::uint32_t> mirror{0}, dirty{0};
        Port() = default;
        Port(Port&& p) noexcept
            : kind(p.kind), input(p.input), midi(p.midi), time(p.time), rateScaled(p.rateScaled),
              bus(p.bus), channel(p.channel), flat(p.flat), symbol(std::move(p.symbol)),
              value(p.value), data(std::move(p.data)) {}
    };
    std::vector<Port> ports_;
    // Port indices by kind and the parameter slot of each control port, built once so
    // processing never scans or searches the whole port list per event or sub-block.
    std::vector<std::uint32_t> audioInputs_, audioOutputs_, atomPorts_, controlPorts_;
    std::vector<std::int32_t> parameterOfPort_;
    AudioBuffer<float> input_, output_; // Float storage for the double entry point.
    std::uint32_t totalInputs_ = 0, totalOutputs_ = 0;
    PluginOutput events_;
    SpscQueue<UiControl> uiQueue_;
    bool active_ = false;
    std::uint32_t latencyPort_ = 0xffffffffu;
    std::int32_t minBlock_ = 1, maxBlock_ = 512, nominalBlock_ = 512, sequenceBytes_ = 65536;
    float sampleRate_ = 48000;
    std::uint32_t midi_ = 0, sequence_ = 0, chunk_ = 0, float_ = 0;
    // Resolved once: forging a time position per sub-block maps nothing.
    LV2_Atom_Forge forge_{};
    std::uint32_t timePosition_ = 0, timeSpeed_ = 0, timeFrame_ = 0, timeBpm_ = 0,
                  timeBeatUnit_ = 0, timeBeatsPerBar_ = 0, timeBeat_ = 0;
    std::array<LV2_Options_Option, 6> opts_{};
    std::array<LV2_Feature, 5> features_{};
    std::array<const LV2_Feature*, 6> featurePointers_{};
    WorkRing requests_, responses_; // Allocated only for plugins with a worker interface.
    const LV2_Worker_Interface* worker_ = nullptr;
    std::thread workerThread_;
    std::atomic<bool> quit_{false};
    // LV2 forbids work() concurrent with state save/restore. Both run off the audio
    // thread, so an ordinary mutex serializes them.
    std::mutex workMutex_;
    DroppedEvents dropped_;
    LV2_Worker_Schedule schedule_{this, [](void* p, std::uint32_t n, const void* b) {
                                      return static_cast<Lv2Backend*>(p)->schedule(n, b);
                                  }};
    LV2_Worker_Status schedule(std::uint32_t n, const void* b) noexcept {
        if (quit_.load())
            return LV2_WORKER_ERR_NO_SPACE;
        return requests_.push(n, b) ? LV2_WORKER_SUCCESS : LV2_WORKER_ERR_NO_SPACE;
    }
    static LV2_Worker_Status respond(void* p, std::uint32_t n, const void* b) {
        auto& self = *static_cast<Lv2Backend*>(p);
        return self.responses_.push(n, b) ? LV2_WORKER_SUCCESS : LV2_WORKER_ERR_NO_SPACE;
    }
    void stopWorker() noexcept {
        quit_.store(true);
        if (workerThread_.joinable())
            workerThread_.join();
        if (instance_ && worker_ && worker_->work_response) {
            PluginAudioScope audio(this);
            std::uint32_t bytes = 0;
            while (const auto* response = responses_.peek(bytes)) {
                worker_->work_response(lilv_instance_get_handle(instance_), bytes, response);
                responses_.discard();
            }
        }
    }
    void launchWorker() {
        if (workerThread_.joinable())
            return;
        quit_.store(false);
        if (!worker_ || !worker_->work)
            return;
        workerThread_ = std::thread([this] {
            std::uint32_t bytes = 0;
            while (!quit_.load() || requests_.peek(bytes)) {
                if (const auto* work = requests_.peek(bytes)) {
                    {
                        std::lock_guard lock(workMutex_);
                        worker_->work(lilv_instance_get_handle(instance_), respond, this, bytes,
                                      work);
                    }
                    requests_.discard();
                } else
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }
    void setupFeatures() {
        midi_ = urids_.get(LV2_MIDI__MidiEvent);
        sequence_ = urids_.get(LV2_ATOM__Sequence);
        chunk_ = urids_.get(LV2_ATOM__Chunk);
        float_ = urids_.get(LV2_ATOM__Float);
        lv2_atom_forge_init(&forge_, &urids_.map);
        timePosition_ = urids_.get(LV2_TIME__Position);
        timeSpeed_ = urids_.get(LV2_TIME__speed);
        timeFrame_ = urids_.get(LV2_TIME__frame);
        timeBpm_ = urids_.get(LV2_TIME__beatsPerMinute);
        timeBeatUnit_ = urids_.get(LV2_TIME__beatUnit);
        timeBeatsPerBar_ = urids_.get(LV2_TIME__beatsPerBar);
        timeBeat_ = urids_.get(LV2_TIME__beat);
        auto integer = urids_.get(LV2_ATOM__Int);
        opts_ = {{{LV2_OPTIONS_INSTANCE, 0, urids_.get(LV2_BUF_SIZE__minBlockLength), 4, integer,
                   &minBlock_},
                  {LV2_OPTIONS_INSTANCE, 0, urids_.get(LV2_BUF_SIZE__maxBlockLength), 4, integer,
                   &maxBlock_},
                  {LV2_OPTIONS_INSTANCE, 0, urids_.get(LV2_BUF_SIZE__nominalBlockLength), 4,
                   integer, &nominalBlock_},
                  {LV2_OPTIONS_INSTANCE, 0, urids_.get(LV2_BUF_SIZE__sequenceSize), 4, integer,
                   &sequenceBytes_},
                  {LV2_OPTIONS_INSTANCE, 0, urids_.get(LV2_PARAMETERS__sampleRate), 4, float_,
                   &sampleRate_},
                  {}}};
        features_ = {{{LV2_URID__map, &urids_.map},
                      {LV2_URID__unmap, &urids_.unmap},
                      {LV2_OPTIONS__options, opts_.data()},
                      {LV2_BUF_SIZE__boundedBlockLength, nullptr},
                      {LV2_WORKER__schedule, &schedule_}}};
        for (std::size_t i = 0; i < features_.size(); ++i)
            featurePointers_[i] = &features_[i];
    }
    bool feature(const char* uri) const noexcept {
        for (const auto* f : featurePointers_)
            if (f && std::strcmp(f->URI, uri) == 0)
                return true;
        return false;
    }
    const PluginParameter* parameter(std::uint32_t id) const noexcept {
        return id < parameterOfPort_.size() && parameterOfPort_[id] >= 0
                   ? &desc_.parameters[std::size_t(parameterOfPort_[id])]
                   : nullptr;
    }
    Status set(const PluginEvent& e) noexcept {
        const auto* p = parameter(e.id);
        if (!p || p->readOnly || !isFinite(e.value) || e.value < p->minimum ||
            e.value > p->maximum || !isFinite(float(e.value)))
            return Status::invalid_argument;
        ports_[e.id].value = float(e.value);
        return Status::ok;
    }
    void connectControls() noexcept {
        for (std::size_t i = 0; i < ports_.size(); ++i)
            lilv_instance_connect_port(instance_, std::uint32_t(i),
                                       ports_[i].kind == Port::control ? &ports_[i].value
                                                                       : nullptr);
    }
    Status instantiate() {
        instance_ = lilv_plugin_instantiate(plugin_, sampleRate_, featurePointers_.data());
        if (!instance_)
            return Status::unsupported_format;
        worker_ = static_cast<const LV2_Worker_Interface*>(
            lilv_instance_get_extension_data(instance_, LV2_WORKER__interface));
        if (worker_ && worker_->work && !requests_.ready()) {
            requests_.prepare();
            responses_.prepare();
        }
        connectControls();
        launchWorker();
        return Status::ok;
    }
    LV2_Extension_Data_Feature uiAccess_{};
    std::atomic<std::uint32_t> uiRequests_{0};
    float uiScale_ = 1, uiRate_ = 30;
    std::array<LV2_Options_Option, 3> uiOptions_{};
    LV2UI_Port_Map uiPortMap_{this, [](void* p, const char* symbol) -> std::uint32_t {
                                  if (!symbol)
                                      return LV2UI_INVALID_PORT_INDEX;
                                  const auto& self = *static_cast<Lv2Backend*>(p);
                                  for (std::size_t i = 0; i < self.ports_.size(); ++i)
                                      if (self.ports_[i].symbol == symbol)
                                          return std::uint32_t(i);
                                  return LV2UI_INVALID_PORT_INDEX;
                              }};
    LV2UI_Touch uiTouch_{this, [](void* p, std::uint32_t id, bool grab) {
                             auto& self = *static_cast<Lv2Backend*>(p);
                             auto* param = self.parameter(id);
                             if (!param || param->readOnly)
                                 return;
                             PluginEvent e;
                             e.kind =
                                 grab ? PluginEventKind::gestureBegin : PluginEventKind::gestureEnd;
                             e.id = id;
                             e.flags = 1;
                             if (self.uiQueue_.push({e})) {
                                 self.uiRequests_.fetch_or(pluginStateDirty | pluginProcess);
                                 if (self.callbacks_.edited)
                                     self.callbacks_.edited(self.callbacks_.context, e);
                             }
                         }};
    std::array<LV2_Feature, 9> uiFeatures_{};
    std::array<const LV2_Feature*, 10> uiFeaturePointers_{};
    std::string bundlePath_;
    std::unique_ptr<SharedModule> uiLibrary_;
    const LV2UI_Descriptor* uiDescriptor_ = nullptr;
    LV2UI_Handle ui_ = nullptr;
    const LV2UI_Idle_Interface* uiIdle_ = nullptr;
    PluginEditorSize editorSize_{320, 200};
    LV2UI_Resize hostResize_{this, [](void* p, int w, int h) {
                                 auto& self = *static_cast<Lv2Backend*>(p);
                                 if (w <= 0 || h <= 0 || w > 16384 || h > 16384)
                                     return 1;
                                 PluginEditorSize size{std::uint32_t(w), std::uint32_t(h)};
                                 if (!self.callbacks_.resizeEditor ||
                                     !self.callbacks_.resizeEditor(self.callbacks_.context, size))
                                     return 1;
                                 self.editorSize_ = size;
                                 return 0;
                             }};
    static void uiWrite(void* p, std::uint32_t id, std::uint32_t n, std::uint32_t protocol,
                        const void* data) {
        auto& self = *static_cast<Lv2Backend*>(p);
        if (protocol || n != 4 || !data || !self.parameter(id))
            return;
        float value = 0;
        std::memcpy(&value, data, 4);
        const auto* param = self.parameter(id);
        if (!isFinite(value) || param->readOnly || value < param->minimum || value > param->maximum)
            return;
        PluginEvent e;
        e.kind = PluginEventKind::parameterValue;
        e.id = id;
        e.value = value;
        e.flags = 1;
        if (!self.uiQueue_.push({e}))
            return;
        self.uiRequests_.fetch_or(pluginStateDirty | pluginProcess);
        if (self.callbacks_.edited)
            self.callbacks_.edited(self.callbacks_.context, e);
    }
    template <Sample T> Status run(const PluginProcess<T>&) noexcept;

  public:
    ~Lv2Backend() override {
        closeEditor();
        stopWorker();
        if (instance_) {
            if (active_)
                lilv_instance_deactivate(instance_);
            lilv_instance_free(instance_);
        }
    }
    Status initialize(const std::filesystem::path&, std::string_view, const PluginHostCallbacks&);
    const PluginDescription& description() const noexcept override {
        return desc_;
    }
    Status activate(const PluginPrepareOptions&) noexcept override;
    Status deactivate() noexcept override {
        stopWorker();
        if (active_)
            lilv_instance_deactivate(instance_);
        active_ = false;
        return Status::ok;
    }
    Status start() noexcept override {
        return active_ ? Status::ok : Status::not_prepared;
    }
    void stop() noexcept override {}
    Status reset() noexcept override {
        return Status::unsupported_format;
    }
    Status process(const PluginProcess<float>& p) noexcept override {
        return run(p);
    }
    Status process(const PluginProcess<double>& p) noexcept override {
        return run(p);
    }
    PluginTiming timing() const noexcept override {
        if (latencyPort_ == 0xffffffffu)
            return {0, 0, true, false, false};
        auto id = latencyPort_;
        auto v =
            id < ports_.size()
                ? (active_ ? std::bit_cast<float>(ports_[id].mirror.load(std::memory_order_acquire))
                           : ports_[id].value)
                : -1.f;
        return {v >= 0 && isFinite(v) && double(v) <= 4294967295. ? std::uint32_t(v) : 0, 0,
                v >= 0 && isFinite(v) && double(v) <= 4294967295., false, false};
    }
    Status parameterValue(std::uint32_t id, double& v) noexcept override {
        if (!parameter(id))
            return Status::invalid_argument;
        v = ports_[id].value;
        return Status::ok;
    }
    Status parameterText(std::uint32_t id, double v, std::string& out) noexcept override {
        if (!parameter(id) || !isFinite(v))
            return Status::invalid_argument;
        try {
            out = std::to_string(v);
            return Status::ok;
        } catch (...) {
            return Status::allocation_failed;
        }
    }
    Status flush(const PluginEventBuffer& e, PluginEventBuffer&) noexcept override {
        for (std::size_t i = 0; i < e.size(); ++i) {
            if (e.event(i)->kind != PluginEventKind::parameterValue)
                return Status::unsupported_format;
            auto r = set(*e.event(i));
            if (r != Status::ok)
                return r;
        }
        return Status::ok;
    }
    Status saveState(std::vector<std::uint8_t>&, std::size_t) noexcept override;
    Status loadState(std::span<const std::uint8_t>) noexcept override;
    Status openEditor(const PluginWindow&, PluginEditorSize&) noexcept override;
    Status resizeEditor(PluginEditorSize&) noexcept override;
    Status scaleEditor(double) noexcept override;
    void closeEditor() noexcept override;
    std::uint32_t pumpMainThread() noexcept override;
};

template <Sample T> Status Lv2Backend::run(const PluginProcess<T>& p) noexcept {
    events_.reset();
    if (p.transport && p.transport->hasTempo && p.transport->tempoIncrement != 0)
        return Status::unsupported_format;
    if (p.inputEvents)
        for (std::size_t i = 0; i < p.inputEvents->size(); ++i) {
            const auto& e = *p.inputEvents->event(i);
            if (e.kind == PluginEventKind::parameterValue) {
                const auto* param = parameter(e.id);
                if (!param || param->readOnly || e.value < param->minimum ||
                    e.value > param->maximum || e.noteId != -1 || e.port != -1 || e.channel != -1 ||
                    e.key != -1)
                    return Status::invalid_argument;
            } else if (e.kind == PluginEventKind::gestureBegin ||
                       e.kind == PluginEventKind::gestureEnd) {
                if (!parameter(e.id))
                    return Status::invalid_argument;
            } else if (e.kind == PluginEventKind::transport)
                return Status::unsupported_format;
            else {
                std::array<std::uint8_t, 3> b;
                if (pluginMidi(e, b).empty() || e.port < 0 ||
                    std::size_t(e.port) >= desc_.noteInputs.size())
                    return Status::unsupported_format;
            }
        }
    // A UI edit that can no longer be applied or recorded is dropped and counted; it
    // must not stay at the queue head and fail every later block.
    while (const auto* e = uiQueue_.peek()) {
        if ((e->event.kind == PluginEventKind::parameterValue && set(e->event) != Status::ok) ||
            events_.add(e->event) != Status::ok)
            dropped_.add();
        uiQueue_.discard();
    }
    if (worker_ && worker_->work_response) {
        std::uint32_t bytes = 0;
        while (const auto* response = responses_.peek(bytes)) {
            worker_->work_response(lilv_instance_get_handle(instance_), bytes, response);
            responses_.discard();
        }
    }
    std::uint32_t begin = 0;
    std::size_t cursor = 0;
    while (begin < p.frames) {
        auto end = p.frames;
        if (p.inputEvents)
            for (std::size_t i = cursor; i < p.inputEvents->size(); ++i) {
                const auto& e = *p.inputEvents->event(i);
                if (e.kind == PluginEventKind::parameterValue && e.frame > begin) {
                    end = e.frame;
                    break;
                }
            }
        if (p.inputEvents)
            for (auto i = cursor;
                 i < p.inputEvents->size() && p.inputEvents->event(i)->frame == begin; ++i) {
                const auto& e = *p.inputEvents->event(i);
                if (e.kind == PluginEventKind::parameterValue) {
                    auto r = set(e);
                    if (r != Status::ok)
                        return r;
                }
            }
        const auto frames = end - begin;
        if constexpr (std::same_as<T, float>) {
            // Native precision runs directly on the caller's memory. The instance never
            // lets an LV2 output alias an input, so nothing is copied in or out.
            for (const auto i : audioInputs_) {
                const auto& port = ports_[i];
                lilv_instance_connect_port(
                    instance_, i,
                    const_cast<float*>(p.inputs[port.bus].channel(port.channel).data() + begin));
            }
            for (const auto i : audioOutputs_) {
                const auto& port = ports_[i];
                lilv_instance_connect_port(
                    instance_, i, p.outputs[port.bus].channel(port.channel).data() + begin);
            }
        } else {
            // Double blocks convert through the float storage that the audio ports
            // have been connected to since activate(); nothing is reconnected here.
            auto in = input_.block();
            for (const auto i : audioInputs_) {
                const auto& port = ports_[i];
                auto source = p.inputs[port.bus].channel(port.channel).subspan(begin, frames);
                auto dest = in.channel(port.flat).first(frames);
                for (std::size_t frame = 0; frame < dest.size(); ++frame)
                    dest[frame] = float(source[frame]);
            }
        }
        // Atom buffers keep the address connected in activate().
        for (const auto i : atomPorts_) {
            auto& port = ports_[i];
            {
                auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(port.data.data());
                const auto capacity = std::uint32_t(port.data.size() * 8);
                seq->atom = {port.input ? std::uint32_t(sizeof(LV2_Atom_Sequence_Body))
                                        : capacity - 8,
                             port.input ? sequence_ : chunk_};
                seq->body = {0, 0};
                if (port.input && port.time) {
                    alignas(8) std::array<std::uint8_t, 512> timeBytes{};
                    LV2_Atom_Forge forge = forge_;
                    lv2_atom_forge_set_buffer(&forge, timeBytes.data(), timeBytes.size());
                    LV2_Atom_Forge_Frame object;
                    lv2_atom_forge_object(&forge, &object, 0, timePosition_);
                    const auto t = p.transport ? *p.transport : PluginTransport{};
                    lv2_atom_forge_key(&forge, timeSpeed_);
                    lv2_atom_forge_float(&forge, t.playing ? 1.f : 0.f);
                    if (t.hasSongFrame || p.steadyFrame >= 0) {
                        lv2_atom_forge_key(&forge, timeFrame_);
                        lv2_atom_forge_long(&forge,
                                            (t.hasSongFrame ? t.songFrame : p.steadyFrame) + begin);
                    }
                    if (t.hasTempo) {
                        lv2_atom_forge_key(&forge, timeBpm_);
                        lv2_atom_forge_float(&forge, float(t.tempo));
                    }
                    if (t.hasSignature) {
                        lv2_atom_forge_key(&forge, timeBeatUnit_);
                        lv2_atom_forge_int(&forge, t.denominator);
                        lv2_atom_forge_key(&forge, timeBeatsPerBar_);
                        lv2_atom_forge_float(&forge, float(t.numerator));
                    }
                    if (t.hasBeats) {
                        lv2_atom_forge_key(&forge, timeBeat_);
                        lv2_atom_forge_double(&forge, t.beats + begin / options_.sampleRate *
                                                                    t.tempo / 60.);
                    }
                    lv2_atom_forge_pop(&forge, &object);
                    struct {
                        LV2_Atom_Event e;
                        std::array<std::uint8_t, 512> bytes;
                    } event{};
                    const auto* body = reinterpret_cast<const LV2_Atom*>(timeBytes.data());
                    event.e.body = *body;
                    std::memcpy(event.bytes.data(), body + 1, body->size);
                    if (!lv2_atom_sequence_append_event(seq, capacity - 8, &event.e))
                        return Status::update_queue_full;
                }
                if (port.input && port.midi && p.inputEvents)
                    for (std::size_t j = cursor; j < p.inputEvents->size(); ++j) {
                        const auto& e = *p.inputEvents->event(j);
                        if (e.frame >= end)
                            break; // Events are ordered by frame.
                        if (e.frame < begin || e.port != std::int16_t(port.bus))
                            continue;
                        std::array<std::uint8_t, 3> b;
                        auto bytes = pluginMidi(e, b);
                        if (bytes.empty())
                            continue;
                        auto offset = sizeof(LV2_Atom) + seq->atom.size;
                        auto size = std::size_t(lv2_atom_pad_size(
                            std::uint32_t(sizeof(LV2_Atom_Event) + bytes.size())));
                        if (size > capacity - offset)
                            return Status::update_queue_full;
                        LV2_Atom_Event ev{};
                        ev.time.frames = e.frame - begin;
                        ev.body = {std::uint32_t(bytes.size()), midi_};
                        auto* dst = reinterpret_cast<std::uint8_t*>(seq) + offset;
                        std::memcpy(dst, &ev, sizeof(ev));
                        std::memcpy(dst + sizeof(ev), bytes.data(), bytes.size());
                        std::memset(dst + sizeof(ev) + bytes.size(), 0,
                                    size - sizeof(ev) - bytes.size());
                        seq->atom.size += std::uint32_t(size);
                    }
            }
        }
        lilv_instance_run(instance_, frames);
        if constexpr (std::same_as<T, double>) {
            auto out = output_.block();
            for (const auto i : audioOutputs_) {
                const auto& port = ports_[i];
                auto source = out.channel(port.flat).first(frames);
                auto dest = p.outputs[port.bus].channel(port.channel).subspan(begin, frames);
                for (std::size_t frame = 0; frame < dest.size(); ++frame)
                    dest[frame] = T(source[frame]);
            }
        }
        for (const auto i : atomPorts_) {
            auto& port = ports_[i];
            if (!port.input) {
                const auto* seq = reinterpret_cast<const LV2_Atom_Sequence*>(port.data.data());
                if (seq->atom.type != sequence_)
                    continue;
                // A malformed output sequence or event is dropped and counted. The audio
                // this run produced is valid and is never discarded because of it.
                auto bytes = std::size_t(seq->atom.size);
                if (bytes < 8 || bytes > port.data.size() * 8 - 8 || seq->body.unit) {
                    dropped_.add();
                    continue;
                }
                std::size_t at = sizeof(LV2_Atom_Sequence), stop = bytes + 8;
                while (at < stop) {
                    if (stop - at < sizeof(LV2_Atom_Event)) {
                        dropped_.add();
                        break;
                    }
                    const auto* e = reinterpret_cast<const LV2_Atom_Event*>(
                        reinterpret_cast<const std::uint8_t*>(seq) + at);
                    if (e->body.size > stop - at - sizeof(*e)) {
                        dropped_.add();
                        break;
                    }
                    auto size = lv2_atom_pad_size(std::uint32_t(sizeof(*e) + e->body.size));
                    if (size > stop - at) {
                        // The last event may omit its trailing padding.
                        size = std::uint32_t(stop - at);
                    }
                    if (e->time.frames < 0 || std::uint64_t(e->time.frames) >= end - begin) {
                        dropped_.add();
                        at += size;
                        continue;
                    }
                    if (e->body.type == midi_ && port.midi && e->body.size) {
                        auto* b = reinterpret_cast<const std::uint8_t*>(e + 1);
                        PluginEvent event;
                        event.kind = b[0] == 0xf0 ? PluginEventKind::sysex : PluginEventKind::midi1;
                        event.port = std::int16_t(port.bus);
                        event.frame = begin + std::uint32_t(e->time.frames);
                        event.bytes = {b, e->body.size};
                        if (events_.add(event) != Status::ok)
                            dropped_.add();
                    }
                    at += size;
                }
            }
        }
        begin = end;
        if (p.inputEvents)
            while (cursor < p.inputEvents->size() && p.inputEvents->event(cursor)->frame < end)
                ++cursor;
    }
    if (worker_ && worker_->end_run)
        worker_->end_run(lilv_instance_get_handle(instance_));
    // Only a control whose value moved is marked for the UI: an idle block no longer
    // makes every later pump send port_event for every port.
    for (const auto i : controlPorts_) {
        auto& port = ports_[i];
        const auto bits = std::bit_cast<std::uint32_t>(port.value);
        if (port.mirror.load(std::memory_order_relaxed) != bits) {
            port.mirror.store(bits, std::memory_order_relaxed);
            port.dirty.store(1, std::memory_order_release);
        }
    }
    dropped_.add(events_.finish(p.outputEvents));
    return Status::ok;
}
Status Lv2Backend::initialize(const std::filesystem::path& path, std::string_view id,
                              const PluginHostCallbacks& cb) {
    callbacks_ = cb;
    auto r = loadBundle(world_, path);
    if (r != Status::ok)
        return r;
    Node pluginUri(world_.value, std::string(id).c_str());
    plugin_ = lilv_plugins_get_by_uri(lilv_world_get_all_plugins(world_.value), pluginUri);
    if (!plugin_)
        return Status::unsupported_format;
    setupFeatures();
    if (lilv_plugin_has_latency(plugin_))
        latencyPort_ = lilv_plugin_get_latency_port_index(plugin_);
    auto* required = lilv_plugin_get_required_features(plugin_);
    bool supported = true;
    LILV_FOREACH(nodes, i, required)
    if (!feature(lilv_node_as_uri(lilv_nodes_get(required, i))))
        supported = false;
    lilv_nodes_free(required);
    if (!supported)
        return Status::unsupported_format;
    desc_.plugin = descriptor(plugin_);
    desc_.state = true;
    desc_.float32 = desc_.float64 = true;
    bundlePath_ = filePath(lilv_plugin_get_bundle_uri(plugin_));
    auto count = lilv_plugin_get_num_ports(plugin_);
    if (count > 4096)
        return Status::invalid_spec;
    ports_.resize(count);
    std::uint32_t ins = 0, outs = 0, notesIn = 0, notesOut = 0;
    std::vector<std::string> inputGroups, outputGroups;
    Node groupProperty(world_.value, "http://lv2plug.in/ns/ext/port-groups#group");
    Node groupName(world_.value, LV2_CORE__name);
    Node input(world_.value, LV2_CORE__InputPort), audio(world_.value, LV2_CORE__AudioPort),
        control(world_.value, LV2_CORE__ControlPort), atom(world_.value, LV2_ATOM__AtomPort),
        optional(world_.value, LV2_CORE__connectionOptional),
        bufferType(world_.value, LV2_ATOM__bufferType), sequence(world_.value, LV2_ATOM__Sequence),
        supports(world_.value, LV2_ATOM__supports), midi(world_.value, LV2_MIDI__MidiEvent),
        time(world_.value, LV2_TIME__Position), integer(world_.value, LV2_CORE__integer),
        toggled(world_.value, LV2_CORE__toggled),
        sampleRateProperty(world_.value, LV2_CORE__sampleRate);
    for (std::uint32_t i = 0; i < count; ++i) {
        auto* p = lilv_plugin_get_port_by_index(plugin_, i);
        auto& port = ports_[i];
        port.symbol = text(lilv_port_get_symbol(plugin_, p));
        port.input = lilv_port_is_a(plugin_, p, input);
        if (lilv_port_is_a(plugin_, p, audio)) {
            port.kind = Port::audio;
            port.flat = port.input ? ins++ : outs++;
            Node group(lilv_port_get(plugin_, p, groupProperty));
            auto key = text(group);
            auto& names = port.input ? inputGroups : outputGroups;
            auto& buses = port.input ? desc_.inputs : desc_.outputs;
            auto it = std::find(names.begin(), names.end(), key);
            if (it == names.end()) {
                names.push_back(key);
                Node label(group.value ? lilv_world_get(world_.value, group, groupName, nullptr)
                                       : nullptr);
                buses.push_back({i, 0, 0xffffffffu,
                                 label.value ? text(label) : (port.input ? "Inputs" : "Outputs"),
                                 "audio", buses.empty(), false});
                port.bus = std::uint32_t(buses.size() - 1);
            } else
                port.bus = std::uint32_t(it - names.begin());
            port.channel = buses[port.bus].channels++;
        } else if (lilv_port_is_a(plugin_, p, control)) {
            port.kind = Port::control;
            LilvNode *def = nullptr, *min = nullptr, *max = nullptr;
            lilv_port_get_range(plugin_, p, &def, &min, &max);
            Node d(def), lo(min), hi(max), name(lilv_port_get_name(plugin_, p));
            PluginParameter par;
            par.id = i;
            par.name = text(name);
            par.minimum = min ? lilv_node_as_float(min) : -std::numeric_limits<float>::max();
            par.maximum = max ? lilv_node_as_float(max) : std::numeric_limits<float>::max();
            par.defaultValue =
                def ? lilv_node_as_float(def) : std::clamp(0., par.minimum, par.maximum);
            port.rateScaled = lilv_port_has_property(plugin_, p, sampleRateProperty);
            if (port.rateScaled) {
                par.minimum *= sampleRate_;
                par.maximum *= sampleRate_;
                par.defaultValue *= sampleRate_;
            }
            if (!isFinite(par.minimum) || !isFinite(par.maximum) || !isFinite(par.defaultValue) ||
                par.minimum > par.maximum || par.defaultValue < par.minimum ||
                par.defaultValue > par.maximum)
                return Status::invalid_spec;
            par.stepped = lilv_port_has_property(plugin_, p, integer) ||
                          lilv_port_has_property(plugin_, p, toggled);
            par.readOnly = !port.input;
            par.automatable = port.input;
            port.value = float(par.defaultValue);
            desc_.parameters.push_back(std::move(par));
        } else if (lilv_port_is_a(plugin_, p, atom)) {
            auto* types = lilv_port_get_value(plugin_, p, bufferType);
            auto isSeq = lilv_nodes_contains(types, sequence);
            lilv_nodes_free(types);
            if (!isSeq)
                return Status::unsupported_format;
            port.kind = Port::atom;
            auto* typesSupported = lilv_port_get_value(plugin_, p, supports);
            port.midi = lilv_nodes_contains(typesSupported, midi);
            port.time = lilv_nodes_contains(typesSupported, time);
            lilv_nodes_free(typesSupported);
            if (port.midi) {
                port.bus = port.input ? notesIn++ : notesOut++;
                auto& list = port.input ? desc_.noteInputs : desc_.noteOutputs;
                Node name(lilv_port_get_name(plugin_, p));
                list.push_back({i, 2, 2, text(name)});
            }
        } else if (!lilv_port_has_property(plugin_, p, optional))
            return Status::unsupported_format;
    }
    if (ins > 256 || outs > 256)
        return Status::invalid_spec;
    totalInputs_ = ins;
    totalOutputs_ = outs;
    parameterOfPort_.assign(count, -1);
    for (std::size_t i = 0; i < desc_.parameters.size(); ++i)
        parameterOfPort_[desc_.parameters[i].id] = std::int32_t(i);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto& port = ports_[i];
        if (port.kind == Port::audio)
            (port.input ? audioInputs_ : audioOutputs_).push_back(i);
        else if (port.kind == Port::atom)
            atomPorts_.push_back(i);
        else if (port.kind == Port::control)
            controlPorts_.push_back(i);
    }
    auto* uis = lilv_plugin_get_uis(plugin_);
    Node nativeUi(world_.value,
#if defined(_WIN32)
                  LV2_UI__WindowsUI
#elif defined(__APPLE__)
                  LV2_UI__CocoaUI
#else
                  LV2_UI__X11UI
#endif
    );
    LILV_FOREACH(uis, u, uis) if (lilv_ui_is_a(lilv_uis_get(uis, u), nativeUi)) desc_.editor = true;
    lilv_uis_free(uis);
    auto q = uiQueue_.prepare(1024);
    if (q != Status::ok)
        return q;
    return instantiate();
}
Status Lv2Backend::activate(const PluginPrepareOptions& o) noexcept {
    if (!instance_)
        return Status::not_prepared;
    try {
        if (o.sampleRate != sampleRate_) {
            const auto ratio = o.sampleRate / sampleRate_;
            for (const auto& p : desc_.parameters)
                if (ports_[p.id].rateScaled && !p.readOnly)
                    if (ports_[p.id].value < p.minimum * ratio ||
                        ports_[p.id].value > p.maximum * ratio)
                        return Status::invalid_spec;
        }
        if (ui_ && (sampleRate_ != o.sampleRate || std::uint32_t(maxBlock_) < o.maximumFrames))
            return Status::transition_in_progress;
        if (sampleRate_ != o.sampleRate || std::uint32_t(maxBlock_) < o.maximumFrames) {
            // Drain accepted work before the state is read from this instance.
            stopWorker();
            std::vector<std::uint8_t> saved;
            auto r = saveState(saved, 64 * 1024 * 1024);
            if (r != Status::ok) {
                launchWorker();
                return r;
            }
            lilv_instance_free(instance_);
            instance_ = nullptr;
            for (auto& p : desc_.parameters)
                if (ports_[p.id].rateScaled) {
                    auto ratio = o.sampleRate / sampleRate_;
                    p.minimum *= ratio;
                    p.maximum *= ratio;
                    p.defaultValue *= ratio;
                }
            sampleRate_ = float(o.sampleRate);
            maxBlock_ = std::int32_t(o.maximumFrames);
            nominalBlock_ = maxBlock_;
            r = instantiate();
            if (r != Status::ok)
                return r;
            r = loadState(saved);
            if (r != Status::ok)
                return r;
        }
        options_ = o;
        auto r = input_.resize(totalInputs_ ? totalInputs_ : 1, o.maximumFrames);
        if (r != Status::ok)
            return r;
        r = output_.resize(totalOutputs_ ? totalOutputs_ : 1, o.maximumFrames);
        if (r != Status::ok)
            return r;
        for (auto& p : ports_)
            if (p.kind == Port::atom)
                p.data.resize(std::size_t(sequenceBytes_) / 8);
        // Output payload is bounded by what the MIDI output sequences can hold; a few
        // sub-blocks of headroom instead of a fixed 16 MiB per instance.
        std::size_t midiOutputs = 0;
        for (const auto i : atomPorts_)
            if (!ports_[i].input && ports_[i].midi)
                ++midiOutputs;
        events_.prepare(o.maximumEvents,
                        std::min<std::size_t>(16 * 1024 * 1024,
                                              midiOutputs * std::size_t(sequenceBytes_) * 4));
        for (std::size_t i = 0; i < ports_.size(); ++i) {
            auto& p = ports_[i];
            if (p.kind == Port::audio)
                lilv_instance_connect_port(instance_, std::uint32_t(i),
                                           p.input ? input_.block().channel(p.flat).data()
                                                   : output_.block().channel(p.flat).data());
            if (p.kind == Port::atom) {
                auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(p.data.data());
                seq->atom = {p.input ? 8u : std::uint32_t(p.data.size() * 8 - 8),
                             p.input ? sequence_ : chunk_};
                seq->body = {0, 0};
                lilv_instance_connect_port(instance_, std::uint32_t(i), seq);
            }
            if (p.kind == Port::control) {
                // One full UI refresh per activation; later only changes are sent.
                p.mirror.store(std::bit_cast<std::uint32_t>(p.value));
                p.dirty.store(1, std::memory_order_release);
            }
        }
        lilv_instance_activate(instance_);
        active_ = true;
        launchWorker();
        return Status::ok;
    } catch (...) {
        return Status::allocation_failed;
    }
}

Status Lv2Backend::saveState(std::vector<std::uint8_t>& out, std::size_t limit) noexcept {
    if (!instance_)
        return Status::not_prepared;
    try {
        auto get = [](const char* symbol, void* ctx, std::uint32_t* size,
                      std::uint32_t* type) -> const void* {
            auto& self = *static_cast<Lv2Backend*>(ctx);
            for (auto& p : self.ports_)
                if (p.kind == Port::control && p.input && p.symbol == symbol) {
                    *size = 4;
                    *type = self.float_;
                    return &p.value;
                }
            return nullptr;
        };
        LilvState* s = nullptr;
        {
            std::lock_guard lock(workMutex_);
            s = lilv_state_new_from_instance(
                plugin_, instance_, &urids_.map, nullptr, nullptr, nullptr, nullptr, get, this,
                LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, featurePointers_.data());
        }
        if (!s)
            return Status::io_error;
        char* data = lilv_state_to_string(world_.value, &urids_.map, &urids_.unmap, s,
                                          "urn:cydsp:state", nullptr);
        lilv_state_free(s);
        if (!data)
            return Status::io_error;
        auto n = std::strlen(data);
        if (n > limit) {
            lilv_free(data);
            return Status::block_too_large;
        }
        try {
            std::vector<std::uint8_t> next(data, data + n);
            lilv_free(data);
            out.swap(next);
            return Status::ok;
        } catch (...) {
            lilv_free(data);
            throw;
        }
    } catch (...) {
        return Status::allocation_failed;
    }
}
Status Lv2Backend::loadState(std::span<const std::uint8_t> data) noexcept {
    if (!instance_)
        return Status::not_prepared;
    try {
        if (std::find(data.begin(), data.end(), 0) != data.end())
            return Status::invalid_argument;
        std::string str(data.begin(), data.end());
        auto* state = lilv_state_new_from_string(world_.value, &urids_.map, str.c_str());
        if (!state)
            return Status::invalid_argument;
        auto* uri = lilv_state_get_plugin_uri(state);
        if (!uri || desc_.plugin.id != lilv_node_as_uri(uri)) {
            lilv_state_free(state);
            return Status::invalid_argument;
        }
        auto setPort = [](const char* symbol, void* ctx, const void* value, std::uint32_t size,
                          std::uint32_t type) {
            auto& self = *static_cast<Lv2Backend*>(ctx);
            if (type != self.float_ || size != 4)
                return;
            float v = 0;
            std::memcpy(&v, value, 4);
            for (auto& p : self.ports_)
                if (p.kind == Port::control && p.input && p.symbol == symbol) {
                    const auto* param = self.parameter(std::uint32_t(&p - self.ports_.data()));
                    if (isFinite(v) && v >= param->minimum && v <= param->maximum)
                        p.value = v;
                }
        };
        {
            std::lock_guard lock(workMutex_);
            lilv_state_restore(state, instance_, setPort, this, 0, featurePointers_.data());
        }
        lilv_state_free(state);
        return Status::ok;
    } catch (...) {
        return Status::allocation_failed;
    }
}
Status Lv2Backend::openEditor(const PluginWindow& window, PluginEditorSize& size) noexcept {
    if (ui_ || window.floating)
        return Status::unsupported_format;
    const char* type = window.api == PluginWindowApi::win32   ? LV2_UI__WindowsUI
                       : window.api == PluginWindowApi::cocoa ? LV2_UI__CocoaUI
                       : window.api == PluginWindowApi::x11   ? LV2_UI__X11UI
                                                              : nullptr;
    if (!type)
        return Status::unsupported_backend;
    auto* uis = lilv_plugin_get_uis(plugin_);
    try {
        Node klass(world_.value, type);
        Status result = Status::unsupported_format;
        LILV_FOREACH(uis, i, uis) {
            auto* ui = lilv_uis_get(uis, i);
            if (!lilv_ui_is_a(ui, klass))
                continue;
            auto library = std::make_unique<SharedModule>();
            auto r = library->open(pathFromUtf8(filePath(lilv_ui_get_binary_uri(ui))));
            if (r != Status::ok) {
                result = r;
                continue;
            }
            auto get =
                reinterpret_cast<LV2UI_DescriptorFunction>(library->symbol("lv2ui_descriptor"));
            if (!get)
                continue;
            const auto id = text(lilv_ui_get_uri(ui));
            for (std::uint32_t index = 0; index < 1024; ++index) {
                auto* d = get(index);
                if (!d)
                    break;
                if (!d->URI || id != d->URI || !d->instantiate || !d->cleanup)
                    continue;
                uiScale_ = float(window.scale);
                uiOptions_ = {
                    {{LV2_OPTIONS_INSTANCE, 0, urids_.get(LV2_UI__scaleFactor), 4, float_,
                      &uiScale_},
                     {LV2_OPTIONS_INSTANCE, 0, urids_.get(LV2_UI__updateRate), 4, float_, &uiRate_},
                     {}}};
                uiAccess_ = {lilv_instance_get_descriptor(instance_)->extension_data};
                uiFeatures_ = {{{LV2_UI__parent, reinterpret_cast<void*>(window.handle)},
                                {LV2_UI__resize, &hostResize_},
                                {LV2_INSTANCE_ACCESS_URI, lilv_instance_get_handle(instance_)},
                                {LV2_DATA_ACCESS_URI, &uiAccess_},
                                {LV2_URID__map, &urids_.map},
                                {LV2_URID__unmap, &urids_.unmap},
                                {LV2_UI__portMap, &uiPortMap_},
                                {LV2_UI__touch, &uiTouch_},
                                {LV2_OPTIONS__options, uiOptions_.data()}}};
                auto& fs = uiFeaturePointers_;
                fs.fill(nullptr);
                for (std::size_t j = 0; j < uiFeatures_.size(); ++j)
                    fs[j] = &uiFeatures_[j];
                Node requiredKey(world_.value, LV2_CORE__requiredFeature);
                auto* required =
                    lilv_world_find_nodes(world_.value, lilv_ui_get_uri(ui), requiredKey, nullptr);
                bool supported = true;
                LILV_FOREACH(nodes, k, required) {
                    const auto* uri = lilv_node_as_uri(lilv_nodes_get(required, k));
                    bool found = false;
                    for (const auto* feature : fs)
                        if (feature && uri && std::strcmp(feature->URI, uri) == 0)
                            found = true;
                    if (!found)
                        supported = false;
                }
                lilv_nodes_free(required);
                if (!supported)
                    continue;
                LV2UI_Widget widget = nullptr;
                ui_ = d->instantiate(d, desc_.plugin.id.c_str(), bundlePath_.c_str(), uiWrite, this,
                                     &widget, fs.data());
                if (!ui_)
                    continue;
                uiDescriptor_ = d;
                uiLibrary_ = std::move(library);
                uiIdle_ = d->extension_data ? static_cast<const LV2UI_Idle_Interface*>(
                                                  d->extension_data(LV2_UI__idleInterface))
                                            : nullptr;
                if (d->port_event)
                    for (std::size_t j = 0; j < ports_.size(); ++j)
                        if (ports_[j].kind == Port::control) {
                            auto value = active_ ? std::bit_cast<float>(ports_[j].mirror.load(
                                                       std::memory_order_acquire))
                                                 : ports_[j].value;
                            d->port_event(ui_, std::uint32_t(j), 4, 0, &value);
                        }
                size = editorSize_;
                lilv_uis_free(uis);
                return Status::ok;
            }
        }
        lilv_uis_free(uis);
        return result;
    } catch (...) {
        lilv_uis_free(uis);
        closeEditor();
        return Status::allocation_failed;
    }
}
Status Lv2Backend::resizeEditor(PluginEditorSize& size) noexcept {
    if (!ui_ || !uiDescriptor_->extension_data)
        return Status::unsupported_format;
    auto* r = static_cast<const LV2UI_Resize*>(uiDescriptor_->extension_data(LV2_UI__resize));
    if (!r || !r->ui_resize)
        return Status::unsupported_format;
    if (r->ui_resize(ui_, int(size.width), int(size.height)))
        return Status::invalid_argument;
    editorSize_ = size;
    return Status::ok;
}
Status Lv2Backend::scaleEditor(double scale) noexcept {
    if (!ui_ || !uiDescriptor_->extension_data)
        return Status::unsupported_format;
    const auto* options = static_cast<const LV2_Options_Interface*>(
        uiDescriptor_->extension_data(LV2_OPTIONS__interface));
    if (!options || !options->set)
        return Status::unsupported_format;
    auto old = uiScale_;
    uiScale_ = float(scale);
    auto result = options->set(ui_, uiOptions_.data());
    if (result) {
        uiScale_ = old;
        return Status::unsupported_format;
    }
    return Status::ok;
}
void Lv2Backend::closeEditor() noexcept {
    if (ui_) {
        uiDescriptor_->cleanup(ui_);
        ui_ = nullptr;
    }
    uiIdle_ = nullptr;
    uiDescriptor_ = nullptr;
    uiLibrary_.reset();
}
std::uint32_t Lv2Backend::pumpMainThread() noexcept {
    std::uint32_t flags = uiRequests_.exchange(0) | dropped_.take();
    if (ui_ && uiIdle_ && uiIdle_->idle && uiIdle_->idle(ui_)) {
        closeEditor(); // The flags gathered above are still delivered below.
        flags |= pluginEditorClosed;
    }
    if (ui_ && uiDescriptor_->port_event)
        for (std::size_t i = 0; i < ports_.size(); ++i) {
            auto& p = ports_[i];
            if (p.kind != Port::control || !p.dirty.exchange(0, std::memory_order_acq_rel))
                continue;
            auto v = std::bit_cast<float>(p.mirror.load(std::memory_order_acquire));
            uiDescriptor_->port_event(ui_, std::uint32_t(i), 4, 0, &v);
        }
    if (flags && callbacks_.requests)
        callbacks_.requests(callbacks_.context, flags);
    return flags;
}
class Lv2Provider final : public PluginProvider {
  public:
    Status scan(const std::filesystem::path& p,
                std::vector<PluginDescriptor>& out) noexcept override {
        try {
            World w;
            auto r = loadBundle(w, p);
            if (r != Status::ok)
                return r;
            std::vector<PluginDescriptor> next;
            auto* plugins = lilv_world_get_all_plugins(w.value);
            LILV_FOREACH(plugins, i, plugins)
            next.push_back(descriptor(lilv_plugins_get(plugins, i)));
            if (next.empty())
                return Status::unsupported_format;
            out.swap(next);
            return Status::ok;
        } catch (...) {
            return Status::io_error;
        }
    }
    Status create(const std::filesystem::path& p, std::string_view id,
                  const PluginHostCallbacks& cb, PluginInstance& out) noexcept override {
        try {
            auto plugin = std::make_unique<Lv2Backend>();
            auto r = plugin->initialize(p, id, cb);
            return r == Status::ok ? PluginInstance::adopt(std::move(plugin), out) : r;
        } catch (...) {
            return Status::allocation_failed;
        }
    }
};
} // namespace
std::unique_ptr<PluginProvider> makeLv2Provider() {
    return std::make_unique<Lv2Provider>();
}
} // namespace cyaudio::detail
