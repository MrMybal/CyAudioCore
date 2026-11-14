#include "detail/PortAudioApi.h"
#include "cyaudio/io/AudioDeviceManager.h"
#include <array>
#include <iostream>
#include <stdexcept>
void runRealtimeAudit(void (*)(void*) noexcept, void*);
namespace {
using namespace cyaudio;
using namespace cyaudio::detail;
void require(bool b,const char* why) { if (!b) throw std::runtime_error(why); }
void ok(Status r) { require(r == Status::ok,"simulated PortAudio status"); }
// No PortAudio implementation function is executed by these tests, including initialization.
struct Fake {
    static inline Fake* self = nullptr;
    int initializes = 0, terminates = 0, opens = 0, closes = 0, starts = 0, stops = 0, aborts = 0, forbidden = 0;
    bool inCallback = false, running = false, duplicate = false, reordered = false, planarOnlyFailure = false, openPlanarFailure = false;
    PaError initError = 0, openError = 0, startError = 0, stopError = 0, finishError = 0, enumerationError = 0;
    bool missingInfo = false;
    PaStreamCallback* callback = nullptr; PaStreamFinishedCallback* finishedCallback = nullptr; void* user = nullptr;
    PaSampleFormat format = 0; int inputs = 0, outputs = 0; bool exclusive = false;
    PaHostApiInfo host{1,paWASAPI,"Simulated WASAPI",1,0,0};
    PaHostApiInfo otherHost{1,paALSA,"Simulated ALSA",1,1,1};
    PaDeviceInfo device{2,"Simulated duplex",0,4,4,.01,.01,.1,.1,48000};
    PaDeviceInfo otherDevice{2,"Other host",1,2,2,.01,.01,.1,.1,48000};
    PaStreamInfo info{1,.012,.023,48000};
    Fake() { self = this; }
    static Fake& control() noexcept { auto& f = *self; if (f.inCallback) ++f.forbidden; return f; }
    static PaStream* handle() noexcept { return reinterpret_cast<PaStream*>(self); }
    PortAudioApi api() {
        PortAudioApi a;
        a.initialize = []() -> PaError { auto& f = control(); ++f.initializes; return f.initError; };
        a.terminate = []() -> PaError { ++control().terminates; return paNoError; };
        a.hostCount = []() { control(); return 2; };
        a.defaultHost = []() { control(); return 0; };
        a.hostInfo = [](PaHostApiIndex index) -> const PaHostApiInfo* {
            auto& f = control(); f.host.defaultInputDevice = f.host.defaultOutputDevice = f.reordered ? 1 : 0;
            f.otherHost.defaultInputDevice = f.otherHost.defaultOutputDevice = f.reordered ? 0 : 1;
            return index == 0 ? &f.host : index == 1 ? &f.otherHost : nullptr;
        };
        a.deviceCount = []() { auto& f = control(); return f.enumerationError ? f.enumerationError : f.duplicate ? 3 : 2; };
        a.deviceInfo = [](PaDeviceIndex index) -> const PaDeviceInfo* {
            auto& f = control(); if (index == 2 && f.duplicate) return &f.device;
            return index == (f.reordered ? 1 : 0) ? &f.device : index == (f.reordered ? 0 : 1) ? &f.otherDevice : nullptr;
        };
        a.formatSupported = [](const PaStreamParameters* in,const PaStreamParameters* out,double rate) -> PaError {
            auto& f = control(); if (rate != 48000) return paInvalidSampleRate;
            if ((in && in->device == paNoDevice) || (out && out->device == paNoDevice)) return paInvalidDevice;
            const auto format = out ? out->sampleFormat : in->sampleFormat;
            return f.planarOnlyFailure && (format & paNonInterleaved) ? paSampleFormatNotSupported : paFormatIsSupported;
        };
        a.open = [](PaStream** stream,const PaStreamParameters* in,const PaStreamParameters* out,double,
            unsigned long frames,PaStreamFlags,PaStreamCallback* cb,void* user) -> PaError {
            auto& f = control(); ++f.opens;
            if (frames != paFramesPerBufferUnspecified) ++f.forbidden;
            f.format = out ? out->sampleFormat : in->sampleFormat;
            f.inputs = in ? in->channelCount : 0; f.outputs = out ? out->channelCount : 0;
            f.exclusive = (in && in->hostApiSpecificStreamInfo) || (out && out->hostApiSpecificStreamInfo);
            if (f.openError || (f.openPlanarFailure && (f.format & paNonInterleaved))) {
                *stream = reinterpret_cast<PaStream*>(std::uintptr_t(1));
                return f.openError ? f.openError : int(paSampleFormatNotSupported);
            }
            f.callback = cb; f.user = user; *stream = handle(); return int(paNoError);
        };
        a.close = [](PaStream* stream) -> PaError { auto& f = control(); if (stream != handle()) ++f.forbidden; ++f.closes; f.running = false; return paNoError; };
        a.start = [](PaStream*) -> PaError { auto& f = control(); ++f.starts; f.running = f.startError == 0; return f.startError; };
        a.stop = [](PaStream*) -> PaError { auto& f = control(); ++f.stops; if (!f.stopError) f.running = false; return f.stopError; };
        a.abort = [](PaStream*) -> PaError { auto& f = control(); ++f.aborts; f.running = false; return paNoError; };
        a.active = [](PaStream*) { return control().running ? 1 : 0; };
        a.finished = [](PaStream*,PaStreamFinishedCallback* cb) -> PaError { auto& f = control(); f.finishedCallback = cb; return f.finishError; };
        a.streamInfo = [](PaStream*) -> const PaStreamInfo* { auto& f = control(); return f.missingInfo ? nullptr : &f.info; };
        return a;
    }
};
constexpr AudioContextOptions selected{AudioBackend::wasapi,AudioProvider::portAudio};
Status silence(void*,AudioBlock<const float>,AudioBlock<float>,std::uint64_t) noexcept { return Status::ok; }
void errors() {
    require(portAudioStatus(paInsufficientMemory) == Status::allocation_failed && portAudioStatus(paDeviceUnavailable) == Status::device_unavailable &&
        portAudioStatus(paHostApiNotFound) == Status::unsupported_backend && portAudioStatus(paInvalidSampleRate) == Status::invalid_spec &&
        portAudioStatus(paSampleFormatNotSupported) == Status::unsupported_format && portAudioStatus(paInvalidChannelCount) == Status::channel_mismatch &&
        portAudioStatus(paTimedOut) == Status::timed_out && portAudioStatus(paUnanticipatedHostError) == Status::io_error,"PortAudio error translation");
    Fake f; auto runtime = std::make_shared<PortAudioRuntime>(f.api()); std::shared_ptr<AudioProviderContext> context;
    f.initError = paInsufficientMemory;
    require(createPortAudioContext(selected,runtime,context) == Status::allocation_failed && !context && !runtime->leases,"failed initialization cleanup");
    f.initError = 0; ok(createPortAudioContext(selected,runtime,context));
    auto kept = context;
    require(createPortAudioContext({AudioBackend::jack,AudioProvider::portAudio},runtime,context) == Status::unsupported_backend && context == kept && runtime->leases == 1,"explicit missing host no fallback");
    require(createPortAudioContext({AudioBackend::nullDevice,AudioProvider::portAudio},runtime,context) == Status::unsupported_backend,"unsupported backend before init");
    std::unique_ptr<AudioProviderStream> stream; AudioDeviceConfig c;
    f.openError = paDeviceUnavailable;
    require(context->open(c,&silence,nullptr,nullptr,stream) == Status::device_unavailable && !stream && !f.closes && !runtime->streams,"failed open pointer never closed");
    f.openError = 0; f.finishError = paInternalError;
    require(context->open(c,&silence,nullptr,nullptr,stream) == Status::io_error && !stream && f.closes == 1 && !runtime->streams && runtime->leases == 1,"finished callback failure cleanup");
    f.finishError = 0; f.missingInfo = true;
    require(context->open(c,&silence,nullptr,nullptr,stream) == Status::io_error && f.closes == 2 && !runtime->streams,"stream info failure cleanup");
    f.missingInfo = false; ok(context->open(c,&silence,nullptr,nullptr,stream));
    f.startError = paDeviceUnavailable;
    require(stream->start() == Status::device_unavailable && !stream->running(),"start failure propagation");
    stream.reset(); require(f.closes == 3 && runtime->leases == 1,"start failure close");
    f.enumerationError = paUnanticipatedHostError; std::vector<AudioDeviceInfo> list;
    require(context->enumerate(list) == Status::io_error,"enumeration error propagation");
    context.reset(); kept.reset();
    require(!runtime->leases && !runtime->initialized && !f.forbidden && f.initializes == f.terminates + 1,"balanced successful initialization/termination");
}
void identities() {
    Fake f; auto runtime = std::make_shared<PortAudioRuntime>(f.api()); std::shared_ptr<AudioProviderContext> context;
    ok(createPortAudioContext(selected,runtime,context));
    std::vector<AudioDeviceInfo> list; ok(context->enumerate(list));
    require(list.size() == 2 && list[0].provider == AudioProvider::portAudio && !list[0].persistentIdentity && list[0].persistentId.empty() && list[0].id.starts_with("cyaudio.portaudio/1/"),"honest scoped identities/host filtering");
    const auto key = list[0].id;
    f.reordered = true; ok(context->enumerate(list)); require(list[0].id == key,"catalog reorder preserves scoped name key");
    AudioDeviceCapabilities caps; caps.bufferRangeKnown = true; ok(context->capabilities(key,false,caps));
    require(!caps.nativeFormatsKnown && !caps.duplexKnown && !caps.channelNamesKnown && !caps.bufferRangeKnown,"unknown hardware capabilities");
    AudioDeviceConfig c; c.outputId = key; std::unique_ptr<AudioProviderStream> stream;
    // Two devices with one name: a positional ordinal keeps each of them selectable.
    f.duplicate = true; ok(context->enumerate(list));
    require(list.size() == 4 && list[0].id == key && list[2].id == key + "/2" && !list[2].input && list[1].input && list[3].id == list[1].id + "/2",
        "identical names stay distinguishable");
    ok(context->open(c,&silence,nullptr,nullptr,stream)); stream.reset(); ok(context->capabilities(key,false,caps));
    c.outputId = list[2].id; ok(context->open(c,&silence,nullptr,nullptr,stream)); stream.reset(); ok(context->capabilities(c.outputId,false,caps));
    f.duplicate = false; ok(context->enumerate(list));
    require(list.size() == 2 && context->open(c,&silence,nullptr,nullptr,stream) == Status::device_unavailable,"ordinal leaves with its device");
    c.outputId = "context/0";
    require(context->open(c,&silence,nullptr,nullptr,stream) == Status::device_unavailable,"cross-provider identity rejected");
    c.outputId.clear(); c.outputChannels = 5;
    require(context->open(c,&silence,nullptr,nullptr,stream) == Status::channel_mismatch,"reported maximum client channels");
    c.outputChannels = 2; c.outputChannelMap = {1,2};
    require(context->open(c,&silence,nullptr,nullptr,stream) == Status::unsupported_backend,"speaker maps never guessed");
    c.outputChannelMap.clear(); c.access = AudioAccess::exclusive;
#if CYAUDIOCORE_PA_WASAPI
    // The main Windows validation recipe includes WASAPI.
    ok(context->open(c,&silence,nullptr,nullptr,stream)); require(f.exclusive,"exclusive host info passed"); stream.reset();
#endif
    std::shared_ptr<AudioProviderContext> other;
    ok(createPortAudioContext({AudioBackend::alsa,AudioProvider::portAudio},runtime,other));
    require(other->open(c,&silence,nullptr,nullptr,stream) == Status::unsupported_backend,"exclusive unsupported host rejected");
    other.reset(); context.reset(); require(!runtime->leases && f.initializes == f.terminates,"multiple context leases balanced");
}
void processing() {
    for (int fallback = 0; fallback < 3; ++fallback) {
        Fake f; f.planarOnlyFailure = fallback == 1; f.openPlanarFailure = fallback == 2;
        auto runtime = std::make_shared<PortAudioRuntime>(f.api()); std::shared_ptr<AudioProviderContext> context;
        ok(createPortAudioContext(selected,runtime,context));
        AudioDeviceConfig c; c.inputChannels = c.outputChannels = 1; c.activeInputChannels = {2}; c.activeOutputChannels = {3}; c.maxCallbackFrames = 3; c.generation = 5;
        struct Check { unsigned frames = 0, errors = 0; const float* input = nullptr; float* output = nullptr; bool direct = false; } check;
        check.direct = fallback == 0;
        std::unique_ptr<AudioProviderStream> stream;
        ok(context->open(c,nullptr,[](void* p,auto in,auto out,const AudioCallbackContext& timing) noexcept {
            auto& s = *static_cast<Check*>(p);
            if (out.frames() > 3 || timing.generation != 5 || timing.clock.firstFrame != s.frames || timing.nativeCallbackFrames != 11 || !timing.adcTime.valid || !timing.dacTime.valid || timing.dacTime.source != AudioMeasurementSource::backendClockEstimate) ++s.errors;
            if (s.direct && (in.channel(0).data() != s.input + s.frames || out.channel(0).data() != s.output + s.frames)) ++s.errors;
            for (std::size_t i = 0; i < out.frames(); ++i) out.channel(0)[i] = in.channel(0)[i] + 1;
            s.frames += unsigned(out.frames()); return Status::ok;
        },&check,stream));
        require(f.inputs == 3 && f.outputs == 4 && bool(f.format & paNonInterleaved) == (fallback == 0),"native layout/channel negotiation");
        AudioStreamDescription info; ok(stream->description(info));
        require(info.provider == AudioProvider::portAudio && info.backend == AudioBackend::wasapi && info.generation == 5 && info.output.latency.valid && info.output.latency.source == AudioMeasurementSource::backendDeclared && info.output.latency.nanoseconds == 23000000 && !info.output.periodKnown && !info.output.nativeSampleRate && !info.output.nativeChannels,"stream latency and unknown hardware facts");
        ok(stream->start()); require(stream->running() && (stream->takeNotifications() & 1),"start notifications");
        ok(stream->start()); require(f.starts == 1,"start is idempotent like miniaudio");
        std::array<std::array<float,11>,4> inputs{},outputs{};
        std::array<const float*,4> ip{}; std::array<float*,4> op{};
        std::array<float,33> interleavedInput{}; std::array<float,44> interleavedOutput{};
        for (unsigned ch = 0; ch < 4; ++ch) { ip[ch] = inputs[ch].data(); op[ch] = outputs[ch].data(); outputs[ch].fill(-1); inputs[ch].fill(float(ch)); }
        for (unsigned i = 0; i < 11; ++i) for (unsigned ch = 0; ch < 3; ++ch) interleavedInput[i*3+ch] = float(ch);
        interleavedOutput.fill(-1); check.input = ip[2]; check.output = op[3];
        struct Job { Fake* f; const void* in; void* out; } job{&f,fallback == 0 ? static_cast<const void*>(ip.data()) : interleavedInput.data(),fallback == 0 ? static_cast<void*>(op.data()) : interleavedOutput.data()};
        runRealtimeAudit([](void* p) noexcept {
            auto& j = *static_cast<Job*>(p); PaStreamCallbackTimeInfo time{1000000-.001,1000000,1000000+.01};
            j.f->inCallback = true; (void)j.f->callback(j.in,j.out,11,&time,paOutputUnderflow,j.f->user); j.f->inCallback = false;
        },&job);
        require(check.frames == 11 && !check.errors && !f.forbidden,"PortAudio callback bridge clock/views/realtime/API restrictions");
        for (unsigned i = 0; i < 11; ++i) for (unsigned ch = 0; ch < 4; ++ch)
            require((fallback == 0 ? outputs[ch][i] : interleavedOutput[i*4+ch]) == (ch == 3 ? 3.0f : 0.0f),"PortAudio complete mapped output");
        require(stream->diagnostics().deviceXruns == 1 && stream->diagnostics().deviceXrunsKnown,"PortAudio xrun flags");
        const auto initializes = f.initializes; std::vector<AudioDeviceInfo> list; ok(context->enumerate(list));
        require(f.initializes == initializes,"enumeration never reinitializes live stream");
        f.running = false; f.inCallback = true; f.finishedCallback(f.user); f.inCallback = false;
        require(!stream->running() && (stream->takeNotifications() & 2),"backend disconnect notification");
        f.stopError = paUnanticipatedHostError;
        require(stream->stop() == Status::io_error && f.aborts == 1,"failed stop aborts on control");
        stream.reset(); context.reset();
        require(f.closes == 1 && f.initializes == f.terminates && !runtime->leases && !runtime->streams,"stream/context teardown");
    }
}
// A catalogue refresh reinitializes every host API: at most one per interval, however
// often the owner scans.
void refreshLimit() {
    Fake f; auto runtime = std::make_shared<PortAudioRuntime>(f.api());
    static std::uint64_t ticks; ticks = 1000;
    runtime->clock = []() noexcept { return ticks; }; runtime->refreshIntervalNs = 1000;
    std::shared_ptr<AudioProviderContext> context; ok(createPortAudioContext(selected,runtime,context));
    std::vector<AudioDeviceInfo> list; const auto initial = f.initializes;
    for (int i = 0; i < 5; ++i) { ticks += 100; ok(context->enumerate(list)); }
    require(f.initializes == initial && list.size() == 2,"scans inside the interval answer from the snapshot");
    ticks += 600; ok(context->enumerate(list)); require(f.initializes == initial + 1,"one refresh once the interval elapsed");
    ticks += 999; ok(context->enumerate(list)); require(f.initializes == initial + 1,"interval restarts at the refresh");
    ticks += 1; ok(context->enumerate(list)); require(f.initializes == initial + 2,"next interval");
    f.initError = paInsufficientMemory; ticks += 5000;
    require(context->enumerate(list) == Status::allocation_failed && !runtime->initialized,"failed refresh reported");
    f.initError = 0; ok(context->enumerate(list)); require(runtime->initialized && list.size() == 2,"recovery is not rate limited");
    context.reset(); require(!runtime->leases && f.initializes == f.terminates + 1 && !f.forbidden,"refresh lifetime balance");
}
void manager() {
    Fake f; auto runtime = std::make_shared<PortAudioRuntime>(f.api());
    runtime->refreshIntervalNs = 0; // This scenario reconnects immediately in virtual time.
    struct Driver {
        std::shared_ptr<AudioProviderContext> context;
        std::unique_ptr<AudioProviderStream> stream;
    } owner;
    ok(createPortAudioContext(selected,runtime,owner.context));
    AudioDeviceDriver d; d.context = &owner;
    d.enumerate = [](void* p,auto& list) noexcept { return static_cast<Driver*>(p)->context->enumerate(list); };
    d.open = [](void* p,const auto& c,AudioDeviceCallback cb,void* user) noexcept {
        auto& s = *static_cast<Driver*>(p); return s.context->open(c,cb,nullptr,user,s.stream);
    };
    d.openWithContext = [](void* p,const auto& c,AudioDeviceContextCallback cb,void* user) noexcept {
        auto& s = *static_cast<Driver*>(p); return s.context->open(c,nullptr,cb,user,s.stream);
    };
    d.start = [](void* p) noexcept { return static_cast<Driver*>(p)->stream->start(); };
    d.close = [](void* p) noexcept { static_cast<Driver*>(p)->stream.reset(); };
    d.running = [](void* p) noexcept { return static_cast<Driver*>(p)->stream->running(); };
    d.sampleRate = [](void* p) noexcept { return static_cast<Driver*>(p)->stream->sampleRate(); };
    d.notifications = [](void* p) noexcept { return static_cast<Driver*>(p)->stream->takeNotifications(); };
    AudioDeviceManager manager; ok(manager.prepare(d)); unsigned generation = 0;
    ok(manager.openWithContext({},[](void* p,auto,auto,const AudioCallbackContext& time) noexcept {
        *static_cast<unsigned*>(p) = time.generation; return Status::ok;
    },&generation));
    float samples[2][5]{}; float* output[] = {samples[0],samples[1]};
    PaStreamCallbackTimeInfo time{0,123456,123456.01};
    (void)f.callback(nullptr,output,5,&time,0,f.user); require(generation == 1,"manager native generation");
    f.running = false; f.finishedCallback(f.user);
    require(manager.poll(300) == Status::device_unavailable && f.closes == 1 && !runtime->streams,"manager closes finished PortAudio stream on control");
    const auto initialized = f.initializes;
    ok(manager.poll(1200));
    require(manager.generation() == 2 && f.initializes == initialized + 1 && runtime->streams == 1,"manager refreshes catalogue and reconnects");
    (void)f.callback(nullptr,output,5,&time,0,f.user); require(generation == 2,"reconnected callback generation");
    f.running = false; f.startError = paTimedOut;
    require(manager.poll(2400) == Status::timed_out && !runtime->streams && runtime->leases == 1 && !manager.running(),"manager cleans failed restart");
    manager.close(); owner.context.reset();
    require(!runtime->leases && f.initializes == f.terminates && !f.forbidden,"manager provider lifetime balance");
}
}
void portAudioRegression() { errors(); identities(); processing(); refreshLimit(); manager(); std::cout << "PASS simulated PortAudio lifecycle/errors/identity/layout/clocks/realtime/reconnect\n"; }
