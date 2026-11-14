#include <algorithm>
#include <array>
#include <cyaudio/CyAudioCore.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
namespace {
using namespace cydsp;
using namespace cyaudio;
using Bytes = std::vector<std::uint8_t>;
void require(bool v, const char* why) {
    if (!v)
        throw std::runtime_error(why);
}
void ok(Status v) {
    require(v == Status::ok, "metadata status");
}
void le(Bytes& b, std::uint64_t v, unsigned n = 4) {
    for (unsigned i = 0; i < n; ++i)
        b.push_back(std::uint8_t(v >> (i * 8)));
}
void tag(Bytes& b, std::string_view t) {
    b.insert(b.end(), t.begin(), t.end());
}
void chunk(Bytes& b, std::string_view t, const Bytes& p) {
    tag(b, t);
    le(b, p.size());
    b.insert(b.end(), p.begin(), p.end());
    if (p.size() & 1)
        b.push_back(0);
}
Bytes string(std::string_view s) {
    Bytes b(s.begin(), s.end());
    b.push_back(0);
    return b;
}
std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t at) {
    require(at <= b.size() && b.size() - at >= 4, "external parser bounds");
    return std::uint32_t(b[at]) | (std::uint32_t(b[at + 1]) << 8) |
           (std::uint32_t(b[at + 2]) << 16) | (std::uint32_t(b[at + 3]) << 24);
}
template <class F> void walk(std::span<const std::uint8_t> b, F f) {
    std::size_t at = 0;
    while (at < b.size()) {
        require(b.size() - at >= 8, "external chunk header");
        std::string name(reinterpret_cast<const char*>(b.data() + at), 4);
        const auto length = u32(b, at + 4);
        at += 8;
        require(length <= b.size() - at && std::size_t(length) + (length & 1u) <= b.size() - at,
                "external chunk/pad");
        f(name, b.subspan(at, length));
        at += length + (length & 1u);
    }
}
Bytes fixture() {
    Bytes body;
    tag(body, "WAVE");
    Bytes fmt;
    le(fmt, 1, 2);
    le(fmt, 1, 2);
    le(fmt, 48000);
    le(fmt, 96000);
    le(fmt, 2, 2);
    le(fmt, 16, 2);
    chunk(body, "fmt ", fmt);
    Bytes cue;
    le(cue, 2);
    for (auto id : {7u, 9u}) {
        le(cue, id);
        le(cue, 0);
        tag(cue, "data");
        le(cue, 0);
        le(cue, 0);
        le(cue, id == 7 ? 93 : 99);
    }
    chunk(body, "cue ", cue);
    Bytes labels;
    tag(labels, "adtl");
    Bytes label;
    le(label, 7);
    auto name = string("Door impact");
    label.insert(label.end(), name.begin(), name.end());
    chunk(labels, "labl", label);
    Bytes note;
    le(note, 7);
    auto detail = string("prise \xc3\xa9");
    note.insert(note.end(), detail.begin(), detail.end());
    chunk(labels, "note", note);
    Bytes region;
    le(region, 7);
    le(region, 55);
    tag(region, "rgn ");
    le(region, 0, 2);
    le(region, 0, 2);
    le(region, 0, 2);
    le(region, 65001, 2);
    auto txt = string("R\xc3\xa9gion");
    region.insert(region.end(), txt.begin(), txt.end());
    chunk(labels, "ltxt", region);
    chunk(labels, "zzzz", {1, 2, 3});
    chunk(body, "LIST", labels);
    Bytes other;
    tag(other, "adtl");
    Bytes label2;
    le(label2, 9);
    auto n = string("\xc3\x89"
                    "cho");
    label2.insert(label2.end(), n.begin(), n.end());
    chunk(other, "labl", label2);
    chunk(body, "LIST", other);
    Bytes info;
    tag(info, "INFO");
    chunk(info, "INAM", string("Title"));
    chunk(info, "IART", string("Artist"));
    chunk(info, "XRAW", {0xff, 0, 1});
    chunk(body, "LIST", info);
    Bytes smpl;
    for (unsigned i = 0; i < 7; ++i)
        le(smpl, i == 3 ? 60 : 0);
    le(smpl, 1);
    le(smpl, 0);
    le(smpl, 7);
    le(smpl, 1);
    le(smpl, 16);
    le(smpl, 31);
    le(smpl, 0);
    le(smpl, 2);
    chunk(body, "smpl", smpl);
    chunk(body, "Odd!", {0x42});
    Bytes audio;
    for (unsigned i = 0; i < 256; ++i)
        le(audio, i * 64, 2);
    chunk(body, "data", audio);
    Bytes file;
    tag(file, "RIFF");
    le(file, body.size());
    file.insert(file.end(), body.begin(), body.end());
    return file;
}
struct Parsed {
    std::vector<Bytes> lists;
    std::string label, note, title, artist, second;
    unsigned cueFrame = 0, regionLength = 0, loopFirst = 0, loopLast = 0;
    bool odd = false, unknown = false;
};
Parsed independently(std::span<const std::uint8_t> file) {
    require(file.size() >= 12 && u32(file, 4) == file.size() - 8, "exact RIFF declared extent");
    Parsed p;
    walk(file.subspan(12), [&](const auto& name, auto b) {
        if (name == "cue ") {
            require(u32(b, 0) == 2, "external cue count");
            p.cueFrame = u32(b, 24);
        }
        if (name == "smpl") {
            p.loopFirst = u32(b, 44);
            p.loopLast = u32(b, 48);
        }
        if (name == "Odd!")
            p.odd = b.size() == 1 && b[0] == 0x42;
        if (name != "LIST")
            return;
        p.lists.emplace_back(b.begin(), b.end());
        require(b.size() >= 4, "list type exists");
        const auto type = std::string(reinterpret_cast<const char*>(b.data()), 4);
        walk(b.subspan(4), [&](const auto& child, auto value) {
            auto text = [&](std::size_t at) {
                require(at < value.size() && value.back() == 0, "external NUL");
                return std::string(reinterpret_cast<const char*>(value.data() + at),
                                   value.size() - at - 1);
            };
            if (type == "adtl") {
                if (child == "labl") {
                    if (u32(value, 0) == 7)
                        p.label = text(4);
                    else if (u32(value, 0) == 9)
                        p.second = text(4);
                }
                if (child == "note")
                    p.note = text(4);
                if (child == "ltxt") {
                    p.regionLength = u32(value, 4);
                    require(text(20) == "R\xc3\xa9gion", "region UTF8");
                }
                if (child == "zzzz")
                    p.unknown = value.size() == 3 && value[2] == 3;
            } else if (type == "INFO") {
                if (child == "INAM")
                    p.title = text(0);
                if (child == "IART")
                    p.artist = text(0);
            }
        });
    });
    return p;
}
void extensible() {
    Bytes body;
    tag(body, "WAVE");
    Bytes fmt;
    le(fmt, 0xfffe, 2);
    le(fmt, 1, 2);
    le(fmt, 48000);
    le(fmt, 192000);
    le(fmt, 4, 2);
    le(fmt, 32, 2);
    le(fmt, 22, 2);
    le(fmt, 24, 2);
    le(fmt, 1);
    const Bytes guid{1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
    fmt.insert(fmt.end(), guid.begin(), guid.end());
    chunk(body, "fmt ", fmt);
    Bytes samples;
    le(samples, 0x40000000);
    le(samples, 0xc0000000);
    chunk(body, "data", samples);
    Bytes bytes;
    tag(bytes, "RIFF");
    le(bytes, body.size());
    bytes.insert(bytes.end(), body.begin(), body.end());
    AudioFileReader reader;
    ok(reader.open(std::make_shared<std::istringstream>(std::string(bytes.begin(), bytes.end()),
                                                        std::ios::binary),
                   2));
    require(reader.info()->encoding == AudioEncoding::pcm32, "extensible container width");
    AudioBuffer<double> output;
    ok(output.resize(1, 2));
    std::size_t read = 0;
    ok(reader.read(0, output.block(), read));
    require(output.block().channel(0)[0] == .5 && output.block().channel(0)[1] == -.5,
            "24 significant bits in 32-bit container");
}
void test() {
    auto original = fixture();
    auto expected = independently(original);
    auto input = std::make_shared<std::istringstream>(
        std::string(reinterpret_cast<const char*>(original.data()), original.size()),
        std::ios::binary);
    AudioFileReader reader;
    ok(reader.open(input, 256));
    auto info = *reader.info();
    AudioMetadata metadata;
    ok(getAudioMetadata(info, metadata));
    require(metadata.markers.size() == 2 && metadata.markers[0].name == "Door impact" &&
                metadata.markers[0].frame == 93 &&
                metadata.markers[1].name == "\xc3\x89"
                                            "cho" &&
                metadata.regions.size() == 1 && metadata.regions[0].lengthFrames == 55 &&
                metadata.loops.size() == 1 && metadata.loops[0].startFrame == 16 &&
                metadata.loops[0].endFrame == 32 &&
                metadata.loops[0].mode == AudioLoopMode::alternating && metadata.tags.size() == 2,
            "public vocabulary");
    AudioBuffer<double> audio;
    ok(audio.resize(1, 256));
    std::size_t n = 0;
    ok(reader.read(0, audio.block(), n));
    auto write = [&](const AudioFileInfo& props) {
        auto output = std::make_shared<std::ostringstream>(std::ios::binary);
        AudioFileWriter w;
        ok(w.open(output, AudioFileFormat::wav, props, {}, 256));
        ok(w.write(audio.block()));
        ok(w.close());
        auto s = output->str();
        return Bytes(s.begin(), s.end());
    };
    auto unchanged = independently(write(info));
    require(unchanged.lists == expected.lists && unchanged.label == expected.label &&
                unchanged.cueFrame == 93 && unchanged.regionLength == 55 &&
                unchanged.loopFirst == 16 && unchanged.loopLast == 31 && unchanged.odd &&
                unchanged.unknown,
            "independent byte-exact LIST roundtrip");
    metadata.markers[0].name = "Edited";
    metadata.loops[0].endFrame = 48;
    metadata.tags[0].value = "Changed";
    ok(setAudioMetadata(info, metadata));
    auto edited = independently(write(info));
    require(edited.label == "Edited" && edited.loopLast == 47 && edited.title == "Changed" &&
                edited.artist == "Artist" && edited.odd && edited.unknown &&
                edited.note == expected.note && edited.second == expected.second,
            "typed edits preserve unknowns");
    std::string json;
    ok(serializeAudioMetadata(metadata, json));
    AudioMetadata decoded;
    ok(parseAudioMetadata(json, decoded));
    require(decoded.markers[0].name == "Edited" && decoded.loops[0].endFrame == 48,
            "versioned metadata");
    auto saved = decoded.markers[0].name;
    require(parseAudioMetadata(R"({"schema":"CyDSP.AudioMetadata","version":2})", decoded) !=
                    Status::ok &&
                decoded.markers[0].name == saved,
            "unknown schema transaction");
    const auto rejected = [&](Bytes bytes) {
        auto stream = std::make_shared<std::istringstream>(
            std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
            std::ios::binary);
        require(reader.open(stream, 256) != Status::ok && reader.info()->frames == 256,
                "malformed RIFF rejected transactionally");
    };
    const auto opened = [&](const Bytes& bytes, AudioFileReader& target) {
        return target.open(
            std::make_shared<std::istringstream>(
                std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
                std::ios::binary),
            256);
    };
    auto invalid = original;
    invalid[32] = 0;
    invalid[33] = 32;
    rejected(invalid); // Invalid frame stride used to produce zero progress.
    invalid = original;
    invalid[8] = 'X';
    rejected(invalid); // Not a WAVE form.
    // A declared RIFF size beyond the stream (streamed or truncated file) is clamped to
    // the stream, never trusted: the intact content stays readable.
    auto oversized = original;
    oversized[4] = oversized[5] = oversized[6] = oversized[7] = 0xff;
    AudioFileReader tolerant;
    ok(opened(oversized, tolerant));
    require(tolerant.info()->frames == 256 &&
                tolerant.info()->metadataJson == reader.info()->metadataJson,
            "oversized RIFF size clamped to the stream");
    // A damaged optional chunk is dropped: it never makes the audio unreadable.
    auto damaged = original;
    for (std::size_t at = 12; at + 8 <= damaged.size();) {
        const auto length = u32(damaged, at + 4);
        if (std::string(reinterpret_cast<const char*>(damaged.data() + at), 4) == "cue ") {
            damaged[at + 8] = damaged[at + 9] = damaged[at + 10] = damaged[at + 11] = 0xff;
            break;
        }
        at += 8 + length + (length & 1u);
    }
    ok(opened(damaged, tolerant));
    AudioMetadata partial;
    ok(getAudioMetadata(*tolerant.info(), partial));
    require(tolerant.info()->frames == 256 && partial.markers.empty() && partial.loops.size() == 1 &&
                partial.tags.size() == 2,
            "damaged cue chunk skipped, other metadata kept");
    // A damaged nested length is not clamped to unrelated file data: that list is
    // dropped as a whole, the following lists are still read.
    auto malformed = original;
    for (std::size_t at = 0; at + 4 < malformed.size(); ++at)
        if (std::string(reinterpret_cast<const char*>(malformed.data() + at), 4) == "labl") {
            malformed[at + 4] = 0xff;
            malformed[at + 5] = 0xff;
            break;
        }
    ok(opened(malformed, tolerant));
    ok(getAudioMetadata(*tolerant.info(), partial));
    require(tolerant.info()->frames == 256 && partial.markers.size() == 2 &&
                partial.markers[0].name.empty() && partial.regions.empty() &&
                partial.markers[1].name == "\xc3\x89"
                                           "cho" &&
                partial.tags.size() == 2,
            "damaged nested list skipped without clamping");
    // The regenerated metadata holds exactly one list of each standardized type, with
    // the unmanaged children inside it: readers that stop at the first list see all.
    unsigned adtl = 0, infoLists = 0;
    for (const auto& list : edited.lists) {
        const auto type = std::string(reinterpret_cast<const char*>(list.data()), 4);
        adtl += type == "adtl";
        infoLists += type == "INFO";
    }
    require(adtl == 1 && infoLists == 1 && edited.lists.size() == 2,
            "one adtl and one INFO list after editing");
}
Bytes riff(const Bytes& body, std::uint32_t declared = 0) {
    Bytes file;
    tag(file, "RIFF");
    le(file, declared ? declared : body.size());
    file.insert(file.end(), body.begin(), body.end());
    return file;
}
Bytes pcm16Format(unsigned channels = 1, unsigned bits = 16) {
    Bytes fmt;
    le(fmt, 1, 2);
    le(fmt, channels, 2);
    le(fmt, 48000);
    le(fmt, 48000 * channels * (bits / 8));
    le(fmt, channels * (bits / 8), 2);
    le(fmt, bits, 2);
    return fmt;
}
Status openBytes(AudioFileReader& reader, const Bytes& bytes) {
    return reader.open(std::make_shared<std::istringstream>(
                           std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
                           std::ios::binary),
                       16);
}
Bytes written(const AudioFileInfo& info, AudioBlock<const double> audio) {
    auto output = std::make_shared<std::ostringstream>(std::ios::binary);
    AudioFileWriter w;
    ok(w.open(output, AudioFileFormat::wav, info, {}, 16));
    ok(w.write(audio));
    ok(w.close());
    const auto s = output->str();
    return Bytes(s.begin(), s.end());
}
// Benign real-world deviations are read; every size stays bounded by the stream.
void tolerance() {
    AudioFileReader reader;
    AudioBuffer<double> out;
    ok(out.resize(1, 16));
    std::size_t n = 0;
    Bytes samples;
    for (unsigned i = 0; i < 3; ++i)
        le(samples, 1000 * (i + 1), 2);
    const auto expect = [&](const Bytes& file, std::uint64_t frames, const char* why) {
        ok(openBytes(reader, file));
        require(reader.info()->frames == frames, why);
        require(reader.read(0, out.block(), n) == Status::end_of_stream && n == frames, why);
        for (std::size_t i = 0; i < frames; ++i)
            require(out.block().channel(0)[i] == 1000.0 * double(i + 1) / 32768, why);
    };
    Bytes body;
    tag(body, "WAVE");
    chunk(body, "fmt ", pcm16Format());
    // Odd-sized last chunk without its pad byte.
    auto unpadded = body;
    chunk(unpadded, "data", samples);
    tag(unpadded, "odd!");
    le(unpadded, 3);
    unpadded.insert(unpadded.end(), {1, 2, 3});
    expect(riff(unpadded), 3, "missing final pad byte");
    // Trailing bytes that cannot hold a chunk header.
    auto trailing = body;
    chunk(trailing, "data", samples);
    trailing.insert(trailing.end(), {9, 9, 9});
    expect(riff(trailing), 3, "trailing bytes after the last chunk");
    // A partial last frame is dropped.
    auto ragged = body;
    auto five = samples;
    five.pop_back();
    chunk(ragged, "data", five);
    expect(riff(ragged), 2, "partial last frame floored");
    // A data size beyond the end (truncated or still-growing recording) is clamped.
    auto truncated = body;
    tag(truncated, "data");
    le(truncated, 1000);
    truncated.insert(truncated.end(), samples.begin(), samples.end());
    expect(riff(truncated, 2000), 3, "truncated data chunk clamped to the stream");
    // A sampler-data size beyond its chunk keeps the loops; a truncated optional chunk
    // at the end is skipped.
    auto sampler = body;
    Bytes smpl;
    for (unsigned i = 0; i < 7; ++i)
        le(smpl, i == 3 ? 61 : 0);
    le(smpl, 1);
    le(smpl, 4096); // Declared sampler data that is not there.
    for (auto v : {5u, 0u, 1u, 2u, 0u, 0u})
        le(smpl, v);
    chunk(sampler, "smpl", smpl);
    chunk(sampler, "data", samples);
    tag(sampler, "LIST");
    le(sampler, 500);
    tag(sampler, "INFO");
    expect(riff(sampler), 3, "defective optional chunks tolerated");
    AudioMetadata metadata;
    ok(getAudioMetadata(*reader.info(), metadata));
    require(metadata.loops.size() == 1 && metadata.loops[0].startFrame == 1 &&
                metadata.loops[0].endFrame == 3 && metadata.unityNote == 61 &&
                metadata.tags.empty(),
            "loops kept despite a bad sampler-data size");
    // An oversized unknown chunk is skipped without being loaded.
    auto large = body;
    chunk(large, "big!", Bytes(16777217, 0x55));
    chunk(large, "data", samples);
    expect(riff(large), 3, "oversized optional chunk skipped");
    require(reader.info()->metadataJson.empty(), "oversized chunk not retained");
    // Structural damage is still rejected, transactionally.
    auto noFormat = Bytes{};
    tag(noFormat, "WAVE");
    chunk(noFormat, "data", samples);
    require(openBytes(reader, riff(noFormat)) != Status::ok && reader.info()->frames == 3,
            "missing fmt rejected");
    auto twice = body;
    chunk(twice, "data", samples);
    chunk(twice, "data", samples);
    require(openBytes(reader, riff(twice)) != Status::ok, "duplicate data rejected");
}
// RF64: ds64 sizes replace 0xffffffff placeholders only (EBU 3306).
void rf64() {
    Bytes samples;
    for (unsigned i = 0; i < 5; ++i)
        le(samples, 1000 * (i + 1), 2);
    const auto build = [&](std::uint64_t ds64Data, std::uint32_t data32) {
        Bytes file;
        tag(file, "RF64");
        le(file, 0xffffffffu);
        tag(file, "WAVE");
        tag(file, "ds64");
        le(file, 28);
        const auto riffAt = file.size();
        le(file, 0, 8);
        le(file, ds64Data, 8);
        le(file, 5, 8);
        le(file, 0);
        chunk(file, "fmt ", pcm16Format());
        tag(file, "data");
        le(file, data32);
        file.insert(file.end(), samples.begin(), samples.end());
        const auto size = std::uint64_t(file.size() - 8);
        for (unsigned i = 0; i < 8; ++i)
            file[riffAt + i] = std::uint8_t(size >> (8 * i));
        return file;
    };
    AudioFileReader reader;
    AudioBuffer<double> out;
    ok(out.resize(1, 16));
    std::size_t n = 0;
    for (const auto& file : {build(10, 0xffffffffu), build(0, 10), build(999, 10)}) {
        ok(openBytes(reader, file));
        require(reader.info()->frames == 5 && reader.info()->formatName == "WAV",
                "RF64 ds64 data size");
        require(reader.read(0, out.block(), n) == Status::end_of_stream && n == 5 &&
                    out.block().channel(0)[4] == 5000.0 / 32768,
                "RF64 samples");
    }
    // A placeholder with a ds64 size beyond the stream is clamped like any data chunk.
    ok(openBytes(reader, build(4000, 0xffffffffu)));
    require(reader.info()->frames == 5, "RF64 oversized ds64 size clamped");
}
// A file without metadata reports an empty string, which every consumer accepts back,
// as it does the "null" older readers produced.
void plain() {
    AudioBuffer<double> audio;
    ok(audio.resize(1, 16));
    for (std::size_t i = 0; i < 16; ++i)
        audio.block().channel(0)[i] = double(i) / 32;
    AudioFileInfo info;
    info.sampleRate = 48000;
    info.channels = 1;
    info.encoding = AudioEncoding::pcm16;
    const auto first = written(info, audio.block());
    AudioFileReader reader;
    ok(openBytes(reader, first));
    require(reader.info()->metadataJson.empty(), "no metadata is an empty string");
    for (const std::string json : {std::string{}, std::string("null")}) {
        auto copy = *reader.info();
        copy.metadataJson = json;
        require(written(copy, audio.block()) == first, "plain reader info re-exported");
        AudioMetadata metadata;
        ok(getAudioMetadata(copy, metadata));
        metadata.markers.push_back({3, 7, "Start", {}});
        metadata.tags.push_back({"title", "Plain"});
        ok(setAudioMetadata(copy, metadata));
        std::string serialized;
        ok(serializeAudioMetadata(metadata, serialized));
        AudioFileReader again;
        ok(openBytes(again, written(copy, audio.block())));
        AudioMetadata decoded;
        ok(getAudioMetadata(*again.info(), decoded));
        require(decoded.markers.size() == 1 && decoded.markers[0].frame == 7 &&
                    decoded.markers[0].name == "Start" && decoded.tags.size() == 1 &&
                    decoded.tags[0].value == "Plain",
                "metadata added to a file that had none");
    }
    // A list type this API does not interpret is opaque: its payload need not be chunks.
    Bytes body;
    tag(body, "WAVE");
    chunk(body, "fmt ", pcm16Format());
    Bytes opaque;
    tag(opaque, "xyzw");
    opaque.insert(opaque.end(), {1, 2, 3, 4, 5});
    chunk(body, "LIST", opaque);
    Bytes samples;
    le(samples, 1, 2);
    chunk(body, "data", samples);
    ok(openBytes(reader, riff(body)));
    AudioMetadata metadata;
    ok(getAudioMetadata(*reader.info(), metadata));
    metadata.tags.push_back({"artist", "Kept"});
    auto edited = *reader.info();
    ok(setAudioMetadata(edited, metadata));
    const auto out = written(edited, audio.block());
    bool kept = false;
    walk(std::span(out).subspan(12), [&](const auto& name, auto b) {
        if (name == "LIST" && b.size() == 9 && b[0] == 'x' && b[8] == 5)
            kept = true;
    });
    require(kept, "opaque list type preserved byte-exactly");
}
// bext text in a legacy 8-bit encoding is preserved byte-exactly, not re-encoded.
void broadcast() {
    Bytes bext(602 + 6, 0);
    const std::string description = "Caf\xe9 \xe0 Paris"; // Latin-1, invalid as UTF-8.
    std::copy(description.begin(), description.end(), bext.begin());
    bext[256] = 'M';
    bext[338] = 0x12; // timeRef
    const std::string history = "A=PCM\r\n";
    std::copy(history.begin(), history.begin() + 6, bext.begin() + 602);
    for (bool legacy : {true, false}) {
        if (!legacy)
            bext[3] = 'e', bext[5] = 'a';
        Bytes body;
        tag(body, "WAVE");
        chunk(body, "fmt ", pcm16Format());
        chunk(body, "bext", bext);
        Bytes samples;
        le(samples, 1, 2);
        chunk(body, "data", samples);
        AudioFileReader reader;
        ok(openBytes(reader, riff(body)));
        AudioBuffer<double> audio;
        ok(audio.resize(1, 1));
        const auto out = written(*reader.info(), audio.block());
        Bytes copy;
        walk(std::span(out).subspan(12), [&](const auto& name, auto b) {
            if (name == "bext")
                copy.assign(b.begin(), b.end());
        });
        require(copy == bext, legacy ? "legacy bext bytes preserved" : "UTF-8 bext roundtrip");
    }
}
} // namespace
void metadataRegression() {
    test();
    extensible();
    tolerance();
    rf64();
    plain();
    broadcast();
    std::cout << "PASS integration WAV LIST/adtl/INFO/typed metadata/independent fixtures\n";
    std::cout << "PASS WAV tolerant bounded parsing/RF64 ds64/plain metadata/legacy bext\n";
}
