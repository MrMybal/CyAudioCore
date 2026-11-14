#include "TestPaths.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cyaudio/CyAudioCore.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
void runRealtimeAudit(void (*)(void*) noexcept, void*);
namespace {
using namespace cydsp;
using namespace cyaudio;
void require(bool v, const char* m) {
    if (!v)
        throw std::runtime_error(m);
}
void ok(Status s, std::source_location at = std::source_location::current()) {
    if (s != Status::ok)
        throw std::runtime_error("IO status " + std::to_string(static_cast<unsigned>(s)) +
                                 " at line " + std::to_string(at.line()));
}
template <Sample T> void clear(AudioBlock<T> b) {
    for (std::size_t ch = 0; ch < b.channels(); ++ch)
        std::fill(b.channel(ch).begin(), b.channel(ch).end(), T{});
}
void files() {
    const std::size_t frames = 16384;
    AudioBuffer<double> source;
    ok(source.resize(2, frames));
    for (std::size_t ch = 0; ch < 2; ++ch)
        for (std::size_t i = 0; i < frames; ++i)
            source.block().channel(ch)[i] =
                .3 * std::sin(2 * std::numbers::pi * (ch ? 1500 : 1000) * static_cast<double>(i) /
                              48000);
    const auto capabilities = audioFileFormats();
    require(capabilities.size() >= 7, "format capabilities");
    for (const auto format : {AudioFileFormat::wav, AudioFileFormat::flac,
                              AudioFileFormat::oggVorbis, AudioFileFormat::aiff}) {
        auto stream =
            std::make_shared<std::stringstream>(std::ios::in | std::ios::out | std::ios::binary);
        AudioFileWriter writer;
        AudioFileInfo info;
        info.sampleRate = 48000;
        info.channels = 2;
        info.encoding =
            format == AudioFileFormat::oggVorbis ? AudioEncoding::float32 : AudioEncoding::pcm24;
        ok(writer.open(stream, format, info, {}, 257));
        for (std::size_t i = 0; i < frames; i += 257)
            ok(writer.write(AudioBlock<const double>(
                source.block().subBlock(i, std::min<std::size_t>(257, frames - i)))));
        require(writer.framesWritten() == frames, "writer exact frame count");
        ok(writer.close());
        const auto encoded = stream->str();
        require(encoded.size() > 100, "encoded file nonempty");
        AudioFileReader reader;
        ok(reader.open(std::make_shared<std::istringstream>(encoded, std::ios::binary), 300));
        require(reader.info() && reader.info()->channels == 2 &&
                    reader.info()->sampleRate == 48000 && reader.info()->frames == frames,
                "audio header properties");
        AudioBuffer<double> out;
        ok(out.resize(2, 300));
        double squared = 0, maximum = 0;
        std::size_t count = 0;
        for (std::size_t i = 0; i < frames; i += 300) {
            const auto n = std::min<std::size_t>(300, frames - i);
            const auto s = reader.read(i, out.block(), count);
            require(s == (n == 300 ? Status::ok : Status::end_of_stream) && count == n,
                    "reader EOS count");
            for (std::size_t ch = 0; ch < 2; ++ch) {
                for (std::size_t j = 0; j < n; ++j) {
                    const auto error =
                        out.block().channel(ch)[j] - source.block().channel(ch)[i + j];
                    maximum = std::max(maximum, std::abs(error));
                    squared += error * error;
                }
                for (std::size_t j = n; j < 300; ++j)
                    require(out.block().channel(ch)[j] == 0, "EOS zero padding");
            }
        }
        require(format == AudioFileFormat::oggVorbis ? std::sqrt(squared / (2 * frames)) < .015
                                                     : maximum < 3e-7,
                "audio roundtrip quality");
        ok(reader.read(700, out.block(19), count));
        require(count == 19, "random-access seek");
        const auto original = reader.info()->frames;
        require(reader.open(std::make_shared<std::istringstream>("not audio")) ==
                        Status::unsupported_format &&
                    reader.info()->frames == original,
                "failed reader preserves state");
        auto moved = std::move(reader);
        require(reader.info() == nullptr && moved.info()->frames == frames, "reader moved-from");
    }
    // Hand-built WAV PCM16 fixture independent of CHOC's writer.
    const std::array<unsigned char, 50> wav{
        'R', 'I', 'F', 'F', 42,  0,   0, 0,    'W',  'A', 'V', 'E',  'f',  'm', 't',  ' ', 16,
        0,   0,   0,   1,   0,   1,   0, 0x80, 0xbb, 0,   0,   0,    0x77, 1,   0,    2,   0,
        16,  0,   'd', 'a', 't', 'a', 6, 0,    0,    0,   0,   0x80, 0,    0,   0xff, 0x7f};
    AudioFileReader reader;
    ok(reader.open(std::make_shared<std::istringstream>(
        std::string(reinterpret_cast<const char*>(wav.data()), wav.size()))));
    AudioBuffer<float> samples;
    ok(samples.resize(1, 3));
    std::size_t count = 0;
    ok(reader.read(0, samples.block(), count));
    require(samples.block().channel(0)[0] == -1 && samples.block().channel(0)[1] == 0 &&
                std::abs(samples.block().channel(0)[2] - 32767.0f / 32768) < 1e-7,
            "independent PCM16 fixture");
    const std::array<unsigned char, 60> aiff{
        'F',  'O',  'R', 'M', 0,  0, 0, 52, 'A', 'I',  'F', 'F', 'C', 'O',  'M',
        'M',  0,    0,   0,   18, 0, 1, 0,  0,   0,    3,   0,   16,  0x40, 0x0e,
        0xbb, 0x80, 0,   0,   0,  0, 0, 0,  'S', 'S',  'N', 'D', 0,   0,    0,
        14,   0,    0,   0,   0,  0, 0, 0,  0,   0x80, 0,   0,   0,   0x7f, 0xff};
    ok(reader.open(std::make_shared<std::istringstream>(
        std::string(reinterpret_cast<const char*>(aiff.data()), aiff.size()))));
    ok(reader.read(0, samples.block(), count));
    require(reader.info()->sampleRate == 48000 && samples.block().channel(0)[0] == -1 &&
                samples.block().channel(0)[2] == 32767.0f / 32768,
            "independent AIFF PCM/rate fixture");
    AudioFileWriter unsupported;
    auto memory = std::make_shared<std::ostringstream>();
    AudioFileInfo info;
    info.sampleRate = 48000;
    info.channels = 2;
    require(unsupported.open(memory, AudioFileFormat::mp3, info) == Status::unsupported_format &&
                memory->str().empty(),
            "MP3 export explicitly unavailable");
}
std::string le(std::uint64_t v, unsigned n) {
    std::string s;
    for (unsigned i = 0; i < n; ++i)
        s.push_back(char(v >> (8 * i)));
    return s;
}
// Canonical mono WAV holding the given integer codes, independent of the writer.
std::string integerWav(unsigned bits, const std::vector<std::int64_t>& codes) {
    const unsigned bytes = bits / 8;
    const auto size = std::uint32_t(codes.size() * bytes);
    auto raw = "RIFF" + le(36 + size + (size & 1), 4) + "WAVEfmt " + le(16, 4) + le(1, 2) +
               le(1, 2) + le(48000, 4) + le(48000 * bytes, 4) + le(bytes, 2) + le(bits, 2) +
               "data" + le(size, 4);
    for (auto x : codes)
        raw += le(bits == 8 ? std::uint64_t(x + 128) : std::uint64_t(x), bytes);
    if (size & 1)
        raw.push_back(0);
    return raw;
}
// Payload of the data chunk, located by an independent chunk walk.
std::string dataOf(const std::string& wav) {
    for (std::size_t at = 12; at + 8 <= wav.size();) {
        std::uint32_t size = 0;
        for (unsigned i = 0; i < 4; ++i)
            size |= std::uint32_t(static_cast<unsigned char>(wav[at + 4 + i])) << (8 * i);
        if (wav.compare(at, 4, "data") == 0)
            return wav.substr(at + 8, size);
        at += 8 + std::size_t(size) + (size & 1u);
    }
    throw std::runtime_error("data chunk not found");
}
std::vector<std::int64_t> sampledCodes(unsigned bits, std::int64_t step) {
    const std::int64_t low = -(std::int64_t(1) << (bits - 1)), high = -low - 1;
    std::vector<std::int64_t> codes;
    for (auto x = low; x <= high; x += step)
        codes.push_back(x);
    if (codes.back() != high)
        codes.push_back(high);
    return codes;
}
// Integer PCM is read as exactly code / 2^(bits-1) and written back bit-identically.
template <Sample T> void exactPcm(unsigned bits, std::int64_t step) {
    const auto codes = sampledCodes(bits, step);
    const auto source = integerWav(bits, codes);
    AudioFileReader reader;
    ok(reader.open(std::make_shared<std::istringstream>(source, std::ios::binary), 4096));
    require(reader.info()->frames == codes.size() && reader.info()->metadataJson.empty(),
            "integer fixture shape");
    auto out = std::make_shared<std::ostringstream>(std::ios::binary);
    AudioFileWriter writer;
    ok(writer.open(out, AudioFileFormat::wav, *reader.info(), {}, 4096));
    AudioBuffer<T> block;
    ok(block.resize(1, 4096));
    const double scale = std::ldexp(1.0, 1 - int(bits));
    for (std::size_t at = 0; at < codes.size(); at += 4096) {
        const auto n = std::min<std::size_t>(4096, codes.size() - at);
        std::size_t count = 0;
        ok(reader.read(at, block.block(n), count));
        require(count == n, "integer fixture count");
        if (bits <= 24 || sizeof(T) == sizeof(double))
            for (std::size_t i = 0; i < n; ++i)
                require(double(block.block().channel(0)[i]) == double(codes[at + i]) * scale,
                        "exact integer normalisation");
        ok(writer.write(AudioBlock<const T>(block.block(n))));
    }
    ok(writer.close());
    require(dataOf(out->str()) == dataOf(source), "bit-transparent integer PCM");
}
void pcmExactness() {
    exactPcm<float>(8, 1);
    exactPcm<double>(8, 1);
    exactPcm<float>(16, 1);
    exactPcm<double>(16, 1);
    exactPcm<float>(24, 37);
    exactPcm<double>(24, 37);
    exactPcm<double>(32, 65521);
    // Export rounds to nearest (half away from zero) and clips to the code range.
    const std::array<double, 12> values{.4 / 32768,  .6 / 32768, -.4 / 32768, -.6 / 32768,
                                        .5 / 32768,  -.5 / 32768, 1.0,        -1.0,
                                        2.0,         -2.0,        100.49 / 32768, -100.51 / 32768};
    const std::array<std::int64_t, 12> expected{0, 1, 0, -1, 1, -1, 32767, -32768,
                                                32767, -32768, 100, -101};
    AudioBuffer<double> block;
    ok(block.resize(1, 4096));
    std::copy(values.begin(), values.end(), block.block().channel(0).begin());
    AudioFileInfo info;
    info.sampleRate = 48000;
    info.channels = 1;
    info.encoding = AudioEncoding::pcm16;
    auto out = std::make_shared<std::ostringstream>(std::ios::binary);
    AudioFileWriter writer;
    ok(writer.open(out, AudioFileFormat::wav, info, {}, 4096));
    ok(writer.write(AudioBlock<const double>(block.block(values.size()))));
    ok(writer.close());
    require(dataOf(out->str()) ==
                dataOf(integerWav(16, {expected.begin(), expected.end()})),
            "rounded and clipped integer export");
    // FLAC goes through CHOC's scale: codes are still exact, except the most negative
    // one, which that codec cannot store.
    const auto codes = sampledCodes(16, 7);
    auto flac = std::make_shared<std::stringstream>(std::ios::in | std::ios::out |
                                                    std::ios::binary);
    ok(writer.open(flac, AudioFileFormat::flac, info, {}, 4096));
    for (std::size_t at = 0; at < codes.size(); at += 4096) {
        const auto n = std::min<std::size_t>(4096, codes.size() - at);
        for (std::size_t i = 0; i < n; ++i)
            block.block().channel(0)[i] = double(codes[at + i]) / 32768;
        ok(writer.write(AudioBlock<const double>(block.block(n))));
    }
    ok(writer.close());
    AudioFileReader reader;
    ok(reader.open(std::static_pointer_cast<std::istream>(flac), 4096));
    require(reader.info()->frames == codes.size() && reader.info()->formatName == "FLAC",
            "FLAC integer shape");
    for (std::size_t at = 0; at < codes.size(); at += 4096) {
        const auto n = std::min<std::size_t>(4096, codes.size() - at);
        std::size_t count = 0;
        ok(reader.read(at, block.block(n), count));
        for (std::size_t i = 0; i < n; ++i)
            require(block.block().channel(0)[i] ==
                        double(std::max<std::int64_t>(codes[at + i], -32767)) / 32768,
                    "exact FLAC integer roundtrip");
    }
}
// Accepts a bounded number of bytes, then fails like a full disk. Rewriting bytes that
// already exist (header finalisation) keeps working, as it does on a real device.
struct LimitedBuffer : std::stringbuf {
    std::streamsize limit;
    explicit LimitedBuffer(std::streamsize bytes)
        : std::stringbuf(std::ios::in | std::ios::out | std::ios::binary), limit(bytes) {}
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        if (std::streamsize(pptr() - pbase()) + n > limit)
            return 0;
        return std::stringbuf::xsputn(s, n);
    }
    int_type overflow(int_type c) override {
        if (std::streamsize(pptr() - pbase()) + 1 > limit)
            return traits_type::eof();
        return std::stringbuf::overflow(c);
    }
};
void writeFailures() {
    AudioBuffer<float> block;
    ok(block.resize(2, 1024));
    std::uint32_t seed = 12345;
    for (std::size_t ch = 0; ch < 2; ++ch)
        for (auto& x : block.block().channel(ch)) {
            seed = seed * 1664525u + 1013904223u; // Incompressible content for the encoders.
            x = float(int(seed >> 8) % 20001 - 10000) / 16384;
        }
    for (const auto format : {AudioFileFormat::wav, AudioFileFormat::aiff, AudioFileFormat::caf,
                              AudioFileFormat::flac, AudioFileFormat::oggVorbis}) {
        LimitedBuffer buffer(format == AudioFileFormat::oggVorbis ? 32768 : 8192);
        auto stream = std::make_shared<std::ostream>(&buffer);
        AudioFileInfo info;
        info.sampleRate = 48000;
        info.channels = 2;
        info.encoding =
            format == AudioFileFormat::oggVorbis ? AudioEncoding::float32 : AudioEncoding::pcm16;
        AudioFileWriter writer;
        ok(writer.open(stream, format, info, {}, 1024));
        unsigned failures = 0, afterFailure = 0;
        for (unsigned i = 0; i < 512; ++i) {
            const auto status = writer.write(AudioBlock<const float>(block.block()));
            require(status == Status::ok || status == Status::io_error, "write failure status");
            if (failures && status == Status::ok)
                ++afterFailure;
            failures += status != Status::ok;
        }
        // Encoders may buffer, so the latest point a full device is reported is close.
        require(writer.close() == Status::io_error && !afterFailure,
                "a failed stream write is reported and sticky");
        if (format == AudioFileFormat::wav || format == AudioFileFormat::aiff ||
            format == AudioFileFormat::caf)
            require(failures == 511, "PCM write failure reported by the write that failed");
    }
}
void qualities() {
    const auto stream = [] {
        return std::make_shared<std::stringstream>(std::ios::in | std::ios::out |
                                                   std::ios::binary);
    };
    AudioFileInfo info;
    info.sampleRate = 48000;
    info.channels = 2;
    info.encoding = AudioEncoding::pcm16;
    AudioFileWriter writer;
    // Plain AIFF is big-endian by definition: it has no quality, and "little-endian"
    // used to produce a byte-swapped file under an AIFF header.
    auto refused = stream();
    require(writer.open(refused, AudioFileFormat::aiff, info, "little-endian") ==
                    Status::unsupported_format &&
                writer.open(refused, AudioFileFormat::aiff, info, "big-endian") ==
                    Status::unsupported_format &&
                writer.open(refused, AudioFileFormat::caf, info, "bogus") ==
                    Status::unsupported_format &&
                writer.open(refused, AudioFileFormat::wav, info, "bogus") ==
                    Status::unsupported_format &&
                writer.open(refused, AudioFileFormat::flac, info, "9") ==
                    Status::unsupported_format &&
                refused->str().empty(),
            "unknown quality refused before writing");
    ok(writer.open(stream(), AudioFileFormat::aiff, info));
    ok(writer.open(stream(), AudioFileFormat::aifc, info, "little-endian"));
    ok(writer.open(stream(), AudioFileFormat::caf, info, "big-endian"));
    ok(writer.open(stream(), AudioFileFormat::flac, info, "5"));
    ok(writer.open(stream(), AudioFileFormat::flac, info, "0"));
    ok(writer.open(stream(), AudioFileFormat::flac, info, "8 (Smallest)"));
    info.encoding = AudioEncoding::float32;
    ok(writer.open(stream(), AudioFileFormat::oggVorbis, info, "10"));
    require(writer.open(stream(), AudioFileFormat::oggVorbis, info, "11") ==
                Status::unsupported_format,
            "Ogg quality levels");
    ok(writer.close());
}
// The path overload never leaves a partial destination: validation happens first, the
// file is written beside the destination and swapped in when complete.
void atomicPaths() {
    const auto folder = cyaudio::testSupport::temporaryDirectory();
    const auto name =
        "cyaudio-atomic-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto path = folder / (name + ".wav");
    struct Cleanup {
        std::filesystem::path folder;
        std::string name;
        ~Cleanup() {
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
                if (entry.path().filename().string().starts_with(name))
                    std::filesystem::remove(entry.path(), ec);
        }
    } cleanup{folder, name};
    const auto content = [&] {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), {});
    };
    const auto siblings = [&] {
        unsigned count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(folder))
            count += entry.path().filename().string().starts_with(name) && entry.path() != path;
        return count;
    };
    {
        std::ofstream keep(path, std::ios::binary);
        keep << "KEEP";
    }
    AudioFileInfo info;
    info.sampleRate = 48000;
    info.channels = 1;
    info.encoding = AudioEncoding::pcm16;
    AudioBuffer<double> block;
    ok(block.resize(1, 64));
    for (std::size_t i = 0; i < 64; ++i)
        block.block().channel(0)[i] = double(i) / 128;
    AudioFileWriter writer;
    info.metadataJson = "{not json";
    require(writer.open(path, AudioFileFormat::wav, info) == Status::invalid_argument &&
                content() == "KEEP" && !siblings(),
            "invalid metadata refused before the destination is touched");
    info.metadataJson = R"([{"type":"data"}])";
    require(writer.open(path, AudioFileFormat::wav, info) == Status::unsupported_format &&
                content() == "KEEP" && !siblings(),
            "metadata refused by the codec leaves no file behind");
    info.metadataJson.clear();
    require(writer.open(path, AudioFileFormat::aiff, info, "little-endian") ==
                    Status::unsupported_format &&
                content() == "KEEP" && !siblings(),
            "refused quality leaves the destination intact");
    ok(writer.open(path, AudioFileFormat::wav, info, {}, 64));
    ok(writer.write(AudioBlock<const double>(block.block())));
    ok(writer.flush());
    require(content() == "KEEP" && siblings() == 1, "destination untouched until close");
    ok(writer.close());
    AudioFileReader reader;
    ok(reader.open(path, 64));
    require(!siblings() && reader.info()->frames == 64, "complete file published by close");
    reader.close();
    {
        AudioFileWriter scoped; // Destruction without close still publishes, best effort.
        ok(scoped.open(path, AudioFileFormat::wav, info, {}, 64));
        ok(scoped.write(AudioBlock<const double>(block.block(32))));
    }
    ok(reader.open(path, 64));
    require(!siblings() && reader.info()->frames == 32, "destruction publishes the file");
    require(reader.open(folder / (name + "-missing.wav"), 64) == Status::io_error &&
                reader.info()->frames == 32,
            "missing file is an I/O error, previous reader kept");
    reader.close();
    require(writer.open(folder / (name + "-missing-directory") / "x.wav",
                        AudioFileFormat::wav, info) == Status::io_error,
            "unwritable destination is an I/O error");
    // Peak caches are replaced the same way.
    WaveformPeakCache peaks, loaded;
    ok(peaks.build(path, 8));
    const auto cache = folder / (name + ".peaks");
    {
        std::ofstream old(cache, std::ios::binary);
        old << "stale";
    }
    ok(peaks.save(cache));
    ok(loaded.load(cache, path));
    require(loaded.levels() == peaks.levels() && loaded.info()->frames == 32 &&
                loaded.peaks(0).size() == 4 && loaded.peaks(0)[3].maximum == peaks.peaks(0)[3].maximum &&
                loaded.peaks(0)[0].minimum == 0 && loaded.peaks(0)[3].maximum == float(31.0 / 128),
            "peak cache replaced by a complete file");
}
// Seekable sink that counts everything and keeps only the head: enough to finalise and
// inspect the header of a file larger than 4 GiB without storing it.
struct SparseBuffer : std::streambuf {
    std::string head = std::string(4096, '\0');
    std::uint64_t position = 0, size = 0;
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        for (std::streamsize i = 0; i < n && position + std::uint64_t(i) < head.size(); ++i)
            head[std::size_t(position) + std::size_t(i)] = s[i];
        position += std::uint64_t(n);
        size = std::max(size, position);
        return n;
    }
    int_type overflow(int_type c) override {
        if (!traits_type::eq_int_type(c, traits_type::eof())) {
            const char byte = traits_type::to_char_type(c);
            xsputn(&byte, 1);
        }
        return traits_type::not_eof(c);
    }
    pos_type seekoff(off_type offset, std::ios_base::seekdir direction,
                     std::ios_base::openmode) override {
        const auto base = direction == std::ios_base::beg   ? off_type(0)
                          : direction == std::ios_base::cur ? off_type(position)
                                                            : off_type(size);
        position = std::uint64_t(base + offset);
        return pos_type(off_type(position));
    }
    pos_type seekpos(pos_type target, std::ios_base::openmode) override {
        position = std::uint64_t(off_type(target));
        return target;
    }
    std::uint64_t field(std::size_t at, unsigned bytes) const {
        std::uint64_t v = 0;
        for (unsigned i = 0; i < bytes; ++i)
            v |= std::uint64_t(static_cast<unsigned char>(head[at + i])) << (8 * i);
        return v;
    }
};
// Crossing 4 GiB turns the reserved JUNK chunk into ds64 and the form into RF64.
void rf64Writer() {
    SparseBuffer sink;
    auto stream = std::make_shared<std::ostream>(&sink);
    AudioFileInfo info;
    info.sampleRate = 48000;
    info.channels = 256;
    info.encoding = AudioEncoding::float64;
    AudioBuffer<double> block;
    ok(block.resize(256, 4096));
    clear(block.block());
    AudioFileWriter writer;
    ok(writer.open(stream, AudioFileFormat::wav, info, {}, 4096));
    std::uint64_t frames = 0;
    const auto append = [&] {
        ok(writer.write(AudioBlock<const double>(block.block())));
        frames += 4096;
    };
    append();
    ok(writer.flush());
    require(sink.head.substr(0, 4) == "RIFF" && sink.field(4, 4) == sink.size - 8 &&
                sink.head.substr(12, 4) == "JUNK" && sink.head.substr(96, 4) == "data" &&
                sink.field(100, 4) == frames * 2048,
            "RIFF header below 4 GiB");
    while (frames * 2048 <= 0x100000000ull)
        append();
    ok(writer.flush());
    require(sink.head.substr(0, 4) == "RF64" && sink.field(4, 4) == 0xffffffffu &&
                sink.head.substr(8, 8) == "WAVEds64" && sink.field(16, 4) == 28 &&
                sink.field(20, 8) == sink.size - 8 && sink.field(28, 8) == frames * 2048 &&
                sink.field(36, 8) == frames && sink.field(44, 4) == 0 &&
                sink.head.substr(48, 4) == "fmt " && sink.head.substr(96, 4) == "data" &&
                sink.field(100, 4) == 0xffffffffu && sink.size == 104 + frames * 2048,
            "RF64 ds64 header above 4 GiB");
    ok(writer.close());
    require(writer.framesWritten() == 0 && sink.size == 104 + frames * 2048, "RF64 close");
}
// Containers do not assume the stream starts at offset zero.
void streamOrigin() {
    AudioBuffer<double> block;
    ok(block.resize(1, 8));
    for (std::size_t i = 0; i < 8; ++i)
        block.block().channel(0)[i] = double(i) / 8 - .5;
    AudioFileInfo info;
    info.sampleRate = 44100;
    info.channels = 1;
    info.encoding = AudioEncoding::pcm24;
    for (const auto format : {AudioFileFormat::aiff, AudioFileFormat::aifc, AudioFileFormat::caf,
                              AudioFileFormat::wav}) {
        auto stream = std::make_shared<std::stringstream>(std::ios::in | std::ios::out |
                                                          std::ios::binary);
        *stream << "12345";
        AudioFileWriter writer;
        ok(writer.open(stream, format, info, {}, 8));
        ok(writer.write(AudioBlock<const double>(block.block())));
        ok(writer.close());
        const auto bytes = stream->str();
        require(bytes.substr(0, 5) == "12345", "prefix preserved");
        for (const bool offset : {false, true}) {
            auto input = std::make_shared<std::istringstream>(
                offset ? bytes : bytes.substr(5), std::ios::binary);
            if (offset)
                input->seekg(5);
            AudioFileReader reader;
            ok(reader.open(input, 8));
            AudioBuffer<double> out;
            ok(out.resize(1, 8));
            std::size_t n = 0;
            ok(reader.read(0, out.block(), n));
            for (std::size_t i = 0; i < 8; ++i)
                require(out.block().channel(0)[i] == block.block().channel(0)[i],
                        "container relative to the stream origin");
        }
    }
    // Truncated AIFF: the declared sizes are clamped and whole frames are kept.
    const std::array<unsigned char, 60> aiff{
        'F',  'O',  'R', 'M', 0,  0, 0, 52, 'A', 'I',  'F', 'F', 'C', 'O',  'M',
        'M',  0,    0,   0,   18, 0, 1, 0,  0,   0,    3,   0,   16,  0x40, 0x0e,
        0xbb, 0x80, 0,   0,   0,  0, 0, 0,  'S', 'S',  'N', 'D', 0,   0,    0,
        14,   0,    0,   0,   0,  0, 0, 0,  0,   0x80, 0,   0,   0,   0x7f, 0xff};
    const std::string whole(reinterpret_cast<const char*>(aiff.data()), aiff.size());
    AudioFileReader reader;
    ok(reader.open(std::make_shared<std::istringstream>(whole.substr(0, 59), std::ios::binary)));
    require(reader.info()->frames == 2, "truncated AIFF keeps whole frames");
    auto oversized = whole;
    oversized[6] = 9;
    ok(reader.open(std::make_shared<std::istringstream>(oversized, std::ios::binary)));
    require(reader.info()->frames == 3, "oversized FORM size clamped");
}
template <Sample T> void stretch() {
    constexpr std::size_t block = 256;
    const ProcessSpec spec{48000, block, 1};
    AudioBuffer<T> in, out;
    ok(in.resize(1, block));
    ok(out.resize(1, block));
    TimeStretch<T> engine;
    ok(engine.prepare(spec, block));
    ok(engine.setPitchSemitones(12));
    std::vector<double> signal;
    signal.reserve(48000);
    for (std::size_t start = 0; start < 48000; start += block) {
        for (std::size_t j = 0; j < block; ++j)
            in.block().channel(0)[j] = static_cast<T>(
                .3 * std::sin(2 * std::numbers::pi * 440 * static_cast<double>(start + j) / 48000));
        ok(engine.process(in.block(), out.block()));
        if (start > 12000)
            for (auto x : out.block().channel(0)) {
                require(isFinite(x), "stretch finite");
                signal.push_back(x);
            }
    }
    auto energy = [&](double hz) {
        std::complex<double> sum{};
        for (std::size_t i = 0; i < signal.size(); ++i)
            sum += signal[i] *
                   std::polar(1.0, -2 * std::numbers::pi * hz * static_cast<double>(i) / 48000);
        return std::abs(sum);
    };
    require(energy(880) > energy(440) * 20 && energy(880) > 100, "pitch octave spectral reference");
    // Own wrapper rejects aliases and preserves state, including nonzero view offsets.
    require(engine.process(in.block(), in.block()) == Status::partial_overlap,
            "stretch overlap rejection");
    struct Audit {
        TimeStretch<T>& engine;
        AudioBuffer<T>& in;
        AudioBuffer<T>& out;
        bool success = true;
    };
    Audit audit{engine, in, out};
    runRealtimeAudit(
        [](void* p) noexcept {
            auto& a = *static_cast<Audit*>(p);
            for (unsigned i = 0; i < 24; ++i)
                a.success =
                    a.engine.process(a.in.block(), a.out.block()) == Status::ok && a.success;
            a.engine.reset();
            a.success = a.engine.setPitchSemitones(-5) == Status::ok && a.success;
            a.success = a.engine.setFormant(1.2, true, 200) == Status::ok && a.success;
            for (unsigned i = 0; i < 32; ++i)
                a.success =
                    a.engine.process(a.in.block(), a.out.block()) == Status::ok && a.success;
            for (unsigned i = 0; i < 64; ++i) {
                const auto s = a.engine.flush(a.out.block());
                a.success = (s == Status::ok || s == Status::end_of_stream) && a.success;
            }
            a.engine.reset();
        },
        &audit);
    require(audit.success, "stretch realtime statuses");
    // Duration ratio and zero input tail are driven by independent input/output sizes.
    ok(engine.prepare(spec, 512));
    AudioBuffer<T> longer;
    ok(longer.resize(1, 512));
    ok(engine.process(in.block(), longer.block()));
    require(engine.inputLatency() > 0 && engine.outputLatency() > 0, "stretch reported latency");
    ok(engine.prepare({48000, 32768, 1}, 512));
    AudioBuffer<T> history;
    ok(history.resize(1, engine.inputLatency() + engine.outputLatency()));
    for (std::size_t i = 0; i < history.block().frames(); ++i)
        history.block().channel(0)[i] = static_cast<T>(.3 * std::sin(.02 * double(i)));
    struct PrimeAudit {
        TimeStretch<T>* engine;
        AudioBuffer<T>* history;
        AudioBuffer<T>* in;
        AudioBuffer<T>* out;
        Status result = Status::ok;
    } prime{&engine, &history, &in, &longer};
    runRealtimeAudit(
        [](void* ptr) noexcept {
            auto& p = *static_cast<PrimeAudit*>(ptr);
            p.result = p.engine->prime(p.history->block());
            if (p.result == Status::ok)
                p.result = p.engine->process(p.in->block(), p.out->block());
        },
        &prime);
    ok(prime.result);
}
void virtualDevice() {
    AudioDeviceContext context;
    ok(context.prepare(true));
    std::vector<AudioDeviceInfo> devices;
    ok(context.enumerate(devices));
    require(!devices.empty(), "null audio enumeration");
    struct Callback {
        std::atomic<unsigned> calls{0};
        std::atomic<bool> valid{true};
    };
    Callback state;
    AudioDevice device;
    AudioDeviceConfig config;
    config.maxCallbackFrames = 37;
    config.periodFrames = 256;
    auto callback = [](void* p, AudioBlock<const float> in, AudioBlock<float> out,
                       std::uint64_t) noexcept {
        auto& s = *static_cast<Callback*>(p);
        if (in.channels() != 0 || out.channels() != 2 || out.frames() > 37)
            s.valid.store(false);
        for (std::size_t ch = 0; ch < out.channels(); ++ch)
            for (auto x : out.channel(ch))
                if (x != 0)
                    s.valid.store(false);
        s.calls.fetch_add(1);
        return Status::ok;
    };
    ok(device.open(context, config, callback, &state));
    ok(device.start());
    for (unsigned i = 0; i < 100 && state.calls.load() < 20; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ok(device.stop());
    require(state.calls.load() >= 20 && state.valid.load() && device.callbackErrors() == 0 &&
                device.sampleRate() == 48000,
            "virtual device callback contract");
    config.outputId = "invalid";
    require(device.open(context, config, callback, &state) == Status::invalid_argument,
            "device ID validation");
    device.close();
    MidiDevicePorts ports;
    const auto status = enumerateMidiPorts(ports);
    require(status == Status::ok || status == Status::unsupported_backend ||
                status == Status::device_unavailable,
            "MIDI device enumeration status");
}
} // namespace
void ioRegression() {
    files();
    pcmExactness();
    writeFailures();
    qualities();
    atomicPaths();
    rf64Writer();
    streamOrigin();
    stretch<float>();
    stretch<double>();
    virtualDevice();
}
