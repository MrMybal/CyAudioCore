#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cyaudio/CyAudioCore.h>
#include <numbers>
#include <stdexcept>
#include <thread>
#include <vector>
void runRealtimeAudit(void (*)(void*) noexcept, void*);
namespace {
using namespace cydsp;
using namespace cyaudio;
void require(bool v, const char* message) {
    if (!v)
        throw std::runtime_error(message);
}
template <Sample T> void clear(AudioBlock<T> block) {
    for (std::size_t ch = 0; ch < block.channels(); ++ch)
        std::fill(block.channel(ch).begin(), block.channel(ch).end(), T{});
}
void ok(Status s) {
    require(s == Status::ok, "expanded status");
}
std::complex<double> response(std::span<const BiquadCoefficients> coeff, double hz) {
    const auto z = std::polar(1.0, -2 * std::numbers::pi * hz / 48000);
    std::complex<double> h = 1;
    for (const auto& c : coeff)
        h *= (c.b0 + c.b1 * z + c.b2 * z * z) / (1.0 + c.a1 * z + c.a2 * z * z);
    return h;
}
void midi() {
    auto a = midiNoteOn(1, 60, 100), b = midiNoteOff(1, 60);
    require(MidiMessageView(a.bytes()).valid() && MidiMessageView(a.bytes()).noteOn(),
            "MIDI note parsing");
    MidiBuffer buffer;
    ok(buffer.prepare(4, 64));
    ok(buffer.add(20, a.bytes()));
    ok(buffer.add(10, b.bytes()));
    ok(buffer.add(20, b.bytes()));
    require(buffer.event(0).frame == 10 && buffer.event(1).bytes[0] == 0x90,
            "MIDI stable sample ordering");
    UmpBuffer ump;
    ok(ump.prepare(3));
    const auto first = umpNote(true, 0, 1, 60, 64000), second = umpNote(true, 0, 1, 62, 64000);
    ok(ump.add(10, first));
    ok(ump.add(20, second));
    ok(ump.add(0, ump.events()[1].packet));
    const auto events = ump.events();
    require(events.size() == 3 && events[0].frame == 0 && events[0].packet.words == second.words &&
                events[1].frame == 10 && events[1].packet.words == first.words &&
                events[2].frame == 20 && events[2].packet.words == second.words,
            "reinserting a borrowed UMP packet survives shifting its original slot");
    MidiStreamParser parser;
    ok(parser.prepare(8));
    std::vector<std::vector<std::uint8_t>> result;
    auto callback = [](void* p, std::span<const std::uint8_t> message, double) noexcept {
        static_cast<decltype(result)*>(p)->emplace_back(message.begin(), message.end());
    };
    const std::array<std::uint8_t, 13> wire{0x90, 60, 100,  61,   0,  0xf0, 1,
                                            0xf8, 2,  0xf7, 0xc0, 12, 0xfe};
    for (auto byte : wire)
        ok(parser.feed({&byte, 1}, 0, callback, &result));
    require(result.size() == 6 && result[1] == std::vector<std::uint8_t>({0x90, 61, 0}) &&
                result[2][0] == 0xf8 && result[3] == std::vector<std::uint8_t>({0xf0, 1, 2, 0xf7}),
            "MIDI fragmented running status/realtime SysEx");
    MidiKeyboardState keyboard;
    ok(keyboard.apply(MidiMessageView(a.bytes())));
    auto sustain = midiController(1, 64, 127);
    ok(keyboard.apply(MidiMessageView(sustain.bytes())));
    ok(keyboard.apply(MidiMessageView(b.bytes())));
    require(keyboard.active(1, 60), "MIDI sustain");
    sustain = midiController(1, 64, 0);
    ok(keyboard.apply(MidiMessageView(sustain.bytes())));
    require(!keyboard.active(1, 60), "MIDI sustain release");
    ok(keyboard.apply(MidiMessageView(sustain.bytes())));
    auto unrelated = midiNoteOff(1, 70);
    ok(keyboard.apply(MidiMessageView(unrelated.bytes())));
    require(!keyboard.active(1, 70), "MIDI unrelated note-off under sustain");
    MidiFile file;
    file.format = 0;
    file.tracks = {{{0, MidiFileEventKind::meta, 0x51, {0x07, 0xa1, 0x20}},
                    {0, MidiFileEventKind::message, 0, {0x90, 60, 100}},
                    {960, MidiFileEventKind::message, 0, {0x80, 60, 0}}}};
    std::vector<std::uint8_t> bytes;
    ok(file.serialize(bytes));
    MidiFile decoded;
    ok(decoded.parse(bytes));
    double seconds = 0;
    ok(decoded.tickToSeconds(960, seconds));
    require(std::abs(seconds - .5) < 1e-12 && decoded.tracks[0].size() == 4,
            "SMF roundtrip/time map");
    const auto original = bytes;
    bytes.pop_back();
    require(decoded.parse(bytes) == Status::invalid_argument && decoded.tracks[0].size() == 4,
            "SMF truncation transactional");
    // Running status fixture independent of our serializer.
    const std::array<std::uint8_t, 33> fixture{
        'M', 'T', 'h', 'd', 0,  0, 0,    6,  0,   0, 0,  1, 1, 0xe0, 'M',  'T', 'r',
        'k', 0,   0,   0,   11, 0, 0x90, 60, 127, 1, 61, 0, 0, 0xff, 0x2f, 0};
    ok(decoded.parse(fixture));
    require(decoded.tracks[0][1].data == std::vector<std::uint8_t>({0x90, 61, 0}),
            "SMF running-status fixture");
    SpscQueue<unsigned> queue;
    ok(queue.prepare(127));
    std::atomic<bool> correct{true};
    std::thread producer([&] {
        for (unsigned i = 0; i < 50000; ++i)
            while (!queue.push(i))
                std::this_thread::yield();
    });
    for (unsigned i = 0; i < 50000; ++i) {
        unsigned value;
        while (!queue.pop(value))
            std::this_thread::yield();
        if (value != i)
            correct.store(false);
    }
    producer.join();
    require(correct.load() && !queue.peek(), "SPSC concurrent order");
    struct Audit {
        MidiBuffer& buffer;
        MidiStreamParser& parser;
        SpscQueue<unsigned>& queue;
        std::span<const std::uint8_t> message;
    };
    Audit audit{buffer, parser, queue, a.bytes()};
    runRealtimeAudit(
        [](void* p) noexcept {
            auto& a = *static_cast<Audit*>(p);
            a.buffer.reset();
            (void)a.buffer.add(0, a.message);
            a.parser.reset();
            (void)a.parser.feed(
                a.message, 0, [](void*, std::span<const std::uint8_t>, double) noexcept {},
                nullptr);
            unsigned value;
            (void)a.queue.push(42);
            (void)a.queue.pop(value);
        },
        &audit);
}
void parameters() {
    MidiParameterParser parser;
    parser.reset();
    MidiParameter p;
    auto cc = [&](unsigned controller, unsigned value) {
        const auto m = midiController(1, controller, value);
        return parser.process(MidiMessageView(m.bytes()), p);
    };
    require(!cc(101, 0) && !cc(100, 0) && cc(6, 2) && p.value == (2u << 7) && cc(38, 50) &&
                p.value == ((2u << 7) | 50) && p.registered && p.number == 0,
            "RPN data entry MSB then LSB");
    require(cc(6, 3) && p.value == (3u << 7), "new data MSB does not keep the previous LSB");
    require(!cc(100, 1) && cc(38, 5) && p.number == 1 && p.value == 5,
            "another parameter does not inherit the previous data MSB");
    require(!cc(101, 0) && !cc(100, 1) && cc(96, 0) && p.value == 6 && cc(97, 0) && p.value == 5,
            "re-sent selection keeps the value for increment/decrement");
    require(!cc(99, 1) && !cc(98, 2) && cc(6, 1) && !p.registered && p.number == 130 &&
                p.value == 128,
            "NRPN selection starts clean");
    // Compound MIDI 1 controllers have no MIDI 2 controller form.
    UmpPacket out = umpNote(true, 0, 1, 1, 1);
    const auto before = out.words;
    for (unsigned controller : {0u, 6u, 32u, 38u, 96u, 97u, 98u, 99u, 100u, 101u}) {
        const auto m = midiController(3, controller, 5);
        require(midi1ToMidi2(umpMidi1(2, MidiMessageView(m.bytes())), out) ==
                        Status::unsupported_format &&
                    out.words == before,
                "compound controller translation refused, output preserved");
    }
    const auto volume = midiController(3, 7, 127);
    ok(midi1ToMidi2(umpMidi1(2, MidiMessageView(volume.bytes())), out));
    require(out.words[0] == 0x42b20700 && out.words[1] == 0xffffffffu,
            "ordinary controller still translated");
}
void tempoMaps() {
    auto tempo = [](std::uint64_t tick, unsigned microseconds) {
        return MidiFileEvent{tick,
                             MidiFileEventKind::meta,
                             0x51,
                             {static_cast<std::uint8_t>(microseconds >> 16),
                              static_cast<std::uint8_t>(microseconds >> 8),
                              static_cast<std::uint8_t>(microseconds)}};
    };
    auto note = [](std::uint64_t tick) {
        return MidiFileEvent{tick, MidiFileEventKind::message, 0, {0x90, 60, 100}};
    };
    // Independent expectation: 480 PPQN, 0.5 s/quarter up to tick 480, 0.25 s to 1440, then 1 s.
    auto expected = [](std::uint64_t tick) {
        if (tick <= 480)
            return static_cast<double>(tick) / 480 * .5;
        if (tick <= 1440)
            return .5 + static_cast<double>(tick - 480) / 480 * .25;
        return 1.0 + static_cast<double>(tick - 1440) / 480;
    };
    MidiFile single; // All tempo events in the conductor track: one-pass path.
    single.format = 1;
    single.division = 480;
    single.tracks = {{tempo(480, 250000), tempo(1440, 500000), tempo(1440, 1000000)},
                     {note(0), note(100), note(2000)}};
    MidiFile spread = single; // Same tempo events over two tracks: general path.
    spread.tracks = {{note(0), tempo(1440, 500000), tempo(1440, 1000000)},
                     {note(0), tempo(480, 250000), note(2000)}};
    for (const auto* file : {&single, &spread}) {
        MidiTempoMap map;
        double seconds = -1;
        require(map.tickToSeconds(0, seconds) == Status::not_prepared && seconds == -1,
                "tempo map requires prepare");
        ok(map.prepare(*file));
        for (std::uint64_t tick : {0ull, 1ull, 479ull, 480ull, 481ull, 1439ull, 1440ull, 1441ull,
                                   2000ull, 96000ull}) {
            double direct = 0, mapped = 0;
            ok(file->tickToSeconds(tick, direct));
            ok(map.tickToSeconds(tick, mapped));
            require(std::abs(direct - expected(tick)) < 1e-9 && direct == mapped,
                    "SMF tempo changes: direct and prepared maps agree with the expectation");
        }
    }
    // A malformed tempo event only matters to the direct call once it is reached.
    auto broken = single;
    broken.tracks[0].push_back({3000, MidiFileEventKind::meta, 0x51, {1, 2}});
    double seconds = 0;
    MidiTempoMap map;
    ok(broken.tickToSeconds(2999, seconds));
    require(broken.tickToSeconds(3000, seconds) == Status::invalid_argument &&
                map.prepare(broken) == Status::invalid_argument,
            "malformed tempo event rejected");
    // Type 2: each track has its own tempo map. SMPTE: constant rate.
    MidiFile patterns;
    patterns.format = 2;
    patterns.division = 480;
    patterns.tracks = {{tempo(0, 250000)}, {tempo(0, 1000000)}};
    ok(map.prepare(patterns, 1));
    ok(map.tickToSeconds(480, seconds));
    double direct = 0;
    ok(patterns.tickToSeconds(480, direct, 1));
    require(seconds == 1 && direct == 1 && map.prepare(patterns, 2) == Status::invalid_argument,
            "type 2 selected track tempo");
    ok(patterns.tickToSeconds(480, direct, 0));
    require(direct == .25, "type 2 tracks are independent");
    patterns.division = 0xe728; // 25 frames per second, 40 ticks per frame.
    ok(map.prepare(patterns));
    ok(map.tickToSeconds(1000, seconds));
    ok(patterns.tickToSeconds(1000, direct));
    require(seconds == 1 && direct == 1, "SMPTE division");
    struct Audit {
        MidiTempoMap& map;
        const MidiFile& file;
        bool good = false;
    } audit{map, single};
    runRealtimeAudit(
        [](void* p) noexcept {
            auto& a = *static_cast<Audit*>(p);
            double x = 0, y = 0;
            a.good = a.map.tickToSeconds(123456, x) == Status::ok &&
                     a.file.tickToSeconds(2000, y) == Status::ok;
        },
        &audit);
    require(audit.good, "tempo lookups do not allocate");
}
void mpeZones() {
    MpeInstrument instrument;
    // Lower zone: manager 1, members 2-4. Upper zone: manager 16, members 14-15.
    ok(instrument.setZones(3, 2));
    struct Log {
        std::array<unsigned, 64> channel{}, key{};
        std::array<MpeChange, 64> change{};
        std::size_t count = 0;
    } log;
    auto callback = [](void* c, MpeChange change, const MpeNote& note, std::uint32_t) noexcept {
        auto& l = *static_cast<Log*>(c);
        if (l.count < l.change.size()) {
            l.channel[l.count] = note.channel;
            l.key[l.count] = note.note;
            l.change[l.count++] = change;
        }
    };
    auto send = [&](ShortMidiMessage m) {
        ok(instrument.process(MidiMessageView(m.bytes()), 0, callback, &log));
    };
    auto pitch = [&](unsigned channel, unsigned key) {
        const auto* n = instrument.note(channel, key);
        require(n != nullptr, "MPE note sounding");
        return n->pitchSemitones;
    };
    send(midiNoteOn(2, 60, 100));
    send(midiNoteOn(4, 72, 100));
    send(midiNoteOn(14, 50, 100));
    send(midiPitchBend(2, 16383));
    send(midiPitchBend(4, 16383));
    send(midiPitchBend(14, 16383));
    require(std::abs(pitch(2, 60) - 48) < 1e-12 && std::abs(pitch(14, 50) - 48) < 1e-12,
            "MPE default member range");
    // Pitch Bend Sensitivity on one member channel covers the whole zone, and only it.
    log.count = 0;
    send(midiController(3, 101, 0));
    send(midiController(3, 100, 0));
    send(midiController(3, 6, 24));
    require(std::abs(pitch(2, 60) - 24) < 1e-12 && std::abs(pitch(4, 72) - 24) < 1e-12 &&
                std::abs(pitch(14, 50) - 48) < 1e-12,
            "member RPN 0 sets the zone-wide per-note range");
    require(log.count == 2 && log.change[0] == MpeChange::expression && log.channel[0] == 2 &&
                log.channel[1] == 4,
            "sounding notes of the zone are updated");
    send(midiNoteOn(3, 64, 100));
    send(midiPitchBend(3, 0));
    require(std::abs(pitch(3, 64) + 24) < 1e-12, "new range applies to later notes of the zone");
    // The manager channel keeps its own range.
    send(midiPitchBend(1, 16383));
    require(std::abs(pitch(2, 60) - 26) < 1e-12, "manager range unchanged by a member RPN");
    send(midiController(1, 101, 0));
    send(midiController(1, 100, 0));
    send(midiController(1, 6, 12));
    require(std::abs(pitch(2, 60) - 36) < 1e-12 && std::abs(pitch(14, 50) - 48) < 1e-12,
            "manager RPN 0 sets only the manager range");
    // Releases and expression visit the sounding notes in ascending key order.
    send(midiPitchBend(1, 8192));
    send(midiNoteOn(2, 100, 100));
    send(midiNoteOn(2, 20, 100));
    log.count = 0;
    send(midiPitchBend(2, 8192));
    require(log.count == 3 && log.key[0] == 20 && log.key[1] == 60 && log.key[2] == 100,
            "channel expression reaches each sounding note once, in key order");
    send({{0xa1, 60, 100}, 3}); // Poly pressure for one key on channel 2.
    send(midiPitchBend(2, 12345));
    send(midiController(2, 74, 32));
    send({{0xd0, 90}, 2}); // Manager pressure is independent of the member note.
    require(instrument.note(2, 60)->pressure == 100.0 / 127 &&
                instrument.note(2, 60)->masterPressure == 90.0 / 127 &&
                instrument.note(2, 100)->pressure == 0,
            "pitch, timbre and manager pressure preserve poly pressure");
    send(midiController(1, 121, 0));
    require(instrument.note(2, 60)->pressure == 100.0 / 127 &&
                instrument.note(2, 60)->masterPressure == 0,
            "manager reset leaves the member pressure intact");
    send({{0xd1, 50}, 2});
    require(instrument.note(2, 60)->pressure == 50.0 / 127 &&
                instrument.note(2, 100)->pressure == 50.0 / 127,
            "member channel pressure updates every sounding key on that channel");
    send(midiController(2, 121, 0));
    require(instrument.note(2, 60)->pressure == 0, "member reset clears member pressure");
    send(midiController(1, 64, 127));
    send(midiNoteOff(2, 60));
    send(midiNoteOff(2, 100));
    require(instrument.note(2, 60) && instrument.note(2, 100), "manager sustain holds members");
    log.count = 0;
    send(midiController(1, 64, 0));
    require(log.count == 2 && log.change[0] == MpeChange::noteOff && log.key[0] == 60 &&
                log.key[1] == 100 && !instrument.note(2, 60) && instrument.note(2, 20),
            "pedal release ends exactly the released notes");
    log.count = 0;
    send(midiController(1, 120, 0));
    require(log.count == 3 && log.change[0] == MpeChange::allSoundOff && !instrument.note(2, 20) &&
                !instrument.note(3, 64) && !instrument.note(4, 72) && instrument.note(14, 50),
            "manager all-sound-off covers its zone only");
    require(instrument.setZones(2, 0) == Status::transition_in_progress, "upper note still sounds");
    send(midiNoteOff(14, 50));
    ok(instrument.setZones(2, 0));
}
// Reserved fields: strict for what we send, ignored for what we receive.
void reservedFields() {
    struct Seen {
        std::array<UmpPacket, 8> packets{};
        std::size_t count = 0;
    } seen;
    auto keep = [](void* c, const UmpPacket& p) noexcept {
        auto& s = *static_cast<Seen*>(c);
        if (s.count < s.packets.size())
            s.packets[s.count++] = p;
    };
    auto controller = umpController(1, 2, 7, 0x12345678);
    auto program = umpProgram(1, 2, 5, 3, 4);
    auto management = umpPerNoteManagement(1, 2, 60, true, false);
    const std::array<std::uint8_t, 2> two{1, 2};
    auto sysex = umpSysEx7(1, 0, two);
    const auto clean = std::array{controller, program, management, sysex};
    controller.words[0] |= 0x55;       // Reserved byte after the controller index.
    program.words[0] |= 0x2a00 | 0x80; // Reserved byte and reserved option flags.
    program.words[1] |= 0x00550000;    // Reserved byte between program and bank.
    management.words[0] |= 0x40;       // Reserved option flags.
    management.words[1] = 0xdeadbeef;  // Reserved data word.
    sysex.words[1] |= 0x00007f7f;      // Padding beyond the two declared bytes.
    const auto dirty = std::array{controller, program, management, sysex};
    UmpStreamParser parser;
    for (std::size_t i = 0; i < dirty.size(); ++i) {
        require(clean[i].valid() && !dirty[i].valid(), "sender validation stays strict");
        ok(parser.feed({dirty[i].words.data(), dirty[i].count}, keep, &seen));
        require(seen.count == i + 1 && seen.packets[i].words == clean[i].words,
                "received reserved fields are cleared, the rest is untouched");
        auto copy = dirty[i];
        require(umpAcceptReceived(copy) && copy.words == clean[i].words, "explicit acceptance");
    }
    auto note = umpNote(true, 0, 1, 60, 1);
    note.words[0] |= 0x8000; // Note number out of range is data, not a reserved field.
    auto badCount = sysex;
    badCount.words[0] |= 0x00070000; // Declares more than six bytes.
    seen.count = 0;
    require(parser.feed({note.words.data(), note.count}, keep, &seen) == Status::invalid_argument &&
                parser.feed({badCount.words.data(), badCount.count}, keep, &seen) ==
                    Status::invalid_argument &&
                !seen.count && !umpAcceptReceived(note),
            "out-of-range data is still rejected on receive");
    MidiEventCollector collector;
    ok(collector.prepare({4, 16}));
    MidiBuffer midi;
    UmpBuffer ump;
    ok(midi.prepare(4, 16));
    ok(ump.prepare(4));
    ok(collector.pushUmp(1000, dirty[0]));
    require(collector.pushUmp(1000, note) == Status::invalid_argument, "collector data range");
    ok(collector.collect({1000, 0, 48000}, 16, midi, ump));
    require(ump.events().size() == 1 && ump.events()[0].packet.words == clean[0].words &&
                ump.add(0, dirty[0]) == Status::invalid_argument,
            "collector accepts a device packet with reserved bits");
    Midi2Controllers controllers;
    ok(controllers.prepare(4));
    ok(controllers.process(dirty[0]));
    ok(controllers.process(dirty[1]));
    const auto* state = controllers.channel(1, 2);
    require(state->cc[7] == 0x12345678 && state->program == 5 && state->bankMsb == 3 &&
                state->bankLsb == 4,
            "controller state from packets with reserved bits");
    UmpSysExAssembler assembler;
    ok(assembler.prepare(16));
    ok(assembler.append(dirty[3]));
    require(assembler.message().size() == 2 && assembler.message()[1] == 2,
            "SysEx padding ignored");
    // MIDI-CI Discovery: reserved category bits ignored on parse, refused on build.
    std::array<std::uint8_t, 32> body{};
    std::size_t n = 0;
    CiDiscovery local;
    local.categories = 0x0c;
    ok(makeCiDiscoveryBody(local, true, 2, body, n));
    body[11] |= 0x61;
    CiDiscovery parsed;
    ok(parseCiDiscovery({0x7f, 0x71, 2, 1, 2, {body.data(), n}}, parsed));
    local.categories = 0x6d;
    std::array<std::uint8_t, 32> other{};
    require(parsed.categories == 0x0c &&
                makeCiDiscoveryBody(local, true, 2, other, n) == Status::invalid_argument,
            "CI discovery reserved categories");
    // Endpoint discovery requests with reserved bits are still answered.
    struct Replies {
        unsigned count = 0;
    } replies;
    UmpEndpointService service;
    UmpEndpointDescription description;
    description.blocks.push_back({{}, "Block"});
    ok(service.prepare(
        description,
        [](void* c, const UmpPacket&) noexcept {
            ++static_cast<Replies*>(c)->count;
            return Status::ok;
        },
        &replies));
    auto discovery = umpEndpointDiscovery(1);
    discovery.words[1] |= 0xabcd0000;
    discovery.words[3] = 7;
    ok(service.process(discovery, 0));
    auto blocks = umpFunctionBlockDiscovery(0, 1);
    blocks.words[0] |= 0x40;
    blocks.words[2] = 9;
    ok(service.process(blocks, 0));
    ok(service.poll(0));
    require(replies.count == 2, "endpoint and function block info replies");
}
}
void midiProtocolRegression() {
    midi();
    parameters();
    tempoMaps();
    mpeZones();
    reservedFields();
}
