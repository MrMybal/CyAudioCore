#include <cyaudio/CyAudioCore.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>

// Core requires CyDSP 0.3. Exercise its processors through the service buffers
// and graph callback used by applications, without opening a physical device.
#if __has_include(<cydsp/synthesis/SourceBatch.h>) && __has_include(<cydsp/control/Quality.h>)
#include "../examples/DspGraphCallback.h"
#include <cydsp/processors/MatrixConvolver.h>
#if defined(CYAUDIOCORE_TEST_AUDIO_PROVIDER)
#include "detail/AudioCallbackAdapter.h"
#endif
void runRealtimeAudit(void (*)(void*) noexcept, void*);
namespace {
using namespace cyaudio;
void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
void ok(Status status) { require(status == Status::ok, "DSP composition status"); }
template <class F> void audited(F&& job) {
    runRealtimeAudit([](void* ptr) noexcept { (*static_cast<std::remove_reference_t<F>*>(ptr))(); }, &job);
}
template <Sample T> struct SourceNode {
    MemoryAudioSource<T> source;
    std::uint64_t position = 0;
    bool fail = false;
    static Status process(void* ptr, std::span<const AudioBlock<const T>>, std::span<AudioBlock<T>> out) noexcept {
        auto& s = *static_cast<SourceNode*>(ptr);
        if (s.fail) return Status::io_error;
        std::size_t count = 0;
        const auto status = s.source.readAt(s.position, out[0], count);
        s.position += out[0].frames();
        return status == Status::end_of_stream ? Status::ok : status;
    }
    static void reset(void* ptr) noexcept { static_cast<SourceNode*>(ptr)->position = 0; }
};
template <Sample T> void sourcesAndAnalysis() {
    AudioBuffer<T> data, output;
    ok(data.resize(2, 23)); ok(output.resize(2, 27));
    std::fill(data.block().channel(0).begin(), data.block().channel(0).end(), T(.25));
    std::fill(data.block().channel(1).begin(), data.block().channel(1).end(), T(-.5));
    SourceNode<T> source; ok(source.source.prepare(data.block(), 48000));
    const std::array<cydsp::DspBus, 1> bus{{{7, 2}}};
    const std::array<cydsp::DspNode<T>, 1> nodes{{{1, &source, &SourceNode<T>::process, {}, bus, {}, &SourceNode<T>::reset}}};
    const std::array<cydsp::DspConnection, 1> routes{{{1, 7, cydsp::graphOutputNode, 7, 1}}};
    cydsp::MultiBusGraph<T> graph;
    ok(graph.prepare({48000, 16, 2}, {}, bus, nodes, routes));
    examples::DspGraphCallback<T> bridge;
    ok(bridge.prepare(graph));
    require(bridge.timing().latencyFrames == 0, "graph timing remains separate from device clocks");
    require(bridge.process({nullptr,0,17},output.block().subBlock(0,17)) == Status::block_too_large && source.position == 0,
            "oversized direct graph call rejected before consuming source");
    require(bridge.process({nullptr,0,15},output.block().subBlock(0,16)) == Status::invalid_argument && source.position == 0,
            "mismatched frame counts rejected before consuming source");
    cydsp::LevelMeter<T> meter; ok(meter.prepare({48000, 16, 2}, 32));
    for (unsigned block = 0; block < 2; ++block) {
        for (std::size_t ch = 0; ch < 2; ++ch)
            std::fill(output.block().channel(ch).begin(), output.block().channel(ch).end(), T(99));
        auto view = output.block().subBlock(5, 16);
        Status status{};
        audited([&]() noexcept { status = bridge.process({nullptr, 0, 16}, view); if (status == Status::ok) status = meter.process(view); });
        ok(status);
        for (std::size_t ch = 0; ch < 2; ++ch) {
            for (std::size_t i = 0; i < 16; ++i)
                require(view.channel(ch)[i] == (block * 16 + i < 23 ? (ch ? T(-.5) : T(.25)) : T{}), "source EOF zero padding through graph");
            require(output.block().channel(ch)[4] == T(99) && output.block().channel(ch)[21] == T(99), "graph callback offset guards");
        }
    }
    require(std::abs(meter.levels(0).rms - .25 * std::sqrt(23. / 32)) < 1e-12, "Core source -> DSP level meter");
    source.fail = true;
    Status status{};
    audited([&]() noexcept { status = bridge.process({nullptr, 0, 16}, output.block().subBlock(0, 16)); });
    require(status == Status::io_error, "source error propagates through graph");
    for (std::size_t ch = 0; ch < 2; ++ch)
        for (T value : output.block().channel(ch).first(16)) require(value == 0, "graph failure silences every bus");
    cydsp::MultiBusGraph<T> empty;
    require(bridge.prepare(empty) == Status::not_prepared, "failed bridge prepare preserves prior state");
    source.fail = false;
    audited([&]() noexcept { bridge.reset(); status = bridge.process({nullptr, 0, 16}, output.block().subBlock(0, 16)); });
    ok(status); require(output.block().channel(0)[0] == T(.25), "reset restarts Core source without freeing");

#if CYDSP_VERSION_MAJOR > 0 || CYDSP_VERSION_MINOR > 4 || (CYDSP_VERSION_MINOR == 4 && CYDSP_VERSION_PATCH >= 6)
    const std::array<cydsp::DspNode<T>, 2> bypassNodes{{nodes[0], {2, nullptr,
        [](void*, auto, auto out) noexcept {
            for (std::size_t ch = 0; ch < out[0].channels(); ++ch)
                std::fill(out[0].channel(ch).begin(), out[0].channel(ch).end(), std::numeric_limits<T>::quiet_NaN());
            return Status::ok;
        }, bus, bus, {2, 0}}}};
    const std::array<cydsp::DspConnection, 2> bypassRoutes{{{1, 7, 2, 7, 1}, {2, 7, cydsp::graphOutputNode, 7, 1}}};
    ok(graph.prepare({48000, 16, 2}, {}, bus, bypassNodes, bypassRoutes));
    ok(bridge.prepare(graph));
    ok(graph.setBypassed(2, true, 0));
    bridge.reset();
    require(bridge.timing().latencyFrames == 2, "bypassed effect retains its declared latency");
    for (std::size_t first : {0u, 16u}) {
        auto view = output.block().subBlock(5, 16);
        audited([&]() noexcept { status = bridge.process({nullptr, 0, 16}, view); });
        ok(status);
        for (std::size_t ch = 0; ch < 2; ++ch)
            for (std::size_t i = 0; i < view.frames(); ++i) {
                const auto frame = first + i;
                const T expected = frame >= 2 && frame - 2 < data.block().frames() ? data.block().channel(ch)[frame - 2] : T{};
                require(view.channel(ch)[i] == expected, "bypass preserves delayed Core source samples through EOF");
            }
    }
#endif

    SampleData<T> sample; ok(sample.prepare(data.block(), 48000));
    cydsp::Granulator<T> grains; ok(grains.prepare({48000, 16, 2}, sample, 2));
    cydsp::GrainParameters parameters; parameters.envelope = cydsp::GrainEnvelope::rectangular;
    parameters.pan = -1; parameters.durationMs = 1;
    audited([&]() noexcept { status = grains.trigger(parameters, 5); if (status == Status::ok) status = grains.process(output.block().subBlock(0, 3)); });
    ok(status);
    for (T value : output.block().channel(0).first(3)) require(value == 0, "pending grain before split boundary");
    audited([&]() noexcept { status = grains.process(output.block().subBlock(0, 7)); });
    ok(status);
    require(output.block().channel(0)[0] == 0 && output.block().channel(0)[1] == 0 && output.block().channel(0)[2] != 0,
            "grain event offset survives shorter Core blocks");
}

template <Sample T> void transportConvolution() {
    constexpr std::size_t sourceFrames = 257, renderedFrames = 512, capacity = 64;
    AudioBuffer<T> data, capture, output;
    ok(data.resize(3, sourceFrames)); ok(capture.resize(3, capacity + 4)); ok(output.resize(3, capacity + 4));
    for (std::size_t ch = 0; ch < 3; ++ch)
        for (std::size_t i = 0; i < sourceFrames; ++i)
            data.block().channel(ch)[i] = T(int((i * 7 + ch * 3) % 19) - 9) / T(16);
    MemoryAudioSource<T> source; ok(source.prepare(data.block(), 48000));
    const std::array<std::size_t, 7> sizes{{64, 1, 17, 32, 63, 7, 16}};
    std::array<T, 193> ir{}; ir[0] = T(.5); ir[17] = T(-.25); ir[127] = T(.125); ir[192] = T(.0625);
    std::array<T, 64> cross{}; cross[0] = T(-.25); cross[63] = T(.125);
    const std::array<cydsp::ConvolutionRoute<T>, 2> filters{{{0, 2, ir}, {2, 0, cross}}};
    const std::array<cydsp::DspBus, 1> bus{{{1, 3}}};
    for (unsigned mode = 0; mode < 4; ++mode) {
        cydsp::ConvolutionConfig config;
        config.earlyPartition = 16; config.latePartition = 64;
        config.mode = mode == 0 ? cydsp::ConvolutionMode::hybrid : mode == 2 ?
            cydsp::ConvolutionMode::scheduled : cydsp::ConvolutionMode::nonuniform;
        config.spectralHead = mode == 3;
        cydsp::MatrixConvolver<T> convolution, inPlace;
        ok(convolution.prepare({48000, capacity, 3}, 3, filters, config));
        ok(inPlace.prepare({48000, capacity, 3}, 3, filters, config));
        const std::array<cydsp::DspNode<T>, 1> nodes{{{1, &convolution,
            [](void* ptr, auto input, auto out) noexcept {
                return static_cast<cydsp::MatrixConvolver<T>*>(ptr)->process(input[0], out[0]);
            }, bus, bus, {}, [](void* ptr) noexcept { static_cast<cydsp::MatrixConvolver<T>*>(ptr)->reset(); }}}};
        const std::array<cydsp::DspConnection, 2> routes{{
            {cydsp::graphInputNode, 1, 1, 1, 1}, {1, 1, cydsp::graphOutputNode, 1, 1}}};
        cydsp::MultiBusGraph<T> graph; ok(graph.prepare({48000, capacity, 3}, bus, bus, nodes, routes));
        examples::DspGraphCallback<T> callback; ok(callback.prepare(graph));
        AudioTransport<T> transport; ok(transport.prepare({48000, capacity, 3}, source));
        require(callback.timing().latencyFrames == 0 && convolution.latencyFrames() == 0,
                "convolution keeps zero added block latency through Core graph bridge");
        for (unsigned pass = 0; pass < 2; ++pass) {
            ok(transport.enqueue({TransportAction::seek, 0})); ok(transport.enqueue({TransportAction::play}));
            audited([&]() noexcept { callback.reset(); inPlace.reset(); });
            std::size_t first = 0, block = 0;
            while (first < renderedFrames) {
                const auto frames = std::min(sizes[block++ % sizes.size()], renderedFrames - first);
                for (std::size_t ch = 0; ch < 3; ++ch) {
                    std::fill(capture.block().channel(ch).begin(), capture.block().channel(ch).end(), T(99));
                    std::fill(output.block().channel(ch).begin(), output.block().channel(ch).end(), T(99));
                }
                auto in = capture.block().subBlock(2, frames), out = output.block().subBlock(2, frames);
                Status status{};
                audited([&]() noexcept {
                    status = transport.process(in);
                    if (status == Status::ok) status = callback.process(in, out);
                    if (status == Status::ok) status = inPlace.process(in, in);
                });
                ok(status);
                auto sample = [&](std::size_t channel, std::size_t frame, std::size_t delay) {
                    return frame >= delay && frame - delay < sourceFrames ?
                        double(data.block().channel(channel)[frame - delay]) : 0.;
                };
                for (std::size_t ch = 0; ch < 3; ++ch) {
                    for (std::size_t i = 0; i < frames; ++i) {
                        const auto frame = first + i;
                        const double expected = ch == 0 ? -.25 * sample(2, frame, 0) + .125 * sample(2, frame, 63) :
                            ch == 2 ? .5 * sample(0, frame, 0) - .25 * sample(0, frame, 17) +
                                .125 * sample(0, frame, 127) + .0625 * sample(0, frame, 192) : 0.;
                        const auto tolerance = std::same_as<T, float> ? 3e-5 : 1e-10;
                        require(std::abs(double(out.channel(ch)[i]) - expected) < tolerance &&
                                std::abs(double(in.channel(ch)[i]) - expected) < tolerance,
                                "transport EOF/tail -> sparse convolution, alias and disjoint views");
                    }
                    require(output.block().channel(ch)[1] == T(99) && output.block().channel(ch)[frames + 2] == T(99) &&
                            capture.block().channel(ch)[1] == T(99) && capture.block().channel(ch)[frames + 2] == T(99),
                            "convolution keeps Core sub-block guards");
                }
                first += frames;
            }
        }
    }
}

template <Sample T> struct VoiceRenderer {
    cydsp::ModulatedOscillator<T> oscillator;
    cydsp::ModulatedOscillatorParameters parameters;
    static void start(void* ptr, const SynthVoiceInfo&) noexcept { static_cast<VoiceRenderer*>(ptr)->oscillator.reset(); }
    static Status render(void* ptr, const SynthVoiceInfo& voice, AudioBlock<T> out) noexcept {
        auto& s = *static_cast<VoiceRenderer*>(ptr);
        s.parameters.frequencyHz = voice.frequency;
        const auto status = s.oscillator.setParameters(s.parameters);
        return status == Status::ok ? s.oscillator.process(out) : status;
    }
};
template <Sample T> void customSynthVoice() {
    VoiceRenderer<T> renderer;
    ok(renderer.oscillator.prepare({48000, 64, 1}));
    Synthesizer<T> synth; ok(synth.prepare({48000, 64, 1}, 1));
    SynthParameters parameters; parameters.gain = 1; parameters.envelope = {0,0,1,0}; parameters.stealFadeFrames = 0;
    ok(synth.setParameters(parameters)); ok(synth.setMpeZones(0,0));
    ok(synth.setRenderer({&renderer, &VoiceRenderer<T>::start, nullptr, &VoiceRenderer<T>::render}));
    UmpBuffer events; ok(events.prepare(3));
    ok(events.add(3, umpNote(true,0,1,69,65535)));
    ok(events.add(17, umpPitch(0,1,0xc0000000u)));
    ok(events.add(41, umpNote(false,0,1,69,0)));
    AudioBuffer<T> output, reference; ok(output.resize(1,64)); ok(reference.resize(1,64));
    Status status{}; audited([&]() noexcept { status = synth.process(output.block(), nullptr, &events); }); ok(status);
    cydsp::ModulatedOscillator<T> direct; ok(direct.prepare({48000,64,1}));
    ok(direct.setParameters({})); ok(direct.process(reference.block().subBlock(3,14)));
    auto directParameters = renderer.parameters;
    ok(direct.setParameters(directParameters)); ok(direct.process(reference.block().subBlock(17,24)));
    bool signal = false;
    for (std::size_t i = 0; i < 64; ++i) {
        require(std::abs(double(output.block().channel(0)[i] - reference.block().channel(0)[i])) < (std::same_as<T,float> ? 2e-6 : 2e-12), "MIDI1/UMP orchestration of public modulated oscillator");
        signal |= output.block().channel(0)[i] != 0;
    }
    require(signal && synth.activeVoices() == 0 && directParameters.frequencyHz > 440, "custom DSP voice pitch/release");
}

template <Sample T> void spatialWidth() {
    std::array<T,16> cycle; cycle.fill(T(.25));
    Wavetable<T> table; ok(table.prepare(cycle));
    const std::array<const Wavetable<T>*,1> tables{{&table}};
    cydsp::SourceBatch<T> batch; ok(batch.prepare({48000,17,64},1,tables));
    cydsp::BatchSource voice; voice.kind = cydsp::BatchSourceKind::wavetable;
    for (std::size_t ch = 0; ch < 64; ++ch) voice.channelGains[ch] = double(ch + 1) / 64;
    ok(batch.start(0,voice));
    const std::array<cydsp::DspBus,1> bus{{{1,64}}};
    const std::array<cydsp::DspNode<T>,1> nodes{{{1,&batch,[](void* p,auto,auto out) noexcept { return static_cast<cydsp::SourceBatch<T>*>(p)->process(out[0]); },{},bus}}};
    const std::array<cydsp::DspConnection,1> routes{{{1,1,cydsp::graphOutputNode,1,1}}};
    cydsp::MultiBusGraph<T> graph; ok(graph.prepare({48000,17,64},{},bus,nodes,routes));
    examples::DspGraphCallback<T> callback; ok(callback.prepare(graph));
    AudioBuffer<T> out; ok(out.resize(64,23));
    Status status{}; audited([&]() noexcept { status = callback.process({nullptr,0,17},out.block().subBlock(3,17)); }); ok(status);
    for (std::size_t ch = 0; ch < 64; ++ch)
        for (T value : out.block().channel(ch).subspan(3,17))
            require(std::abs(double(value) - .25 * double(ch + 1) / 64) < 1e-7, "64-channel source bank -> graph -> Core block");
}

#if CYDSP_VERSION_MAJOR == 0 && CYDSP_VERSION_MINOR >= 4
template <Sample T> void hrtfTransportGraph([[maybe_unused]] bool removeCommonDelay = false,
                                           [[maybe_unused]] bool fft = false) {
    constexpr std::size_t capacity = 17;
    const std::array<T,3> left{{T(1),T(.25),T(-.125)}}, right{{T(.5),T(-.25),T(.125)}};
    const std::array<cydsp::HrirPoint<T>,2> points{{
        {{1,0,0},left,right,.5,1.25}, {{0,1,0},right,left,1.25,.5}}};
    cydsp::HrtfPreparation preparation;
    ok(cydsp::hrtfPreparation(cydsp::HrtfQuality::realtimeHigh,48000,preparation));
    preparation.detectOnsets = false;
#if CYDSP_VERSION_MINOR > 4 || CYDSP_VERSION_PATCH >= 5
    require(!preparation.removeCommonDelay, "HRTF measurement timing is preserved by default");
    preparation.removeCommonDelay = removeCommonDelay;
#endif
    cydsp::HrtfSet<T> bank; ok(bank.prepare(points,preparation));
    require(bank.storageReport().fractionalDelayTaps == 17 && !bank.truncated(), "explicit 0.4 HRTF preparation policy");
    cydsp::HrirBinaural<T> renderer, reference;
#if CYDSP_VERSION_MINOR > 4 || CYDSP_VERSION_PATCH >= 5
    require(bank.removedCommonDelayFrames() == (removeCommonDelay ? .5 : 0.),
            "explicit common acoustic advance is separate from graph latency");
    const cydsp::HrirRenderConfig config{fft ? cydsp::HrirConvolution::fft : cydsp::HrirConvolution::direct};
    ok(renderer.prepare({48000,capacity,1},bank,config));
    require(renderer.configuration().convolution == config.convolution, "explicit HRIR convolution policy");
    ok(reference.prepare({48000,capacity,1},bank,{cydsp::HrirConvolution::direct}));
#else
    ok(renderer.prepare({48000,capacity,1},bank)); ok(reference.prepare({48000,capacity,1},bank));
#endif
    const std::array<cydsp::DspBus,1> mono{{{7,1}}}, stereo{{{42,2}}};
    const std::array<cydsp::DspNode<T>,1> nodes{{{1,&renderer,
        [](void* ptr,auto in,auto out) noexcept { return static_cast<cydsp::HrirBinaural<T>*>(ptr)->process(in[0],out[0]); },
        mono,stereo,{renderer.latencyFrames(),renderer.tailFrames()},
        [](void* ptr) noexcept { static_cast<cydsp::HrirBinaural<T>*>(ptr)->reset(); }}}};
    const std::array<cydsp::DspConnection,2> routes{{
        {cydsp::graphInputNode,7,1,7,1},{1,42,cydsp::graphOutputNode,42,1}}};
    cydsp::MultiBusGraph<T> graph; ok(graph.prepare({48000,capacity,2},mono,stereo,nodes,routes));
    examples::DspGraphCallback<T> callback; ok(callback.prepare(graph));
    require(renderer.latencyFrames() > 0 && callback.timing().latencyFrames == renderer.latencyFrames(),
            "HRTF fractional-delay latency survives graph callback composition");
    AudioBuffer<T> data, dry, actual, expected;
    ok(data.resize(1,13)); ok(dry.resize(1,capacity));
    ok(actual.resize(2,capacity+4)); ok(expected.resize(2,capacity));
    data.block().channel(0)[0] = T(1);
    MemoryAudioSource<T> source; ok(source.prepare(data.block(),48000));
    AudioTransport<T> transport; ok(transport.prepare({48000,capacity,1},source));
    const std::array<std::size_t,5> sizes{{1,7,17,3,11}};
    for (unsigned pass = 0; pass < 2; ++pass) {
        ok(transport.enqueue({TransportAction::seek,0})); ok(transport.enqueue({TransportAction::play}));
        Status status{};
        audited([&]() noexcept {
            callback.reset(); reference.reset();
            status = renderer.setDirection(pass ? cydsp::Vec3{0,1,0} : cydsp::Vec3{1,0,0},0);
            if (status == Status::ok) status = reference.setDirection(pass ? cydsp::Vec3{0,1,0} : cydsp::Vec3{1,0,0},0);
        }); ok(status);
        bool signal = false;
        for (std::size_t first = 0, block = 0; first < 96; ++block) {
            const auto frames = std::min(sizes[block%sizes.size()],96-first);
            for (std::size_t ch = 0; ch < 2; ++ch)
                std::fill(actual.block().channel(ch).begin(),actual.block().channel(ch).end(),T(99));
            auto input = dry.block().subBlock(0,frames), output = actual.block().subBlock(2,frames);
            auto comparison = expected.block().subBlock(0,frames);
            audited([&]() noexcept {
                status = transport.process(input);
                if (status == Status::ok) status = callback.process(input,output);
                if (status == Status::ok) status = reference.process(input,comparison);
            }); ok(status);
            for (std::size_t ch = 0; ch < 2; ++ch) {
                for (std::size_t i = 0; i < frames; ++i) {
                    require(std::abs(double(output.channel(ch)[i]-comparison.channel(ch)[i])) < 1e-7,
                            "transport EOF and variable blocks preserve prepared HRTF output");
                    signal |= output.channel(ch)[i] != 0;
                }
                require(actual.block().channel(ch)[1] == T(99) && actual.block().channel(ch)[frames+2] == T(99),
                        "HRTF graph leaves Core sub-block guards intact");
            }
            first += frames;
        }
        require(signal,"HRTF graph produces audio before its tail drains");
    }
}
#endif

#if defined(CYAUDIOCORE_TEST_AUDIO_PROVIDER)
void deviceBuses() {
    for (bool planar : {false,true}) for (bool absent : {false,true}) {
        cydsp::Dynamics<float> dynamics, reference;
        ok(dynamics.prepare({48000,17,2})); ok(reference.prepare({48000,17,2}));
        cydsp::DynamicsParameters parameters; parameters.thresholdDb = -20; parameters.kneeDb = 0;
        parameters.attackMs = parameters.releaseMs = 0;
        ok(dynamics.setParameters(parameters)); ok(reference.setParameters(parameters));
        const std::array<cydsp::DspBus,2> ins{{{7,2},{9,1}}};
        const std::array<cydsp::DspBus,2> outs{{{42,2},{43,1}}};
        const std::array<cydsp::DspBus,1> stereo{{{12,2}}};
        const std::array<cydsp::DspNode<float>,1> nodes{{{1,&dynamics,[](void* p,auto in,auto out) noexcept { return static_cast<cydsp::Dynamics<float>*>(p)->process(in[0],in[1],out[0]); },ins,stereo}}};
        const std::array<cydsp::DspConnection,4> routes{{
            {cydsp::graphInputNode,7,1,7,1},{cydsp::graphInputNode,9,1,9,1},
            {1,12,cydsp::graphOutputNode,42,1},{cydsp::graphInputNode,9,cydsp::graphOutputNode,43,1}}};
        cydsp::MultiBusGraph<float> graph; ok(graph.prepare({48000,17,3},ins,outs,nodes,routes));
        examples::DspGraphCallback<float> callback; ok(callback.prepare(graph));
        AudioDeviceConfig config; config.sampleRate = 48000; config.inputChannels = config.outputChannels = 3;
        config.maxCallbackFrames = 17; config.activeInputChannels = {2,0,3}; config.activeOutputChannels = {4,1,3};
        detail::AudioCallbackAdapter adapter;
        ok(adapter.prepare(config,nullptr,&examples::DspGraphCallback<float>::callback,&callback,4,5));
        std::array<std::array<float,67>,4> in{};
        std::array<std::array<float,67>,5> out{};
        std::array<const float*,4> inPointers{}; std::array<float*,5> outPointers{};
        std::array<float,268> flatIn{}; std::array<float,335> flatOut{};
        for (std::size_t ch = 0; ch < 4; ++ch) {
            inPointers[ch] = in[ch].data();
            for (std::size_t i = 0; i < 67; ++i)
                in[ch][i] = flatIn[i*4+ch] = ch == 3 ? (i < 30 ? 1.0f : .01f) : float(ch + 1) / 8;
        }
        for (std::size_t ch = 0; ch < 5; ++ch) { outPointers[ch] = out[ch].data(); out[ch].fill(99); }
        flatOut.fill(99);
        audited([&]() noexcept {
            adapter.render(absent ? nullptr : planar ? static_cast<const void*>(inPointers.data()) : flatIn.data(),
                           planar ? static_cast<void*>(outPointers.data()) : flatOut.data(),67,planar,0);
        });
        require(adapter.diagnostics().callbackErrors == 0, "multibus native callback composition");
        AudioBuffer<float> program, side, expected;
        ok(program.resize(2,17)); ok(side.resize(1,17)); ok(expected.resize(2,17));
        for (std::size_t first = 0; first < 67; first += 17) {
            const auto count = std::min(std::size_t(17),67-first);
            for (std::size_t i = 0; i < count; ++i) {
                program.block().channel(0)[i] = absent ? 0 : in[2][first+i];
                program.block().channel(1)[i] = absent ? 0 : in[0][first+i];
                side.block().channel(0)[i] = absent ? 0 : in[3][first+i];
            }
            ok(reference.process(program.block().subBlock(0,count),side.block().subBlock(0,count),expected.block().subBlock(0,count)));
            for (std::size_t i = 0; i < count; ++i) for (std::size_t ch = 0; ch < 5; ++ch) {
                const auto actual = planar ? out[ch][first+i] : flatOut[(first+i)*5+ch];
                const auto value = ch == 4 ? expected.block().channel(0)[i] : ch == 1 ? expected.block().channel(1)[i] : ch == 3 ? side.block().channel(0)[i] : 0;
                require(std::abs(actual-value) < 1e-7, "sidechain buses/native mappings/unselected silence");
            }
        }
        AudioBuffer<float> missingCapture; ok(missingCapture.resize(3,17));
        Status status{};
        audited([&]() noexcept { status = callback.process({nullptr,0,17},missingCapture.block()); });
        ok(status);
        for (std::size_t ch = 0; ch < 3; ++ch)
            for (float value : missingCapture.block().channel(ch)) require(value == 0, "prepared graph silence for missing capture buses");
    }
}
#endif
} // namespace
void dspCompositionRegression() {
    sourcesAndAnalysis<float>(); sourcesAndAnalysis<double>();
    transportConvolution<float>(); transportConvolution<double>();
    customSynthVoice<float>(); customSynthVoice<double>();
    spatialWidth<float>(); spatialWidth<double>();
#if CYDSP_VERSION_MAJOR == 0 && CYDSP_VERSION_MINOR >= 4
    hrtfTransportGraph<float>(); hrtfTransportGraph<double>();
#if CYDSP_VERSION_MINOR > 4 || CYDSP_VERSION_PATCH >= 5
    for (bool removeCommonDelay : {false,true}) for (bool fft : {false,true}) {
        if (!removeCommonDelay && !fft) continue; // Already covered by the default calls.
        hrtfTransportGraph<float>(removeCommonDelay,fft);
        hrtfTransportGraph<double>(removeCommonDelay,fft);
    }
#endif
#endif
#if defined(CYAUDIOCORE_TEST_AUDIO_PROVIDER)
    deviceBuses();
#endif
    std::cout << "PASS Core/DSP composition sources/convolution/grains/MIDI voices/64 channels/multibus sidechain/realtime\n";
}
#else
void dspCompositionRegression() { std::cout << "SKIP optional DSP expansion composition: older public headers\n"; }
#endif
