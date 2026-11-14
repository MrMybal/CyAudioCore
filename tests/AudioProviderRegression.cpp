#include "detail/AudioCallbackAdapter.h"
#include "detail/AudioProvider.h"
#include "cyaudio/io/AudioDeviceManager.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>
#include <limits>
#include <stdexcept>
void runRealtimeAudit(void (*)(void*) noexcept, void*);
namespace {
using namespace cyaudio;
using namespace cyaudio::detail;
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
void ok(Status r) { require(r == Status::ok,"audio provider status"); }
struct RenderCheck {
    std::uint64_t frame = 0;
    unsigned calls = 0, errors = 0;
    bool planar = false, absent = false, fail = false;
    const float* directInput = nullptr;
    float* directOutput = nullptr;
    static Status callback(void* ptr, AudioBlock<const float> in, AudioBlock<float> out, const AudioCallbackContext& c) noexcept {
        auto& s = *static_cast<RenderCheck*>(ptr);
        if (in.channels() != 2 || out.channels() != 2 || out.frames() > 4 || in.frames() != out.frames() ||
            !c.clockValid || c.clock.firstFrame != s.frame || c.clock.sampleRate != 48000 || c.generation != 71 ||
            !c.nativeCallbackSizeKnown || c.nativeCallbackFrames != 13 || !c.callbackTime.valid) ++s.errors;
        const auto expectedTime = 4000000000ull + static_cast<std::uint64_t>(double(s.frame) * 1e9 / 48000);
        if (c.clock.monotonicNanoseconds != expectedTime) ++s.errors;
        if (!s.absent && (!c.adcTime.valid || c.adcTime.source != AudioMeasurementSource::backendClockEstimate ||
            std::abs(double(c.adcTime.nanoseconds) - double(expectedTime - 1000000)) > 2)) ++s.errors;
        if (s.absent && c.adcTime.valid) ++s.errors;
        if (!c.dacTime.valid || std::abs(double(c.dacTime.nanoseconds) - double(expectedTime + 10000000)) > 2) ++s.errors;
        if (s.planar && !s.absent && in.channel(0).data() != s.directInput + s.frame) ++s.errors;
        if (s.planar && out.channel(0).data() != s.directOutput + s.frame) ++s.errors;
        for (std::size_t i = 0; i < out.frames(); ++i) {
            const auto a = s.absent ? 0.0f : float(200 + s.frame + i);
            const auto b = s.absent ? 0.0f : float(s.frame + i);
            if (in.channel(0)[i] != a || in.channel(1)[i] != b) ++s.errors;
            out.channel(0)[i] = in.channel(0)[i] + 1;
            out.channel(1)[i] = in.channel(1)[i] + 2;
        }
        s.frame += out.frames(); ++s.calls;
        return s.fail && s.calls == 2 ? Status::io_error : Status::ok;
    }
};
void buffers() {
    for (bool planar : {false,true}) for (bool absent : {false,true}) for (bool fail : {false,true}) {
        std::array<std::array<float,13>,4> input{}, output{};
        std::array<float,52> interleavedInput{}, interleavedOutput{};
        std::array<const float*,4> inputs{}; std::array<float*,4> outputs{};
        for (std::size_t ch = 0; ch < 4; ++ch) {
            inputs[ch] = input[ch].data(); outputs[ch] = output[ch].data();
            output[ch].fill(-9);
            for (std::size_t i = 0; i < 13; ++i) input[ch][i] = interleavedInput[i * 4 + ch] = float(ch * 100 + i);
        }
        interleavedOutput.fill(-9);
        RenderCheck check; check.planar = planar; check.absent = absent; check.fail = fail;
        check.directInput = inputs[2]; check.directOutput = outputs[3];
        AudioDeviceConfig c; c.inputChannels = c.outputChannels = 2; c.maxCallbackFrames = 4; c.generation = 71;
        c.activeInputChannels = {2,0}; c.activeOutputChannels = {3,1};
        AudioCallbackAdapter adapter; ok(adapter.prepare(c,nullptr,&RenderCheck::callback,&check,4,4));
        struct Job { AudioCallbackAdapter* adapter; const void* in; void* out; bool planar; } job{
            &adapter,absent ? nullptr : planar ? static_cast<const void*>(inputs.data()) : interleavedInput.data(),
            planar ? static_cast<void*>(outputs.data()) : interleavedOutput.data(),planar};
        runRealtimeAudit([](void* p) noexcept {
            auto& j = *static_cast<Job*>(p);
            j.adapter->render(j.in,j.out,13,j.planar,4000000000ull,{9000,8999.999,9000.01,true,true},true);
        },&job);
        require(!check.errors && check.calls == 4 && check.frame == 13,"callback slicing/views/timing");
        for (std::size_t ch = 0; ch < 4; ++ch) for (std::size_t i = 0; i < 13; ++i) {
            const auto value = planar ? output[ch][i] : interleavedOutput[i * 4 + ch];
            const bool silenced = fail && i >= 4 && i < 8;
            const auto expected = silenced ? 0.0f : ch == 3 ? (absent ? 1.0f : float(201+i)) : ch == 1 ? (absent ? 2.0f : float(2+i)) : 0.0f;
            require(value == expected,"complete output/mapped channels/error silence");
        }
        const auto d = adapter.diagnostics(true);
        require(d.callbacks == 1 && d.callbackErrors == (fail ? 1u : 0u) && d.deviceXruns == 1 && d.deviceXrunsKnown,"xrun versus callback error diagnostics");
    }
    // A missing plane and missing output use retained scratch; zero-channel directions are valid.
    AudioDeviceConfig c; c.inputChannels = 1; c.outputChannels = 0; c.maxCallbackFrames = 3;
    unsigned count = 0; AudioCallbackAdapter adapter;
    ok(adapter.prepare(c,[](void* p,AudioBlock<const float> in,AudioBlock<float> out,std::uint64_t first) noexcept {
        auto& count = *static_cast<unsigned*>(p);
        if (first != count || out.channels() || !out.isValid()) return Status::invalid_argument;
        for (auto sample : in.channel(0)) if (sample != 0) return Status::io_error;
        count += unsigned(in.frames()); return Status::ok;
    },nullptr,&count,1,0));
    const float* missing[] = {nullptr};
    adapter.render(missing,nullptr,0,true,0); adapter.render(missing,nullptr,17,true,0);
    require(count == 17 && !adapter.diagnostics().callbackErrors,"variable size/missing planes/input-only");
    c.inputChannels = 0; c.outputChannels = 1;
    ok(adapter.prepare(c,[](void*,auto in,auto out,auto) noexcept {
        return in.channels() == 0 && out.isValid() ? Status::ok : Status::invalid_argument;
    },nullptr,nullptr,0,1));
    adapter.render(nullptr,nullptr,7,true,0);
    require(!adapter.diagnostics().callbackErrors,"absent output retained buffer");
    c.inputChannels = c.outputChannels = 1;
    unsigned unknown = 0;
    ok(adapter.prepare(c,nullptr,[](void* p,auto,auto,const AudioCallbackContext& time) noexcept {
        if (time.clockValid && !time.adcTime.valid && !time.dacTime.valid) ++*static_cast<unsigned*>(p);
        return Status::ok;
    },&unknown,1,1));
    float samples[2]{}; float* plane[] = {samples};
    adapter.render(plane,plane,1,true,1000,{});
    adapter.render(plane,plane,1,true,1000,{std::numeric_limits<double>::quiet_NaN(),1,2,true,true});
    adapter.render(plane,plane,1,true,1000,{1,std::numeric_limits<double>::infinity(),-1e300,true,true});
    require(unknown == 3,"missing/nonfinite/out-of-domain foreign times stay unknown");
}
void selection() {
    std::vector<AudioDeviceInfo> list{{"mini","Same",false,true,"stable",AudioBackend::wasapi,true},
        {"pa","Same",false,true,{},AudioBackend::wasapi,false,AudioProvider::portAudio}};
    AudioDeviceSelector select; select.name = "Same"; std::size_t index = 99;
    ok(resolveAudioDevice(list,select,index)); require(index == 0,"default provider compatibility");
    select.provider = AudioProvider::portAudio; ok(resolveAudioDevice(list,select,index)); require(index == 1,"provider/backend independent");
    select.persistentId = "pa"; require(resolveAudioDevice(list,select,index) == Status::device_unavailable,"scoped ID never persistent");
    select.persistentId.clear(); list.push_back(list[1]);
    require(resolveAudioDevice(list,select,index) == Status::ambiguous,"provider duplicate refusal");
    AudioDeviceContext context; ok(context.prepare(true));
    require(context.provider() == AudioProvider::miniaudio && context.backend() == AudioBackend::nullDevice,"legacy null provider");
    require(context.prepare({AudioBackend::nullDevice,AudioProvider::portAudio}) == Status::unsupported_backend && context.provider() == AudioProvider::miniaudio,"explicit provider/backend no fallback");
#if !CYAUDIOCORE_WITH_PORTAUDIO
    require(context.prepare({AudioBackend::automatic,AudioProvider::portAudio}) == Status::unsupported_backend,"disabled provider clear error");
#endif
    AudioDeviceConfig c; c.activeOutputChannels = {0,0};
    require(validateAudioConfig(c,[](void*,auto,auto,auto) noexcept { return Status::ok; },nullptr) == Status::invalid_argument,"duplicate outputs rejected");
    c.activeOutputChannels = {0,256};
    require(validateAudioConfig(c,[](void*,auto,auto,auto) noexcept { return Status::ok; },nullptr) == Status::invalid_argument,"out-of-range channels rejected");
}
void managerClock() {
    struct Fake {
        AudioDeviceContextCallback callback = nullptr; void* user = nullptr;
        AudioDeviceConfig config; bool active = false; unsigned closed = 0;
    } fake;
    AudioDeviceDriver d; d.context = &fake;
    d.enumerate = [](void*,std::vector<AudioDeviceInfo>& list) noexcept {
        try { list = {{"fake","Simulated",false,true,{},AudioBackend::wasapi,false,AudioProvider::portAudio}}; return Status::ok; }
        catch (...) { return Status::allocation_failed; }
    };
    d.open = [](void*,const auto&,auto,void*) noexcept { return Status::io_error; };
    d.openWithContext = [](void* p,const AudioDeviceConfig& c,AudioDeviceContextCallback cb,void* user) noexcept {
        auto& s = *static_cast<Fake*>(p); s.config = c; s.callback = cb; s.user = user; return Status::ok;
    };
    d.start = [](void* p) noexcept { static_cast<Fake*>(p)->active = true; return Status::ok; };
    d.close = [](void* p) noexcept { auto& s = *static_cast<Fake*>(p); s.active = false; ++s.closed; };
    d.running = [](void* p) noexcept { return static_cast<Fake*>(p)->active; };
    d.sampleRate = [](void*) noexcept { return 48000u; };
    AudioDeviceManager manager; ok(manager.prepare(d)); bool observed = false;
    ok(manager.openWithContext({},[](void* p,auto,auto,const AudioCallbackContext& c) noexcept {
        *static_cast<bool*>(p) = c.clock.monotonicNanoseconds == 123 && c.adcTime.nanoseconds == 100 && c.nativeCallbackFrames == 37 && c.generation == 1;
        return Status::ok;
    },&observed));
    AudioCallbackContext timing; timing.clock.monotonicNanoseconds = 123; timing.adcTime.nanoseconds = 100; timing.nativeCallbackFrames = 37; timing.generation = fake.config.generation;
    ok(fake.callback(fake.user,{},{},timing)); require(observed,"manager preserves provider timing");
    fake.active = false; require(manager.poll(300) == Status::device_unavailable,"retry interval respected"); require(manager.generation() == 1 && fake.closed == 1,"disconnect observed before retry");
    ok(manager.poll(1200)); require(manager.generation() == 2 && manager.running(),"control reconnect advances generation");
    manager.close(); require(fake.closed == 2,"manager stream cleanup");
}
// Active channels are indices into the device's own channels: any subset of them can be
// selected, not only one that ends on the last native channel. The null device is stereo.
void activeChannels() {
    AudioDeviceContext context; ok(context.prepare(true));
    struct Seen { std::atomic<unsigned> calls{0}, wrong{0}; } seen;
    const AudioDeviceCallback callback = [](void* p,AudioBlock<const float>,AudioBlock<float> out,std::uint64_t) noexcept {
        auto& s = *static_cast<Seen*>(p);
        if (out.channels() != 1) s.wrong.fetch_add(1);
        for (auto x : out.channel(0)) if (x != 0) s.wrong.fetch_add(1);
        s.calls.fetch_add(1); return Status::ok;
    };
    AudioDevice device; AudioDeviceConfig c; c.outputChannels = 1; c.maxCallbackFrames = 64;
    for (unsigned native : {0u,1u}) {
        c.activeOutputChannels = {native};
        ok(device.open(context,c,callback,&seen));
        AudioStreamDescription d; ok(device.description(d));
        require(d.output.channels == 1 && d.output.nativeChannels == 2 && d.output.activeChannels == std::vector<unsigned>{native},
            "any native channel selectable");
    }
    c.activeOutputChannels = {2};
    require(device.open(context,c,callback,&seen) == Status::channel_mismatch && device.sampleRate() == 48000,
        "channel beyond the device refused, previous stream kept");
    ok(device.start());
    for (unsigned i = 0; i < 200 && seen.calls.load() < 8; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ok(device.stop());
    require(seen.calls.load() >= 8 && !seen.wrong.load() && !device.callbackErrors(),"selected channel callback contract");
    device.close();
}
}
void audioProviderRegression() { selection(); buffers(); managerClock(); activeChannels(); std::cout << "PASS audio providers/shared buffers/layouts/maps/clocks/reconnect/realtime\n"; }
