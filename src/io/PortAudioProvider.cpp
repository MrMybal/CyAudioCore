// Original adapter; unmodified private runtime: third_party/licenses/portaudio.txt
#include "detail/PortAudioApi.h"
#include "detail/AudioCallbackAdapter.h"
#include <algorithm>
#include <cmath>
#if CYAUDIOCORE_PA_WASAPI
#include <pa_win_wasapi.h>
#endif
namespace cyaudio::detail {
Status portAudioStatus(PaError r) noexcept {
    switch (r) {
    case paNoError: return Status::ok;
    case paNotInitialized: return Status::not_prepared;
    case paInsufficientMemory: return Status::allocation_failed;
    case paInvalidChannelCount: return Status::channel_mismatch;
    case paInvalidSampleRate: return Status::invalid_spec;
    case paSampleFormatNotSupported: return Status::unsupported_format;
    case paHostApiNotFound: case paInvalidHostApi: return Status::unsupported_backend;
    case paDeviceUnavailable: case paInvalidDevice: return Status::device_unavailable;
    case paTimedOut: return Status::timed_out;
    case paStreamIsStopped: case paStreamIsNotStopped: return Status::transition_in_progress;
    case paBadIODeviceCombination: case paIncompatibleHostApiSpecificStreamInfo: return Status::unsupported_backend;
    case paBadStreamPtr: case paBadBufferPtr: case paNullCallback: case paInvalidFlag:
    case paBufferTooBig: case paBufferTooSmall: return Status::invalid_argument;
    default: return Status::io_error;
    }
}
Status PortAudioRuntime::acquire() noexcept {
    // Caller holds gate.
    if (!initialized) {
        const auto r = portAudioStatus(api.initialize());
        if (r != Status::ok) return r;
        initialized = true;
    }
    ++leases; return Status::ok;
}
void PortAudioRuntime::release() noexcept {
    if (leases && --leases == 0 && initialized) { (void)api.terminate(); initialized = false; }
}
Status PortAudioRuntime::refresh() noexcept {
    // PortAudio's catalogue is an initialization-time snapshot. Never invalidate live streams.
    if (streams) return Status::ok;
    if (initialized) { const auto r = portAudioStatus(api.terminate()); initialized = false; if (r != Status::ok) return r; }
    const auto r = portAudioStatus(api.initialize()); initialized = r == Status::ok; return r;
}
namespace {
PaHostApiTypeId nativeHost(AudioBackend b) noexcept {
    switch (b) {
    case AudioBackend::wasapi: return paWASAPI;
    case AudioBackend::winmm: return paMME;
    case AudioBackend::directSound: return paDirectSound;
    case AudioBackend::coreAudio: return paCoreAudio;
    case AudioBackend::alsa: return paALSA;
    case AudioBackend::jack: return paJACK;
    default: return paInDevelopment;
    }
}
AudioBackend publicHost(PaHostApiTypeId type) noexcept {
    for (auto b : {AudioBackend::wasapi,AudioBackend::winmm,AudioBackend::directSound,AudioBackend::coreAudio,AudioBackend::alsa,AudioBackend::jack})
        if (nativeHost(b) == type) return b;
    return AudioBackend::custom;
}
class PortAudioStream final : public AudioProviderStream {
  public:
    std::shared_ptr<PortAudioRuntime> runtime;
    PaStream* stream = nullptr;
    AudioCallbackAdapter adapter;
    AudioStreamDescription info;
    bool planar = true;
    std::atomic<std::uint32_t> notifications{0};
    ~PortAudioStream() override {
        if (!stream) return;
        std::lock_guard lock(runtime->gate);
        // All API calls and waits are on the control owner. Close joins the native callback.
        if (runtime->api.active(stream) > 0) (void)runtime->api.abort(stream);
        (void)runtime->api.close(stream);
        --runtime->streams; runtime->release();
    }
    static int render(const void* in, void* out, unsigned long frames,
        const PaStreamCallbackTimeInfo* time, PaStreamCallbackFlags flags, void* user) noexcept {
        auto& s = *static_cast<PortAudioStream*>(user);
        const auto entry = audioSteadyNow();
        AudioNativeTimes times;
        if (time) {
            times.current = time->currentTime; times.adc = time->inputBufferAdcTime; times.dac = time->outputBufferDacTime;
            // Several APIs use zero for unavailable timestamps. Never invent an ADC/DAC measurement.
            times.adcKnown = time->currentTime != 0 && time->inputBufferAdcTime != 0 && !(flags & paInputUnderflow);
            times.dacKnown = time->currentTime != 0 && time->outputBufferDacTime != 0;
        }
        s.adapter.render(in,out,frames,s.planar,entry,times,
            (flags & (paInputUnderflow|paInputOverflow|paOutputUnderflow|paOutputOverflow)) != 0);
        return paContinue;
    }
    static void finished(void* user) noexcept {
        static_cast<PortAudioStream*>(user)->notifications.fetch_or(2u, std::memory_order_relaxed);
    }
    Status description(AudioStreamDescription& out) const noexcept override {
        try { auto next = info; out = std::move(next); return Status::ok; } catch (...) { return Status::allocation_failed; }
    }
    AudioCallbackDiagnostics diagnostics() const noexcept override { return adapter.diagnostics(true); }
    Status start() noexcept override {
        std::lock_guard lock(runtime->gate);
        const auto active = runtime->api.active(stream);
        if (active == 1) return Status::ok;
        if (active < 0) return portAudioStatus(active);
        auto r = portAudioStatus(runtime->api.start(stream));
        if (r == Status::ok) notifications.fetch_or(1u, std::memory_order_relaxed);
        return r;
    }
    Status stop() noexcept override {
        std::lock_guard lock(runtime->gate);
        const auto r = runtime->api.stop(stream);
        if (r == paStreamIsStopped) return Status::ok;
        if (r != paNoError) (void)runtime->api.abort(stream);
        return portAudioStatus(r);
    }
    bool running() const noexcept override { std::lock_guard lock(runtime->gate); return runtime->api.active(stream) == 1; }
    std::uint32_t sampleRate() const noexcept override { return info.sampleRate; }
    std::uint32_t takeNotifications() noexcept override { return notifications.exchange(0, std::memory_order_relaxed); }
};
class PortAudioContext final : public AudioProviderContext {
  public:
    struct Record { PaDeviceIndex index; std::string key; bool input; };
    std::shared_ptr<PortAudioRuntime> runtime;
    std::vector<Record> records;
    PaHostApiTypeId type = paInDevelopment;
    std::uint32_t session = 0;
    bool leased = false;
    explicit PortAudioContext(std::shared_ptr<PortAudioRuntime> r) : runtime(std::move(r)) {
        static std::atomic<std::uint32_t> sessions{0}; session = ++sessions;
    }
    ~PortAudioContext() override { if (leased) { std::lock_guard lock(runtime->gate); runtime->release(); } }
    Status host(PaHostApiIndex& index) const noexcept {
        if (!runtime->initialized) return Status::device_unavailable;
        const auto count = runtime->api.hostCount(); if (count < 0) return portAudioStatus(count);
        for (int i = 0; i < count; ++i) { const auto* h = runtime->api.hostInfo(i); if (h && h->type == type) { index = i; return Status::ok; } }
        return Status::unsupported_backend;
    }
    AudioBackend backend() const noexcept override { return publicHost(type); }
    std::string backendName() const override {
        std::lock_guard lock(runtime->gate); PaHostApiIndex h;
        if (host(h) != Status::ok) return {};
        const auto* value = runtime->api.hostInfo(h); return value && value->name ? value->name : "";
    }
    Status enumerate(std::vector<AudioDeviceInfo>& out) noexcept override {
        std::lock_guard lock(runtime->gate);
        const auto r = runtime->refresh(); if (r != Status::ok) return r;
        return catalogue(out);
    }
    Status catalogue(std::vector<AudioDeviceInfo>& out) noexcept {
        try {
            PaHostApiIndex h; auto r = host(h); if (r != Status::ok) return r;
            const auto* api = runtime->api.hostInfo(h); if (!api) return Status::io_error;
            const auto count = runtime->api.deviceCount(); if (count < 0) return portAudioStatus(count);
            std::vector<AudioDeviceInfo> next; std::vector<Record> nextRecords;
            for (int i = 0; i < count; ++i) {
                const auto* device = runtime->api.deviceInfo(i); if (!device) return Status::io_error;
                if (device->hostApi != h) continue;
                for (bool input : {false,true}) {
                    if ((input ? device->maxInputChannels : device->maxOutputChannels) <= 0) continue;
                    const std::string name = device->name ? device->name : "";
                    // A scoped name token, never a supposedly persistent enumeration index.
                    std::string key = "cyaudio.portaudio/1/" + std::to_string(session) + "/" + std::to_string(int(type)) + (input ? "/input/" : "/output/");
                    constexpr char hex[] = "0123456789abcdef";
                    for (unsigned char c : name) { key += hex[c >> 4]; key += hex[c & 15]; }
                    next.push_back({key,name,input,i == (input ? api->defaultInputDevice : api->defaultOutputDevice),{},backend(),false,AudioProvider::portAudio});
                    nextRecords.push_back({i,key,input});
                }
            }
            records.swap(nextRecords); out.swap(next); return Status::ok;
        } catch (...) { return Status::allocation_failed; }
    }
    Status select(std::string_view key, bool input, PaDeviceIndex& index) const noexcept {
        PaHostApiIndex h; auto r = host(h); if (r != Status::ok) return r;
        if (key.empty()) {
            const auto* api = runtime->api.hostInfo(h); if (!api) return Status::io_error;
            index = input ? api->defaultInputDevice : api->defaultOutputDevice;
            return index == paNoDevice ? Status::device_unavailable : Status::ok;
        }
        unsigned matches = 0;
        for (const auto& item : records) if (item.key == key && item.input == input) { index = item.index; ++matches; }
        return matches > 1 ? Status::ambiguous : matches ? Status::ok : Status::device_unavailable;
    }
    Status capabilities(std::string_view id, bool input, AudioDeviceCapabilities& out) noexcept override {
        std::lock_guard lock(runtime->gate);
        std::vector<AudioDeviceInfo> list; auto r = catalogue(list); if (r != Status::ok) return r;
        PaDeviceIndex index; r = select(id,input,index); if (r != Status::ok) return r;
        const auto* d = runtime->api.deviceInfo(index);
        if (!d || (input ? d->maxInputChannels : d->maxOutputChannels) <= 0) return Status::device_unavailable;
        out = {}; // Maximum client channels/default rate are not native format or duplex guarantees.
        return Status::ok;
    }
    Status open(const AudioDeviceConfig& c, AudioDeviceCallback cb, AudioDeviceContextCallback ext,
                void* user, std::unique_ptr<AudioProviderStream>& out) noexcept override {
        const auto valid = validateAudioConfig(c,cb,ext); if (valid != Status::ok) return valid;
        // Portable PortAudio has no speaker-position map contract. Active channel indices are supported.
        if (!c.inputChannelMap.empty() || !c.outputChannelMap.empty()) return Status::unsupported_backend;
        if (c.access == AudioAccess::exclusive && (type != paWASAPI || !CYAUDIOCORE_PA_WASAPI)) return Status::unsupported_backend;
        std::lock_guard lock(runtime->gate);
        try {
            auto r = runtime->initialized ? Status::ok : runtime->refresh(); if (r != Status::ok) return r;
            std::vector<AudioDeviceInfo> list; r = catalogue(list); if (r != Status::ok) return r;
            auto next = std::make_unique<PortAudioStream>(); next->runtime = runtime;
            PaStreamParameters input{}, output{};
#if CYAUDIOCORE_PA_WASAPI
            PaWasapiStreamInfo wasapi{}; wasapi.size = sizeof(wasapi); wasapi.hostApiType = paWASAPI; wasapi.version = 1; wasapi.flags = paWinWasapiExclusive;
#endif
            auto direction = [&](PaStreamParameters& p, bool in) -> Status {
                const auto channels = in ? c.inputChannels : c.outputChannels; if (!channels) return Status::ok;
                auto status = select(in ? c.inputId : c.outputId,in,p.device); if (status != Status::ok) return status;
                const auto& active = in ? c.activeInputChannels : c.activeOutputChannels;
                p.channelCount = int(active.empty() ? channels : *std::max_element(active.begin(),active.end()) + 1);
                const auto* device = runtime->api.deviceInfo(p.device);
                PaHostApiIndex h; status = host(h); if (status != Status::ok) return status;
                if (!device || device->hostApi != h) return Status::device_unavailable;
                if (p.channelCount > (in ? device->maxInputChannels : device->maxOutputChannels)) return Status::channel_mismatch;
                p.sampleFormat = paFloat32 | paNonInterleaved; p.suggestedLatency = double(c.periodFrames) / c.sampleRate;
#if CYAUDIOCORE_PA_WASAPI
                if (c.access == AudioAccess::exclusive) p.hostApiSpecificStreamInfo = &wasapi;
#endif
                return Status::ok;
            };
            r = direction(input,true); if (r != Status::ok) return r;
            r = direction(output,false); if (r != Status::ok) return r;
            auto* ip = c.inputChannels ? &input : nullptr; auto* op = c.outputChannels ? &output : nullptr;
            auto format = runtime->api.formatSupported(ip,op,c.sampleRate);
            if (format == paSampleFormatNotSupported) {
                next->planar = false; input.sampleFormat = output.sampleFormat = paFloat32;
                format = runtime->api.formatSupported(ip,op,c.sampleRate);
            }
            if (format != paFormatIsSupported) return portAudioStatus(format);
            r = next->adapter.prepare(c,cb,ext,user,std::size_t(input.channelCount),std::size_t(output.channelCount)); if (r != Status::ok) return r;
            PaStream* opened = nullptr;
            auto error = runtime->api.open(&opened,ip,op,c.sampleRate,paFramesPerBufferUnspecified,paNoFlag,&PortAudioStream::render,next.get());
            if (error == paSampleFormatNotSupported && next->planar) {
                next->planar = false; input.sampleFormat = output.sampleFormat = paFloat32;
                error = runtime->api.open(&opened,ip,op,c.sampleRate,paFramesPerBufferUnspecified,paNoFlag,&PortAudioStream::render,next.get());
            }
            // PortAudio specifies that the returned pointer is invalid on failure.
            if (error != paNoError) return portAudioStatus(error);
            next->stream = opened; ++runtime->streams; ++runtime->leases;
            r = portAudioStatus(runtime->api.finished(opened,&PortAudioStream::finished)); if (r != Status::ok) return r;
            const auto* info = runtime->api.streamInfo(opened);
            if (!info || !isFinite(info->sampleRate) || info->sampleRate < 1 || info->sampleRate > 768000) return Status::io_error;
            auto actual = c; actual.sampleRate = static_cast<std::uint32_t>(std::llround(info->sampleRate));
            r = next->adapter.prepare(actual,cb,ext,user,std::size_t(input.channelCount),std::size_t(output.channelCount)); if (r != Status::ok) return r;
            next->info.provider = AudioProvider::portAudio; next->info.backend = backend(); next->info.access = c.access;
            next->info.sampleRate = actual.sampleRate; next->info.generation = c.generation;
            auto fill = [](AudioStreamDirection& d, std::size_t channels, const auto& active, double latency) {
                d.channels = channels; d.activeChannels = active;
                if (channels && isFinite(latency) && latency >= 0 && latency <= double(UINT64_MAX) / 1e9)
                    d.latency = {static_cast<std::uint64_t>(latency * 1e9),AudioMeasurementSource::backendDeclared,true};
                // Native hardware channel counts, rates and periods are not supplied by this API.
            };
            fill(next->info.input,c.inputChannels,c.activeInputChannels,info->inputLatency);
            fill(next->info.output,c.outputChannels,c.activeOutputChannels,info->outputLatency);
            out = std::move(next); return Status::ok;
        } catch (...) { return Status::allocation_failed; }
    }
};
}
Status createPortAudioContext(const AudioContextOptions& o, const std::shared_ptr<PortAudioRuntime>& runtime,
    std::shared_ptr<AudioProviderContext>& out) noexcept {
    if (o.provider != AudioProvider::portAudio || (o.backend != AudioBackend::automatic && nativeHost(o.backend) == paInDevelopment)) return Status::unsupported_backend;
    try {
        std::lock_guard lock(runtime->gate);
        auto next = std::make_shared<PortAudioContext>(runtime);
        auto r = runtime->acquire(); if (r != Status::ok) return r; next->leased = true;
        if (o.backend == AudioBackend::automatic) {
            const auto index = runtime->api.defaultHost(); if (index < 0) return portAudioStatus(index);
            const auto* h = runtime->api.hostInfo(index); if (!h || publicHost(h->type) == AudioBackend::custom) return Status::unsupported_backend;
            next->type = h->type;
        } else next->type = nativeHost(o.backend);
        PaHostApiIndex h; r = next->host(h); if (r != Status::ok) return r;
        out = std::move(next); return Status::ok;
    } catch (...) { return Status::allocation_failed; }
}
Status createPortAudioContext(const AudioContextOptions& o, std::shared_ptr<AudioProviderContext>& out) noexcept {
    try {
        static auto runtime = std::make_shared<PortAudioRuntime>();
        return createPortAudioContext(o,runtime,out);
    } catch (...) { return Status::allocation_failed; }
}
} // namespace cyaudio::detail
