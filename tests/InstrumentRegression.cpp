#include "TestPaths.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <complex>
#include <cyaudio/CyAudioCore.h>
#include <fstream>
#include <iostream>
#include <limits>
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
    {
        InstrumentPreset oscillator, roundtrip;
        oscillator.cycle.assign(65536, .5);
        std::string encoded;
        ok(serializeInstrumentPreset(oscillator, encoded));
        ok(parseInstrumentPreset(encoded, roundtrip));
        require(roundtrip.cycle == oscillator.cycle, "largest inactive cycle roundtrips");
        oscillator.cycle.push_back(.5);
        require(serializeInstrumentPreset(oscillator, encoded) == Status::invalid_argument,
                "oversized inactive cycle rejected at serialization");
        oscillator.cycle = {std::numeric_limits<double>::infinity()};
        require(serializeInstrumentPreset(oscillator, encoded) == Status::invalid_argument,
                "inactive cycle cannot serialize non-finite values");
    }
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
    // Sample presets load an audio file named by the preset: both are untrusted.
    const auto folder =
        cyaudio::testSupport::temporaryDirectory() /
        ("cyaudio-instrument-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto base = folder / "samples";
    std::filesystem::create_directories(base / "sub");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{folder};
    const auto floatWav = [](const std::filesystem::path& file, unsigned channels,
                             const std::vector<float>& interleaved) {
        std::string raw;
        const auto put = [&](std::uint64_t v, unsigned n) {
            for (unsigned i = 0; i < n; ++i)
                raw.push_back(char(v >> (8 * i)));
        };
        const auto size = std::uint32_t(interleaved.size() * 4);
        raw += "RIFF";
        put(36 + size, 4);
        raw += "WAVEfmt ";
        put(16, 4);
        put(3, 2);
        put(channels, 2);
        put(48000, 4);
        put(48000 * 4 * channels, 4);
        put(4 * channels, 2);
        put(32, 2);
        raw += "data";
        put(size, 4);
        for (auto sample : interleaved)
            put(std::bit_cast<std::uint32_t>(sample), 4);
        std::ofstream(file, std::ios::binary).write(raw.data(), std::streamsize(raw.size()));
    };
    std::vector<float> tone(256);
    for (std::size_t i = 0; i < tone.size(); ++i)
        tone[i] = float(.5 * std::sin(2 * std::numbers::pi * double(i) / 64));
    floatWav(base / "tone.wav", 1, tone);
    floatWav(folder / "outside.wav", 1, tone);
    auto poisoned = tone;
    poisoned[100] = std::numeric_limits<float>::quiet_NaN();
    floatWav(base / "nan.wav", 1, poisoned);
    poisoned[100] = std::numeric_limits<float>::infinity();
    floatWav(base / "inf.wav", 1, poisoned);
    floatWav(base / "wide.wav", 4, std::vector<float>(4 * 256, .25f));
    InstrumentPreset sampled;
    sampled.name = "Sampled";
    sampled.voices = 2;
    sampled.source = InstrumentSource::sample;
    sampled.samplePath = "tone.wav";
    sampled.parameters.envelope = {0, 0, 1, .01};
    const ProcessSpec spec{48000, 128, 2};
    Instrument<float> sampler;
    ok(sampler.prepare(spec, sampled, base));
    const auto* kept = sampler.synthesizer();
    for (const auto* name : {"nan.wav", "inf.wav"}) {
        sampled.samplePath = name;
        require(sampler.prepare(spec, sampled, base) == Status::invalid_argument &&
                    sampler.synthesizer() == kept,
                "non-finite sample data rejected transactionally");
    }
    // The frame limit also bounds memory for wide files.
    sampled.samplePath = "tone.wav";
    ok(sampler.prepare(spec, sampled, base, CpuBackend::automatic, 256));
    require(sampler.prepare(spec, sampled, base, CpuBackend::automatic, 255) ==
                Status::block_too_large,
            "sample frame limit");
    sampled.samplePath = "wide.wav";
    require(sampler.prepare(spec, sampled, base, CpuBackend::automatic, 256) ==
                Status::block_too_large,
            "sample limit accounts for channels");
    ok(sampler.prepare(spec, sampled, base, CpuBackend::automatic, 512));
    // 3 frames x 8 channels fit exactly at 12 stereo frames. A limit of 8
    // exercises the former integer truncation. A very large limit must not wrap.
    floatWav(base / "short-wide.wav", 8, std::vector<float>(24, .25f));
    sampled.samplePath = "short-wide.wav";
    for (std::size_t limit : {8u, 11u})
        require(sampler.prepare(spec, sampled, base, CpuBackend::automatic, limit) ==
                    Status::block_too_large,
                "odd frame count respects multichannel sample budget");
    ok(sampler.prepare(spec, sampled, base, CpuBackend::automatic, 12));
    ok(sampler.prepare(spec, sampled, base, CpuBackend::automatic,
                       std::numeric_limits<std::size_t>::max()));
    // Optional confinement of the preset's path to the host's base directory.
    for (const auto& inside : {std::filesystem::path("tone.wav"),
                               std::filesystem::path("sub") / ".." / "tone.wav"}) {
        sampled.samplePath = inside;
        ok(sampler.prepare(spec, sampled, base, CpuBackend::automatic, 67108864, true));
    }
    for (const auto& outside : {std::filesystem::path("..") / "outside.wav",
                                std::filesystem::path("sub") / ".." / ".." / "outside.wav",
                                folder / "outside.wav"}) {
        sampled.samplePath = outside;
        ok(sampler.prepare(spec, sampled, base));
        require(sampler.prepare(spec, sampled, base, CpuBackend::automatic, 67108864, true) ==
                    Status::invalid_argument,
                "sample path confined to the base directory");
    }
    // Saving replaces the destination only by a complete file.
    const auto presetPath = folder / "preset.json";
    {
        std::ofstream old(presetPath, std::ios::binary);
        old << "previous";
    }
    ok(saveInstrumentPreset(presetPath, preset));
    InstrumentPreset reloaded;
    ok(loadInstrumentPreset(presetPath, reloaded));
    unsigned files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder))
        files += entry.is_regular_file();
    require(reloaded.name == preset.name && reloaded.cycle == preset.cycle && files == 2,
            "preset saved atomically, no temporary left");
    auto invalid = preset;
    invalid.voices = 0;
    require(saveInstrumentPreset(presetPath, invalid) == Status::invalid_argument &&
                loadInstrumentPreset(presetPath, reloaded) == Status::ok &&
                reloaded.name == preset.name,
            "failed save keeps the previous preset");
}
#endif
}
void instrumentRegression() { synthesis<float>(); synthesis<double>();
#if defined(CYAUDIOCORE_TEST_IO)
 instruments();
#endif
}
