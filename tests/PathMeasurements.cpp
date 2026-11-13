#include <cyaudio/CyAudioCore.h>
#include "detail/AudioCallbackAdapter.h"
#include "PathMeasurement.h"
#include <memory>
#include <numbers>

namespace {
using namespace cyaudio;
using namespace path_measurement;
void ok(Status status) { require(status==Status::ok); }
double signature(AudioBlock<const float> block) {
    double sum = 0;
    for (std::size_t ch=0; ch<block.channels(); ++ch)
        for (auto value : block.channel(ch)) { require(std::isfinite(value)); sum += value; }
    return sum;
}
void fill(AudioBuffer<float>& buffer) {
    for (std::size_t ch=0; ch<buffer.block().channels(); ++ch)
        for (std::size_t i=0; i<buffer.block().frames(); ++i)
            buffer.block().channel(ch)[i]=float(.125*std::sin(2*std::numbers::pi*(i+ch*7)/83.));
}
void vectors(std::size_t frames, CpuBackend requested) {
    VectorOps ops; ok(VectorOps::create(requested,ops));
    AudioBuffer<float> input,output; ok(input.resize(2,frames)); ok(output.resize(2,frames)); fill(input);
    bench("mix32",frames,32,backendName(ops.backend()),[&] {
        for (std::size_t ch=0; ch<2; ++ch) ops.fill(output.block().channel(ch),0.f);
        for (unsigned track=0; track<32; ++track) for (std::size_t ch=0; ch<2; ++ch)
            ok(ops.addScaled(input.block().channel(ch),output.block().channel(ch),.03125f));
    },[&] { return signature(output.block()); });
    bench("rms32",frames,32,backendName(ops.backend()),[&] {
        double result=0;
        for (unsigned track=0; track<32; ++track) for (std::size_t ch=0; ch<2; ++ch) {
            float dot=0; const auto data=input.block().channel(ch);
            ok(ops.dot(data,data,dot)); result+=std::sqrt(dot/frames);
        }
        sink=result;
    },[&] { return double(sink); });
}
void callbacks(std::size_t frames, bool planar, bool diagnostics) {
    AudioDeviceConfig config; config.maxCallbackFrames=static_cast<std::uint32_t>(frames);
    config.outputChannels=2; config.diagnostics=diagnostics;
    VectorOps ops; ok(VectorOps::create(CpuBackend::automatic,ops));
    detail::AudioCallbackAdapter adapter;
    ok(adapter.prepare(config,nullptr,[](void* p,auto,auto out,const auto&) noexcept {
        auto& operations=*static_cast<VectorOps*>(p);
        for (std::size_t ch=0; ch<out.channels(); ++ch) operations.fill(out.channel(ch),.125f);
        return Status::ok;
    },&ops,0,2));
    AudioBuffer<float> output; ok(output.resize(2,frames)); std::vector<float> flat(2*frames);
    std::array<float*,2> pointers{output.block().channel(0).data(),output.block().channel(1).data()};
    bench(diagnostics ? (planar ? "callback_planar_diagnostics" : "callback_flat_diagnostics") :
                        (planar ? "callback_planar" : "callback_flat"),frames,1,backendName(ops.backend()),[&] {
        adapter.render(nullptr,planar ? static_cast<void*>(pointers.data()) : flat.data(),
                       frames,planar,diagnostics ? detail::audioSteadyNow() : 0);
    },[&] {
        require(adapter.diagnostics().callbackErrors==0);
        if (!planar) ok(deinterleave(std::span<const float>(flat),output.block()));
        require(signature(output.block())==double(frames)/4); return signature(output.block());
    });
}
void transport(std::size_t frames, bool playing) {
    AudioBuffer<float> input; ok(input.resize(2,frames)); fill(input);
    std::array<MemoryAudioSource<float>,32> sources;
    std::array<AudioTransport<float>,32> transports;
    std::array<AudioBuffer<float>,32> outputs;
    for (unsigned i=0; i<32; ++i) {
        ok(sources[i].prepare(input.block(),48000)); ok(outputs[i].resize(2,frames));
        ok(transports[i].prepare({48000,frames,2},sources[i]));
        if (playing) { ok(transports[i].enqueue({TransportAction::setLoop,0,frames,true})); ok(transports[i].enqueue({TransportAction::play})); }
    }
    bench(playing ? "transport32_playing" : "transport32_paused",frames,32,"automatic",[&] {
        for (unsigned i=0; i<32; ++i) ok(transports[i].process(outputs[i].block()));
    },[&] {
        double sum=0; for (auto& output : outputs) sum+=signature(output.block());
        require(playing ? std::abs(sum-32*signature(input.block()))<1e-8 : sum==0); return sum;
    });
}
void effects(std::size_t frames, bool chain) {
    AudioBuffer<float> input,temporary,output; ok(input.resize(2,frames)); ok(temporary.resize(2,frames)); ok(output.resize(2,frames)); fill(input);
    std::array<cydsp::Dynamics<float>,16> dynamics;
    std::array<cydsp::Chorus<float>,16> chorus;
    std::array<cydsp::Reverb<float>,16> reverb;
    for (unsigned i=0; i<16; ++i) {
        ok(dynamics[i].prepare({48000,frames,2}));
        if (chain) { ok(chorus[i].prepare({48000,frames,2})); ok(reverb[i].prepare({48000,frames,2})); }
    }
    bench(chain ? "dsp16_compressor_chorus_reverb" : "dsp16_compressor",frames,16,"public_dsp",[&] {
        for (unsigned i=0; i<16; ++i) {
            ok(dynamics[i].process(input.block(),output.block()));
            if (chain) { ok(chorus[i].process(output.block(),temporary.block())); ok(reverb[i].process(temporary.block(),output.block())); }
        }
    },[&] { return signature(output.block()); });
}
void plugins(std::size_t frames, const std::filesystem::path& directory) {
    std::unique_ptr<PluginProvider> provider; ok(createPluginProvider(PluginFormat::vst3,provider));
    const auto path=directory/"cydsp_vst_fixture.vst3";
    std::vector<PluginDescriptor> descriptions; ok(provider->scan(path,descriptions)); require(descriptions.size()==1);
    std::array<PluginInstance,8> instances;
    AudioBuffer<float> input,side,output; ok(input.resize(2,frames)); ok(side.resize(1,frames)); ok(output.resize(2,frames)); fill(input);
    const std::array<AudioBlock<const float>,2> ins{input.block(),side.block()};
    const std::array<AudioBlock<float>,1> outs{output.block()};
    PluginProcess<float> process{ins,outs,static_cast<std::uint32_t>(frames)};
    for (auto& instance : instances) {
        ok(provider->create(path,descriptions[0].id,{},instance));
        PluginPrepareOptions options; options.maximumFrames=static_cast<std::uint32_t>(frames);
        ok(instance.activate(options)); ok(instance.start());
    }
    bench("vst3_fixture8_process",frames,8,"fixture",[&] {
        for (auto& instance : instances) ok(instance.process(process));
    },[&] { return signature(output.block()); });
    for (auto& instance : instances) ok(instance.stop());
    bench("vst3_fixture8_control_pump",frames,8,"fixture",[&] {
        for (auto& instance : instances) { std::uint32_t flags=0; ok(instance.pumpMainThread(flags)); sink=flags; }
    },[&] { return double(sink); });
    for (auto& instance : instances) ok(instance.deactivate());
}
void synthesis(std::size_t frames) {
    std::array<Synthesizer<float>,8> synths;
    AudioBuffer<float> output; ok(output.resize(2,frames)); MidiBuffer notes; ok(notes.prepare(8,24));
    for (unsigned note=0; note<8; ++note) { auto message=midiNoteOn(1,48+note,100); ok(notes.add(0,message.bytes())); }
    for (auto& synth : synths) {
        ok(synth.prepare({48000,frames,2},8)); SynthParameters parameters; parameters.envelope={0,0,1,.1};
        ok(synth.setParameters(parameters)); ok(synth.process(output.block(),&notes));
    }
    bench("synth8x8_oscillator",frames,8,"automatic",[&] {
        for (auto& synth : synths) ok(synth.process(output.block()));
    },[&] { return signature(output.block()); });
}
} // namespace
int main(int argc,char** argv) {
    try {
        for (const std::size_t frames : {128u,1024u}) {
            vectors(frames,CpuBackend::scalar); vectors(frames,CpuBackend::automatic);
            callbacks(frames,true,false); callbacks(frames,false,false);
            callbacks(frames,true,true); callbacks(frames,false,true);
            transport(frames,false); transport(frames,true);
            effects(frames,false); effects(frames,true); synthesis(frames);
            if (argc==2) plugins(frames,std::filesystem::absolute(argv[1]));
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
