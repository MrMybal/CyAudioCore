#include <cyaudio/CyAudioCore.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace cyaudio;
namespace {
volatile double sink=0;
void require(bool v) { if(!v) throw std::runtime_error("synthesis measurement"); }
template<class F> void bench(const char* name,const char* type,CpuBackend backend,std::size_t voices,int,F f) {
 for(int i=0;i<20;++i)f();
 const auto begin=std::chrono::steady_clock::now();
 for(int i=0;i<1000;++i)f();
 const auto ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-begin).count()/1000;
 std::cout << name << " sampleType=" << type << " backend=" << backendName(backend) << " voices=" << voices << " nsPerBlock=" << ns << "\n";
}
template<class T> void run() {
 const auto* type=std::same_as<T,float>?"float":"double";
 VectorOps ops;require(VectorOps::create(CpuBackend::automatic,ops)==Status::ok);const auto backend=ops.backend();
 AudioBuffer<T> output; require(output.resize(2,128)==Status::ok);
    for (std::size_t voices : {16u, 64u}) {
        Synthesizer<T> synth;
        require(synth.prepare({48000, 128, 2}, voices, backend) == Status::ok);
        SynthParameters p;
        p.envelope = {0, 0, 1, .1};
        require(synth.setParameters(p) == Status::ok);
        MidiBuffer notes;
        require(notes.prepare(voices, voices * 3) == Status::ok);
        for (std::size_t i = 0; i < voices; ++i) {
            const auto note = midiNoteOn(1, 36 + static_cast<unsigned>(i), 100);
            require(notes.add(0, note.bytes()) == Status::ok);
        }
        require(synth.process(output.block(), &notes) == Status::ok);
        bench("polyphonicSynth128", type, backend, voices, 2, [&] {
            require(synth.process(output.block()) == Status::ok);
            sink = output.block().channel(0)[127];
        });
    }
}
}
int main(){ try {run<float>();run<double>();return 0;}catch(const std::exception& e){std::cerr<<e.what();return 1;} }
