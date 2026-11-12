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
}
void midiProtocolRegression() { midi(); }
