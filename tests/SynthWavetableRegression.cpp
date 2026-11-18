#include <cyaudio/CyAudioCore.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
void runRealtimeAudit(void (*)(void*) noexcept, void*);
namespace {
using namespace cyaudio;
void require(bool v, const char* why) { if (!v) throw std::runtime_error(why); }
void ok(Status r) { require(r == Status::ok, "wavetable integration status"); }
template <Sample T> void processAudited(Synthesizer<T>& synth, AudioBlock<T> block,
    const MidiBuffer* midi = nullptr, const UmpBuffer* ump = nullptr, bool add = false) {
    struct Job {
        Synthesizer<T>& synth; AudioBlock<T> block;
        const MidiBuffer* midi; const UmpBuffer* ump; bool add;
        Status result = Status::not_prepared;
    } job{synth,block,midi,ump,add};
    runRealtimeAudit([](void* raw) noexcept {
        auto& j = *static_cast<Job*>(raw);
        j.result = j.synth.process(j.block,j.midi,j.ump,j.add);
    },&job);
    ok(job.result);
}
template <Sample T> void fill(AudioBuffer<T>& out, T value) {
    for (std::size_t ch = 0; ch < out.block().channels(); ++ch)
        std::fill(out.block().channel(ch).begin(),out.block().channel(ch).end(),value);
}
template <Sample T> void eventsAndPhase(CpuBackend backend, std::size_t channels, bool add) {
    std::array<T, 2048> cycle{};
    for (std::size_t i = 0; i < cycle.size(); ++i) {
        const auto phase = 2 * std::numbers::pi * double(i) / cycle.size();
        cycle[i] = static_cast<T>(.125 + std::sin(phase) + .25 * std::sin(7 * phase));
    }
    Wavetable<T> table;
    ok(table.prepare(cycle,backend));
    Synthesizer<T> synth;
    ok(synth.prepare({48000,64,channels},2,backend));
    SynthParameters p; p.envelope = {0,0,1,0}; p.gain = .75; p.stealFadeFrames = 0;
    ok(synth.setParameters(p)); ok(synth.setMpeZones(0,0)); ok(synth.setWavetable(&table));
    MidiBuffer midi; ok(midi.prepare(4,16));
    const auto expression = midiController(1,11,127), pan = midiController(1,10,90), volume = midiController(1,7,64);
    ok(midi.add(11,expression.bytes())); ok(midi.add(23,pan.bytes())); ok(midi.add(37,volume.bytes()));
    UmpBuffer ump; ok(ump.prepare(8));
    ok(ump.add(3,umpNote(true,0,1,69,65535)));
    ok(ump.add(11,umpNote(true,0,1,84,65535)));
    ok(ump.add(17,umpPitch(0,1,0xc0000000u)));
    ok(ump.add(29,umpPitch(0,1,0xa0000000u,69)));
    ok(ump.add(41,umpNote(false,0,1,69,0)));
    ok(ump.add(50,umpController(0,1,120,0)));
    ok(ump.add(53,umpNote(true,0,1,69,32768)));
    ok(ump.add(63,umpPitch(0,1,0x80000000u)));
    AudioBuffer<T> actual; ok(actual.resize(channels,74));
    // Legacy reference uses public sample lookup and explicit MIDI semantics, not block rendering.
    struct Voice { double phase = 0, note = 69, velocity = 1, bend = 0; bool active = false; };
    std::array<Voice,2> voices{};
    double pitch = 0, channelPan = 0, channelVolume = 1;
    auto reference = [&](std::size_t frame, std::size_t ch, bool advance) {
        if (frame == 3) voices[0] = {0,69,1,0,true};
        if (frame == 11) voices[1] = {0,84,1,0,true};
        if (frame == 17) pitch = 2 * (double(0xc0000000u) - 2147483648.0) / 2147483647.0;
        if (frame == 23) channelPan = 26.0 / 63;
        if (frame == 29) voices[0].bend = 12;
        if (frame == 37) channelVolume = 64.0 / 127;
        if (frame == 41) voices[0].active = false;
        if (frame == 50) voices[0].active = voices[1].active = false;
        if (frame == 53) voices[0] = {0,69,32768.0 / 65535,0,true};
        if (frame == 63) pitch = 0;
        T result = add ? T(.125) : T{};
        for (auto& v : voices) if (v.active) {
            const auto frequency = std::clamp(440 * std::exp2((v.note - 69 + v.bend + pitch) / 12),0.0,24000.0);
            const auto gain = static_cast<T>(v.velocity * .75 * channelVolume *
                (channels == 2 ? std::sqrt(ch == 0 ? (1 - channelPan) / 2 : (1 + channelPan) / 2) : 1));
            result += table.value(v.phase,frequency,48000) * gain;
            if (advance) { v.phase += frequency / 48000; v.phase -= std::floor(v.phase); }
        }
        return result;
    };
    for (const auto frames : {64u,13u}) {
        const auto globalStart = frames == 64 ? 0u : 64u;
        fill(actual,T(99));
        auto block = actual.block().subBlock(5,frames);
        if (add) for (std::size_t ch = 0; ch < channels; ++ch) std::fill(block.channel(ch).begin(),block.channel(ch).end(),T(.125));
        processAudited(synth,block,frames == 64 ? &midi : nullptr,frames == 64 ? &ump : nullptr,add);
        for (std::size_t i = 0; i < frames; ++i) {
            // Advance each reference voice only after all output channels read its sample.
            for (std::size_t ch = 0; ch < channels; ++ch) {
                const auto expected = reference(globalStart + i,ch,ch + 1 == channels);
                require(std::abs(double(block.channel(ch)[i]) - expected) <= (std::same_as<T,float> ? 2e-6 : 2e-13),
                    "wavetable MIDI boundaries/phase/add/pan differ from legacy sample reference");
            }
        }
        for (std::size_t ch = 0; ch < channels; ++ch) {
            for (std::size_t i = 0; i < 5; ++i) require(actual.block().channel(ch)[i] == T(99),"wavetable leading guard");
            for (std::size_t i = 5 + frames; i < 74; ++i) require(actual.block().channel(ch)[i] == T(99),"wavetable trailing guard");
        }
        processAudited(synth,actual.block().subBlock(0,0));
        require(synth.process({nullptr,channels,1}) == Status::invalid_argument,"invalid output rejected");
        require(synth.process(actual.block().subBlock(0,65)) == Status::block_too_large,"oversized output rejected");
    }
    require(synth.activeVoices() == 1 && synth.stolenVoices() == 0,"wavetable voice lifecycle");
}
template <Sample T> void stealing(CpuBackend backend) {
    std::array<T,256> cycle{};
    for (std::size_t i = 0; i < cycle.size(); ++i)
        cycle[i] = static_cast<T>(.125 + std::sin(2 * std::numbers::pi * double(i) / cycle.size()));
    Wavetable<T> table; ok(table.prepare(cycle,backend));
    Synthesizer<T> synth; ok(synth.prepare({48000,16,1},1,backend));
    SynthParameters p; p.envelope = {0,0,1,0}; p.gain = 1; p.stealFadeFrames = 4;
    ok(synth.setParameters(p)); ok(synth.setWavetable(&table));
    UmpBuffer events; ok(events.prepare(2));
    ok(events.add(0,umpNote(true,0,1,69,65535))); ok(events.add(7,umpNote(true,0,1,76,65535)));
    AudioBuffer<T> out; ok(out.resize(1,16)); processAudited(synth,out.block(),nullptr,&events);
    double phase = 0; T last{};
    for (std::size_t i = 0; i < 16; ++i) {
        if (i == 7) phase = 0;
        const auto frequency = i < 7 ? 440 : 440 * std::exp2(7.0 / 12);
        const auto value = table.value(phase,frequency,48000);
        T expected = value;
        if (i >= 7 && i < 11) {
            const auto mix = double(i - 7) / 4;
            expected = static_cast<T>(value * mix + double(last) * (1 - mix));
        }
        if (i == 6) last = value;
        require(std::abs(double(out.block().channel(0)[i]) - expected) <= (std::same_as<T,float> ? 1e-6 : 1e-13),"stolen wavetable resets phase and retains crossfade");
        phase += frequency / 48000; phase -= std::floor(phase);
    }
    require(synth.stolenVoices() == 1 && synth.activeVoices() == 1,"wavetable stealing count");
}
template <Sample T> void lifetime(CpuBackend backend) {
    std::array<T,16> cycle{}; cycle.fill(T(.25));
    Wavetable<T> table, empty; ok(table.prepare(cycle,backend));
    Synthesizer<T> synth; ok(synth.prepare({48000,16,1},1,backend));
    SynthParameters p; p.envelope = {0,0,1,0}; p.gain = 1; p.stealFadeFrames = 0;
    ok(synth.setParameters(p)); ok(synth.setWavetable(&table));
    UmpBuffer events; ok(events.prepare(1)); ok(events.add(0,umpNote(true,0,1,69,65535)));
    AudioBuffer<T> out; ok(out.resize(1,16)); processAudited(synth,out.block(),nullptr,&events);
    require(synth.setWavetable(&empty) == Status::transition_in_progress,"table binding requires idle voices");
    // The borrowed object stays stable; replacement/moves occur with its reader idle.
    cycle.fill(T(-.5)); ok(table.prepare(cycle,backend));
    processAudited(synth,out.block());
    for (T sample : out.block().channel(0)) require(sample == T(-.5),"synth refreshes re-prepared wavetable");
    auto movedTable = std::move(table); processAudited(synth,out.block());
    for (T sample : out.block().channel(0)) require(sample == 0,"synth safely reads moved-from table");
    table = std::move(movedTable);
    auto movedSynth = std::move(synth); processAudited(movedSynth,out.block());
    require(synth.process(out.block()) == Status::not_prepared,"moved-from synth");
    for (T sample : out.block().channel(0)) require(sample == T(-.5),"moved synth retains borrowed wavetable");
    struct Reset { Synthesizer<T>& synth; } reset{movedSynth};
    runRealtimeAudit([](void* p) noexcept { static_cast<Reset*>(p)->synth.reset(); },&reset);
    require(movedSynth.setWavetable(&empty) == Status::invalid_argument,"empty table replacement rejected");
    processAudited(movedSynth,out.block(),nullptr,&events);
    for (T sample : out.block().channel(0)) require(sample == T(-.5),"failed binding preserves old source");
    movedSynth.reset(); ok(movedSynth.setWavetable(nullptr)); processAudited(movedSynth,out.block(),nullptr,&events);
    require(out.block().channel(0)[0] == 0,"clearing wavetable restores oscillator mode");
    movedSynth.reset(); ok(movedSynth.prepare({44100,16,1},1,backend));
    ok(movedSynth.setParameters(p)); ok(movedSynth.setWavetable(&table));
    processAudited(movedSynth,out.block(),nullptr,&events);
    for (T sample : out.block().channel(0)) require(sample == T(-.5),"reprepared synth binds new-rate oscillator");
}
template <Sample T> void allSoundOff() {
    Synthesizer<T> synth;
    ok(synth.prepare({48000, 32, 1}, 5));
    ok(synth.setMpeZones(2, 1));
    SynthParameters params;
    params.envelope = {0, 0, 1, 2};
    ok(synth.setParameters(params));
    UmpBuffer events;
    ok(events.prepare(10));
    const auto add = [&](unsigned group, unsigned frame, ShortMidiMessage message) {
        ok(events.add(frame, umpMidi1(group, MidiMessageView(message.bytes()))));
    };
    constexpr std::array<unsigned, 5> channels{2, 3, 15, 8, 2};
    for (unsigned i = 0; i < channels.size(); ++i) {
        const unsigned group = i == 4 ? 1 : 0;
        add(group, 0, midiNoteOn(channels[i], 69, 127));
        if (i != 1)
            add(group, 8, midiNoteOff(channels[i], 69));
    }
    AudioBuffer<T> out;
    ok(out.resize(1, 32));
    processAudited(synth, out.block(), nullptr, &events);
    require(synth.activeVoices() == 5 && synth.voice(0).releasing && !synth.voice(1).releasing,
            "held and releasing voices present before All Sound Off");
    events.reset();
    add(0, 0, midiController(1, 120, 0));
    processAudited(synth, out.block(), nullptr, &events);
    require(synth.activeVoices() == 3 && !synth.voice(0).active && !synth.voice(1).active &&
                synth.voice(2).active && synth.voice(3).active && synth.voice(4).active,
            "lower manager panic respects other zones, channels and groups");
    events.reset();
    add(0, 0, midiController(16, 120, 0));
    add(0, 0, midiController(8, 120, 0));
    add(1, 16, midiController(2, 120, 0));
    processAudited(synth, out.block(), nullptr, &events);
    const auto samples = out.block().channel(0);
    require(!synth.activeVoices() &&
                std::any_of(samples.begin(), samples.begin() + 16, [](T v) { return v != 0; }) &&
                std::all_of(samples.begin() + 16, samples.end(), [](T v) { return v == 0; }),
            "upper manager, independent channel and member panic silence release at event frame");
}
template <Sample T> void run() {
    allSoundOff<T>();
    for (auto backend : {CpuBackend::scalar,CpuBackend::automatic}) {
        for (std::size_t channels : {1u,2u}) for (bool add : {false,true}) eventsAndPhase<T>(backend,channels,add);
        stealing<T>(backend); lifetime<T>(backend);
    }
}
}
void synthWavetableRegression() {
    run<float>(); run<double>();
    std::cout << "PASS synthesis wavetable blocks/MIDI/phase/stealing/lifetimes/realtime\n";
}
