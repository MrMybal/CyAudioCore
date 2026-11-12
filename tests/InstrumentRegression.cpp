#include <algorithm>
#include <complex>
#include <cyaudio/CyAudioCore.h>
#include <iostream>
#include <numbers>
#include <source_location>
#include <stdexcept>
void runRealtimeAudit(void (*)(void*) noexcept, void*);
namespace {
using namespace cydsp;
using namespace cyaudio;
void require(bool v, const char* why) {
    if (!v)
        throw std::runtime_error(why);
}
void ok(Status r, std::source_location at = std::source_location::current()) {
    if (r != Status::ok)
        throw std::runtime_error("DAW expansion status " + std::to_string(unsigned(r)) + " line " +
                                 std::to_string(at.line()));
}
double magnitude(std::span<const BiquadCoefficients> coefficients, double hz, double rate) {
    auto z = std::polar(1.0, -2 * std::numbers::pi * hz / rate);
    std::complex<double> h = 1;
    for (const auto& c : coefficients)
        h *= (c.b0 + c.b1 * z + c.b2 * z * z) / (1.0 + c.a1 * z + c.a2 * z * z);
    return std::abs(h);
}
template <Sample T> void synthesis() {
    std::array<T, 256> cycle{};
    for (std::size_t i = 0; i < cycle.size(); ++i) {
        const auto phase = 2 * std::numbers::pi * double(i) / cycle.size();
        cycle[i] = static_cast<T>(std::sin(phase) + .5 * std::sin(6 * phase));
    }
    Wavetable<T> table;
    ok(table.prepare(cycle));
    require(table.levels() == 8, "harmonic mip levels");
    WavetableOscillator<T> oscillator;
    ok(oscillator.prepare({48000, 1024, 1}, table));
    ok(oscillator.setFrequency(6000));
    AudioBuffer<T> out;
    ok(out.resize(1, 1024));
    ok(oscillator.process(out.block()));
    std::complex<double> fundamental = 0, alias = 0;
    for (std::size_t i = 0; i < 1024; ++i) {
        fundamental += double(out.block().channel(0)[i]) *
                       std::polar(1.0, -2 * std::numbers::pi * double(i) / 8);
        alias += double(out.block().channel(0)[i]) *
                 std::polar(1.0, -2 * std::numbers::pi * double(i) / 4);
    }
    require(std::abs(std::abs(fundamental) * 2 / 1024 - 1) < 1e-5 &&
                std::abs(alias) * 2 / 1024 < 1e-5,
            "independent wavetable alias rejection");
    AudioBuffer<T> input;
    ok(input.resize(1, 4096));
    for (std::size_t i = 0; i < 4096; ++i)
        input.block().channel(0)[i] =
            static_cast<T>(std::sin(2 * std::numbers::pi * .35 * double(i)));
    SampleData<T> sample;
    ok(sample.prepare(input.block(), 48000));
    double linear = 0, filtered = 0;
    for (std::size_t i = 0; i < 512; ++i) {
        const auto position = 512 + 2 * double(i);
        const auto a = sample.value(0, position, 2, SampleInterpolation::linear),
                   b = sample.value(0, position, 2);
        linear += a * a;
        filtered += b * b;
    }
    require(linear > 200 && filtered < linear * 1e-5, "sampler antialias mip rejection");
    for (std::size_t i = 0; i < 128; ++i)
        require(sample.value(0, double(i), 1) == input.block().channel(0)[i],
                "native sampler exactness");
    std::array<T, 8> loop{1, 1, 1, 1, -1, -1, -1, -1};
    const T* channels[]{loop.data()};
    SampleData<T> loopSample;
    ok(loopSample.prepare({channels, 1, 8}, 48000));
    ok(loopSample.setLoop(0, 8, 2));
    require(loopSample.value(0, 6) == T{-1} && loopSample.value(0, 7) == T{0},
            "loop linear crossfade");
    Synthesizer<T> synth;
    ok(synth.prepare({48000, 1024, 1}, 4));
    ok(synth.setWavetable(&table));
    SynthParameters params;
    params.envelope = {0, 0, 1, 0};
    params.gain = 1;
    params.stealFadeFrames = 0;
    ok(synth.setParameters(params));
    ok(synth.setMpeZones(0, 0));
    MidiBuffer events;
    ok(events.prepare(4, 16));
    const auto note = midiNoteOn(1, 69, 127);
    ok(events.add(0, note.bytes()));
    struct Audit {
        Synthesizer<T>* synth;
        AudioBuffer<T>* out;
        MidiBuffer* events;
        Status status = Status::ok;
    } audit{&synth, &out, &events};
    runRealtimeAudit(
        [](void* ptr) noexcept {
            auto& a = *static_cast<Audit*>(ptr);
            a.status = a.synth->process(a.out->block(), a.events);
            a.synth->reset();
        },
        &audit);
    ok(audit.status);
}
#if defined(CYAUDIOCORE_TEST_IO)
void instruments() {
    const std::string_view fixture =
        R"({"format":"CyDSPInstrument","version":1,"name":"Known","source":0,"voices":1,"mpeLower":0,"mpeUpper":0,"rootNote":60,"loopStart":0,"loopEnd":0,"crossfadeFrames":0,"sample":"","cycle":[],"parameters":{"waveform":0,"gain":0.25,"pan":0,"pressureToGain":0,"stealFadeFrames":0,"sampleInterpolation":1,"attack":0,"decay":0,"sustain":1,"release":0}})";
    InstrumentPreset known;
    ok(parseInstrumentPreset(fixture, known));
    require(known.name == "Known" && known.parameters.gain == .25 && known.voices == 1,
            "independent instrument fixture");
    InstrumentPreset preset;
    preset.name = "CyDSP piano";
    preset.voices = 8;
    preset.parameters.envelope = {0, 0, 1, .01};
    preset.source = InstrumentSource::wavetable;
    preset.cycle.resize(256);
    for (std::size_t i = 0; i < 256; ++i)
        preset.cycle[i] = std::sin(2 * std::numbers::pi * double(i) / 256);
    std::string json;
    ok(serializeInstrumentPreset(preset, json));
    InstrumentPreset copy;
    ok(parseInstrumentPreset(json, copy));
    require(copy.name == preset.name && copy.cycle == preset.cycle && copy.voices == 8,
            "versioned instrument roundtrip");
    auto unknown = json;
    const auto at = unknown.find(R"("version")");
    require(at != std::string::npos, "serialized version");
    const auto number = unknown.find_first_of("0123456789", at);
    unknown.replace(number, 1, "2");
    require(parseInstrumentPreset(unknown, copy) == Status::unsupported_format &&
                copy.name == preset.name,
            "version rejection preserves preset");
    std::string deep(40, '[');
    deep += "0";
    deep.append(40, ']');
    require(parseInstrumentPreset(deep, copy) == Status::invalid_argument, "bounded preset JSON");
    Instrument<float> instrument;
    ok(instrument.prepare({48000, 128, 2}, copy));
    auto* previous = instrument.synthesizer();
    copy.voices = 0;
    require(instrument.prepare({48000, 128, 2}, copy) == Status::invalid_argument &&
                instrument.synthesizer() == previous,
            "instrument transactional preparation");
    AudioBuffer<float> out;
    ok(out.resize(2, 128));
    MidiBuffer notes;
    ok(notes.prepare(4, 16));
    const auto on = midiNoteOn(1, 69, 127);
    ok(notes.add(0, on.bytes()));
    struct Audit {
        Instrument<float>* i;
        AudioBuffer<float>* out;
        MidiBuffer* notes;
        Status status = Status::ok;
    } audit{&instrument, &out, &notes};
    runRealtimeAudit(
        [](void* ptr) noexcept {
            auto& a = *static_cast<Audit*>(ptr);
            a.status = a.i->process(a.out->block(), a.notes);
            a.i->reset();
        },
        &audit);
    ok(audit.status);
}
#endif
}
void instrumentRegression() { synthesis<float>(); synthesis<double>();
#if defined(CYAUDIOCORE_TEST_IO)
 instruments();
#endif
}
