#include <cyaudio/CyAudioCore.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <stdexcept>
using namespace cyaudio;
namespace {
volatile double sink = 0;
void require(bool v) { if (!v) throw std::runtime_error("synthesis measurement"); }

template <class F> void bench(const char* source, const char* type, CpuBackend backend,
                              std::size_t voices, std::size_t frames, F process) {
    constexpr unsigned batch = 16;
    for (unsigned i = 0; i < 3 * batch; ++i) process();
    std::array<double, 31> costs{};
    for (auto& cost : costs) {
        const auto begin = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < batch; ++i) process();
        cost = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin).count() / batch;
    }
    std::sort(costs.begin(), costs.end());
    std::cout << std::setprecision(12) << "polyphonicSynth source=" << source
              << " sampleType=" << type << " backend=" << backendName(backend)
              << " voices=" << voices << " frames=" << frames
              << " medianBatchMeanNs=" << costs[costs.size() / 2]
              << " p95BatchMeanNs=" << costs[29] << " batchSize=" << batch
              << " checksum=" << sink << '\n';
}

template <Sample T> void run() {
    const auto* type = std::same_as<T, float> ? "float" : "double";
    VectorOps ops;
    require(VectorOps::create(CpuBackend::automatic, ops) == Status::ok);
    const auto backend = ops.backend();
    std::array<T, 2048> cycle{};
    for (std::size_t i = 0; i < cycle.size(); ++i) {
        const auto phase = 2 * std::numbers::pi * double(i) / cycle.size();
        cycle[i] = static_cast<T>(std::sin(phase) + .25 * std::sin(3 * phase) + .125 * std::sin(7 * phase));
    }
    Wavetable<T> table;
    require(table.prepare(cycle, backend) == Status::ok);
    for (bool wavetable : {false, true})
        for (std::size_t frames : {32u, 128u, 512u})
            for (std::size_t voices : {16u, 64u}) {
                AudioBuffer<T> output;
                require(output.resize(2, frames) == Status::ok);
                Synthesizer<T> synth;
                require(synth.prepare({48000, frames, 2}, voices, backend) == Status::ok);
                SynthParameters p;
                p.envelope = {0, 0, 1, .1};
                require(synth.setParameters(p) == Status::ok);
                if (wavetable) require(synth.setWavetable(&table) == Status::ok);
                MidiBuffer notes;
                require(notes.prepare(voices, voices * 3) == Status::ok);
                for (std::size_t i = 0; i < voices; ++i) {
                    const auto note = midiNoteOn(1, 36 + static_cast<unsigned>(i), 100);
                    require(notes.add(0, note.bytes()) == Status::ok);
                }
                require(synth.process(output.block(), &notes) == Status::ok);
                bench(wavetable ? "wavetable" : "oscillator", type, backend, voices, frames, [&] {
                    require(synth.process(output.block()) == Status::ok);
                    sink = output.block().channel(0).back();
                });
            }
}
}
int main() {
    try { run<float>(); run<double>(); return 0; }
    catch (const std::exception& e) { std::cerr << e.what(); return 1; }
}
