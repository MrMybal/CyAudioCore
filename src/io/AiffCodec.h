#pragma once
#include "cyaudio/core/DspTypes.h"
// Original CyDSP PCM containers. Apple AIFF-C 1.0 and CAF 1.0 layouts.
// CAF flags are CAF-specific (little endian bit1), not AudioFormat ASBD flags.
#include <algorithm>
#include <array>
#include <bit>
#include <choc/audio/choc_AudioFileFormat.h>
#include <choc/text/choc_JSON.h>
#include <cmath>
#include <limits>
namespace cyaudio::detail {
namespace aiff {
inline std::uint64_t read(std::istream& s, unsigned n) {
    std::uint64_t v = 0;
    while (n--) {
        char c = 0;
        s.get(c);
        v = (v << 8) | static_cast<unsigned char>(c);
    }
    return v;
}
inline void write(std::ostream& s, std::uint64_t v, unsigned n) {
    while (n--)
        s.put(static_cast<char>(v >> (n * 8)));
}
inline double rate(std::istream& s) {
    const auto exponent = read(s, 2);
    const auto mantissa = read(s, 8);
    if (exponent & 0x8000 || exponent == 0x7fff || (exponent && !(mantissa >> 63)))
        return 0;
    return std::ldexp(static_cast<double>(mantissa),
                      static_cast<int>(exponent ? exponent : 1) - 16383 - 63);
}
inline void rate(std::ostream& s, double hz) {
    int exponent = 0;
    const auto fraction = std::frexp(hz, &exponent);
    write(s, static_cast<std::uint64_t>(exponent + 16382), 2);
    write(s, static_cast<std::uint64_t>(std::ldexp(fraction, 64)), 8);
}

enum class Container { aiff, aifc, caf };
struct Metadata {
    std::uint32_t type = 0;
    std::string bytes;
};
inline bool structural(Container f, std::uint32_t t) {
    return f == Container::caf ? (t == 0x64657363 || t == 0x64617461 || t == 0x70616b74)
                               : (t == 0x434f4d4d || t == 0x53534e44 || t == 0x46564552);
}
inline std::string tag(std::uint32_t t) {
    std::string s(4, ' ');
    for (unsigned i = 0; i < 4; ++i)
        s[i] = char(t >> (24 - 8 * i));
    return s;
}
inline choc::value::Value metadata(const std::vector<Metadata>& chunks) {
    auto array = choc::value::createEmptyArray();
    constexpr char digits[] = "0123456789abcdef";
    for (auto& c : chunks) {
        std::string hex;
        hex.reserve(c.bytes.size() * 2);
        for (unsigned char b : c.bytes) {
            hex += digits[b >> 4];
            hex += digits[b & 15];
        }
        array.addArrayElement(choc::json::create("type", tag(c.type), "hex", hex));
    }
    return array;
}
inline bool metadata(choc::value::ValueView array, Container f, std::vector<Metadata>& out) {
    if (array.isVoid())
        return true;
    if (!array.isArray() || array.size() > 4096)
        return false;
    std::size_t total = 0;
    for (auto c : array) {
        if (!c.isObject() || !c["type"].isString() || !c["hex"].isString())
            return false;
        const auto t = c["type"].getString(), hex = c["hex"].getString();
        if (t.size() != 4 || hex.size() % 2 || hex.size() > 33554432 ||
            total > 16777216 - hex.size() / 2)
            return false;
        Metadata m;
        for (unsigned char b : t)
            m.type = (m.type << 8) | b;
        if (structural(f, m.type))
            return false;
        auto digit = [](char v) -> int {
            return v >= '0' && v <= '9'   ? v - '0'
                   : v >= 'a' && v <= 'f' ? v - 'a' + 10
                   : v >= 'A' && v <= 'F' ? v - 'A' + 10
                                          : -1;
        };
        m.bytes.resize(hex.size() / 2);
        for (std::size_t i = 0; i < m.bytes.size(); ++i) {
            auto a = digit(hex[2 * i]), b = digit(hex[2 * i + 1]);
            if (a < 0 || b < 0)
                return false;
            m.bytes[i] = char((a << 4) | b);
        }
        total += m.bytes.size();
        out.push_back(std::move(m));
    }
    return true;
}
// Frames move through one retained block: one stream call per block, never per byte.
constexpr std::size_t blockBytes = 65536;
// Round half away from zero, clamped to [-2^(bits-1), 2^(bits-1) - 1].
inline std::int64_t quantise(double v, double scale) noexcept {
    const auto x = v * scale;
    if (x >= scale - 1)
        return std::int64_t(scale - 1);
    if (x <= -scale)
        return std::int64_t(-scale);
    auto i = std::int64_t(x);
    const auto fraction = x - double(i);
    if (fraction >= 0.5)
        ++i;
    else if (fraction <= -0.5)
        --i;
    return i;
}
struct Reader : choc::audio::AudioFileReader {
    std::shared_ptr<std::istream> stream;
    choc::audio::AudioFileProperties properties;
    Container container = Container::aiff;
    std::uint64_t start = 0;
    unsigned bits = 0;
    bool little = false, floating = false;
    std::vector<char> raw;
    const choc::audio::AudioFileProperties& getProperties() override {
        return properties;
    }
    bool initialise() {
        auto& s = *stream;
        s.clear();
        const auto first = s.tellg();
        s.seekg(0, std::ios::end);
        const auto end = s.tellg();
        if (first < 0 || end < first || end - first < 12)
            return false;
        s.seekg(first);
        // Offsets are relative to the position the stream was handed over at.
        const auto origin = std::uint64_t(std::streamoff(first)),
                   length = std::uint64_t(std::streamoff(end - first));
        std::uint64_t limit = length, at = 0;
        if (container == Container::caf) {
            if (read(s, 4) != 0x63616666 || read(s, 2) != 1 || read(s, 2) != 0)
                return false;
            at = 8;
        } else {
            if (read(s, 4) != 0x464f524d)
                return false;
            const auto size = read(s, 4);
            if (size < 4 ||
                read(s, 4) != (container == Container::aiff ? 0x41494646u : 0x41494643u))
                return false;
            // A declared size beyond the stream (truncated file) is clamped, never trusted.
            limit = std::min(size + 8, length);
            at = 12;
        }
        bool desc = false, data = false, version = container != Container::aifc;
        std::uint64_t bytes = 0;
        std::size_t totalMeta = 0;
        std::vector<Metadata> chunks;
        const auto isData = [&](std::uint32_t t) {
            return t == (container == Container::caf ? 0x64617461u : 0x53534e44u);
        };
        while (at < limit) {
            const unsigned header = container == Container::caf ? 12 : 8;
            if (limit - at < header)
                break; // Trailing bytes that cannot hold a chunk header.
            s.seekg(std::streamoff(origin + at));
            const auto type = std::uint32_t(read(s, 4));
            auto n = read(s, header - 4);
            at += header;
            if (container == Container::caf && n == UINT64_MAX && type == 0x64617461)
                n = limit - at;
            if (n > limit - at) {
                // Only the sample chunk of a truncated file is kept, shortened to the stream.
                if (structural(container, type) && !isData(type))
                    return false;
                if (!isData(type))
                    break;
                n = limit - at;
            }
            if (container == Container::caf && type == 0x64657363) {
                if (desc || at != 20 || n != 32)
                    return false;
                desc = true;
                properties.sampleRate = std::bit_cast<double>(read(s, 8));
                if (read(s, 4) != 0x6c70636d)
                    return false;
                const auto flags = read(s, 4), packet = read(s, 4), frames = read(s, 4);
                properties.numChannels = std::uint32_t(read(s, 4));
                bits = unsigned(read(s, 4));
                if (flags > 3 || frames != 1 || !properties.numChannels || bits % 8 ||
                    packet != properties.numChannels * (bits / 8))
                    return false;
                floating = (flags & 1) != 0;
                little = (flags & 2) != 0;
            } else if (container != Container::caf && type == 0x434f4d4d) {
                if (desc || n < (container == Container::aifc ? 24u : 18u) ||
                    (container == Container::aiff && n != 18))
                    return false;
                desc = true;
                properties.numChannels = std::uint32_t(read(s, 2));
                properties.numFrames = read(s, 4);
                bits = unsigned(read(s, 2));
                properties.sampleRate = rate(s);
                if (container == Container::aifc) {
                    const auto compression = read(s, 4), nameLength = read(s, 1);
                    if (nameLength > n - 23)
                        return false;
                    little = compression == 0x736f7774;
                    floating = compression == 0x666c3332 || compression == 0x464c3332 ||
                               compression == 0x666c3634 || compression == 0x464c3634;
                    if (!little && !floating && compression != 0x4e4f4e45 &&
                        compression != 0x74776f73)
                        return false;
                    if (floating && bits != ((compression & 0xffff) == 0x3332 ? 32u : 64u))
                        return false;
                }
            } else if (container != Container::caf && type == 0x46564552) {
                if (n != 4 || read(s, 4) != 0xa2805140)
                    return false;
                version = true;
            } else if ((container == Container::caf && type == 0x64617461) ||
                       (container != Container::caf && type == 0x53534e44)) {
                const unsigned prefix = container == Container::caf ? 4 : 8;
                if (data || n < prefix)
                    return false;
                data = true;
                std::uint64_t offset = 0;
                if (container == Container::caf)
                    (void)read(s, 4);
                else {
                    offset = read(s, 4);
                    (void)read(s, 4);
                }
                if (offset > n - prefix)
                    return false;
                start = origin + at + prefix + offset;
                bytes = n - prefix - offset;
            } else if (!structural(container, type)) {
                // An oversized optional chunk (large artwork tag...) is skipped, not fatal.
                if (n <= 16777216 && chunks.size() < 4096 && totalMeta <= 16777216 - n) {
                    Metadata m;
                    m.type = type;
                    m.bytes.resize(std::size_t(n));
                    s.read(m.bytes.data(), std::streamsize(n));
                    totalMeta += std::size_t(n);
                    chunks.push_back(std::move(m));
                }
            } else
                return false;
            if (!s)
                return false;
            const auto pad = container == Container::caf ? 0 : (n & 1);
            at += std::min(n + pad, limit - at); // A missing final pad byte is accepted.
        }
        if (!desc || !version || !data || !properties.numChannels || properties.numChannels > 256 ||
            !std::isfinite(properties.sampleRate) || properties.sampleRate <= 0 ||
            properties.sampleRate > 1e9 ||
            (floating ? (bits != 32 && bits != 64)
                      : (bits != 8 && bits != 16 && bits != 24 && bits != 32)))
            return false;
        const auto frameBytes = properties.numChannels * (bits / 8);
        // Whole frames only: a partial last frame or a truncated payload shortens the file.
        if (container == Container::caf)
            properties.numFrames = bytes / frameBytes;
        else
            properties.numFrames = std::min<std::uint64_t>(properties.numFrames, bytes / frameBytes);
        raw.resize(std::max<std::size_t>(frameBytes, blockBytes / frameBytes * frameBytes));
        using D = choc::audio::BitDepth;
        properties.bitDepth = floating ? (bits == 32 ? D::float32 : D::float64)
                                       : choc::audio::getIntegerBitDepth(std::uint16_t(bits));
        properties.formatName = container == Container::caf    ? "CAF"
                                : container == Container::aifc ? "AIFC"
                                                               : "AIFF";
        properties.metadata = metadata(chunks);
        return true;
    }
    template <class T> bool frames(std::uint64_t first, choc::buffer::ChannelArrayView<T> out) {
        if (out.getNumChannels() != properties.numChannels)
            return false;
        const auto n =
            first < properties.numFrames
                ? std::min<std::uint64_t>(out.getNumFrames(), properties.numFrames - first)
                : 0;
        auto& s = *stream;
        s.clear();
        const unsigned width = bits / 8;
        const std::size_t frameBytes = std::size_t(properties.numChannels) * width;
        if (n)
            s.seekg(std::streamoff(start + first * frameBytes));
        const auto scale = std::ldexp(1.0, 1 - int(bits)); // Exactly value / 2^(bits-1).
        const unsigned shift = 64 - bits;
        for (std::uint64_t done = 0; done < n;) {
            const auto count =
                std::size_t(std::min<std::uint64_t>(n - done, raw.size() / frameBytes));
            s.read(raw.data(), std::streamsize(count * frameBytes));
            if (!s)
                return false;
            for (std::uint32_t ch = 0; ch < properties.numChannels; ++ch) {
                auto* p = reinterpret_cast<const unsigned char*>(raw.data()) + ch * width;
                for (std::size_t i = 0; i < count; ++i, p += frameBytes) {
                    std::uint64_t v = 0;
                    if (little)
                        for (unsigned b = width; b--;)
                            v = (v << 8) | p[b];
                    else
                        for (unsigned b = 0; b < width; ++b)
                            v = (v << 8) | p[b];
                    const double value =
                        !floating    ? double(std::int64_t(v << shift) >> shift) * scale
                        : bits == 32 ? double(std::bit_cast<float>(std::uint32_t(v)))
                                     : std::bit_cast<double>(v);
                    out.getSample(ch, std::uint32_t(done + i)) = static_cast<T>(value);
                }
            }
            done += count;
        }
        for (auto i = std::uint32_t(n); i < out.getNumFrames(); ++i)
            for (std::uint32_t ch = 0; ch < properties.numChannels; ++ch)
                out.getSample(ch, i) = 0;
        return true;
    }
    bool readFrames(std::uint64_t i, choc::buffer::ChannelArrayView<float> b) override {
        return frames(i, b);
    }
    bool readFrames(std::uint64_t i, choc::buffer::ChannelArrayView<double> b) override {
        return frames(i, b);
    }
};
struct Writer : choc::audio::AudioFileWriter {
    std::shared_ptr<std::ostream> stream;
    choc::audio::AudioFileProperties properties;
    std::vector<Metadata> chunks;
    Container container = Container::aiff;
    unsigned bits = 16;
    bool floating = false, little = false;
    // Absolute stream positions; origin is where the stream was handed over.
    std::uint64_t origin = 0, start = 0, dataSizeOffset = 0;
    std::vector<char> raw;
    const choc::audio::AudioFileProperties& getProperties() override {
        return properties;
    }
    ~Writer() override {
        try {
            flush();
        } catch (...) {
        }
    }
    std::uint64_t bytes() const {
        return properties.numFrames * properties.numChannels * (bits / 8);
    }
    bool initialise() {
        auto& s = *stream;
        const auto first = s.tellp();
        if (first < 0)
            return false;
        origin = std::uint64_t(std::streamoff(first));
        const std::size_t frameBytes = std::size_t(properties.numChannels) * (bits / 8);
        raw.resize(std::max<std::size_t>(frameBytes, blockBytes / frameBytes * frameBytes));
        if (container == Container::caf) {
            write(s, 0x63616666, 4);
            write(s, 1, 2);
            write(s, 0, 2);
            write(s, 0x64657363, 4);
            write(s, 32, 8);
            write(s, std::bit_cast<std::uint64_t>(properties.sampleRate), 8);
            write(s, 0x6c70636d, 4);
            write(s, (floating ? 1 : 0) | (little ? 2 : 0), 4);
            write(s, properties.numChannels * (bits / 8), 4);
            write(s, 1, 4);
            write(s, properties.numChannels, 4);
            write(s, bits, 4);
        } else {
            write(s, 0x464f524d, 4);
            write(s, 0, 4);
            write(s, container == Container::aiff ? 0x41494646 : 0x41494643, 4);
            if (container == Container::aifc) {
                write(s, 0x46564552, 4);
                write(s, 4, 4);
                write(s, 0xa2805140, 4);
            }
            write(s, 0x434f4d4d, 4);
            write(s, container == Container::aifc ? 24 : 18, 4);
            write(s, properties.numChannels, 2);
            write(s, 0, 4);
            write(s, bits, 2);
            rate(s, properties.sampleRate);
            if (container == Container::aifc) {
                write(s,
                      floating ? (bits == 32 ? 0x666c3332 : 0x666c3634)
                      : little ? 0x736f7774
                               : 0x4e4f4e45,
                      4);
                write(s, 0, 2);
            }
        }
        for (auto& c : chunks) {
            write(s, c.type, 4);
            write(s, c.bytes.size(), container == Container::caf ? 8 : 4);
            s.write(c.bytes.data(), std::streamsize(c.bytes.size()));
            if (container != Container::caf && (c.bytes.size() & 1))
                s.put(0);
        }
        write(s, container == Container::caf ? 0x64617461 : 0x53534e44, 4);
        dataSizeOffset = std::uint64_t(s.tellp());
        write(s, container == Container::caf ? 4 : 8, container == Container::caf ? 8 : 4);
        write(s, 0, container == Container::caf ? 4 : 8);
        start = std::uint64_t(s.tellp());
        return bool(s) && flush();
    }
    bool flush() override {
        if (!start)
            return false;
        auto& s = *stream;
        const auto n = bytes();
        s.seekp(std::streamoff(dataSizeOffset));
        write(s, n + (container == Container::caf ? 4 : 8), container == Container::caf ? 8 : 4);
        if (container != Container::caf) {
            s.seekp(std::streamoff(origin + 4));
            write(s, start - origin + n + (n & 1) - 8, 4);
            s.seekp(std::streamoff(origin + (container == Container::aifc ? 34 : 22)));
            write(s, properties.numFrames, 4);
            s.seekp(std::streamoff(start + n));
            if (n & 1)
                s.put(0);
        }
        s.flush();
        return bool(s);
    }
    template <class T> bool frames(choc::buffer::ChannelArrayView<const T> in) {
        if (in.getNumChannels() != properties.numChannels)
            return false;
        const auto count = std::uint64_t(in.getNumFrames());
        const unsigned width = bits / 8;
        const std::size_t frameBytes = std::size_t(properties.numChannels) * width;
        const auto limit =
            container == Container::caf ? std::uint64_t(INT64_MAX) : std::uint64_t(0xffffffff);
        const auto header = start - origin; // Container sizes are relative to the stream origin.
        if (header > limit || bytes() >= limit - header ||
            count >
                (limit - header - bytes() - ((container == Container::caf) ? 0 : 1)) / frameBytes ||
            (container != Container::caf && count > 0xffffffff - properties.numFrames))
            return false;
        // Nothing is written for a block that contains a non-finite sample.
        for (std::uint32_t ch = 0; ch < properties.numChannels; ++ch)
            for (std::uint32_t i = 0; i < in.getNumFrames(); ++i)
                if (!std::isfinite(double(in.getSample(ch, i))))
                    return false;
        auto& s = *stream;
        s.seekp(std::streamoff(start + bytes()));
        const auto scale = std::ldexp(1.0, int(bits - 1));
        for (std::uint64_t done = 0; done < count;) {
            const auto block =
                std::size_t(std::min<std::uint64_t>(count - done, raw.size() / frameBytes));
            for (std::uint32_t ch = 0; ch < properties.numChannels; ++ch) {
                auto* p = reinterpret_cast<unsigned char*>(raw.data()) + ch * width;
                for (std::size_t i = 0; i < block; ++i, p += frameBytes) {
                    const double v = in.getSample(ch, std::uint32_t(done + i));
                    const auto value =
                        !floating    ? std::uint64_t(quantise(v, scale))
                        : bits == 32 ? std::uint64_t(std::bit_cast<std::uint32_t>(float(v)))
                                     : std::bit_cast<std::uint64_t>(v);
                    if (little)
                        for (unsigned b = 0; b < width; ++b)
                            p[b] = static_cast<unsigned char>(value >> (8 * b));
                    else
                        for (unsigned b = 0; b < width; ++b)
                            p[b] = static_cast<unsigned char>(value >> (8 * (width - 1 - b)));
                }
            }
            s.write(raw.data(), std::streamsize(block * frameBytes));
            if (!s)
                return false;
            properties.numFrames += block; // Only frames that reached the stream are declared.
            done += block;
        }
        return true;
    }
    bool appendFrames(choc::buffer::ChannelArrayView<const float> b) override {
        return frames(b);
    }
    bool appendFrames(choc::buffer::ChannelArrayView<const double> b) override {
        return frames(b);
    }
};
} // namespace aiff
class AiffCodec : public choc::audio::AudioFileFormat {
    aiff::Container container_;

  public:
    explicit AiffCodec(aiff::Container f = aiff::Container::aiff) : container_(f) {}
    std::string getFileSuffixes() override {
        return container_ == aiff::Container::caf    ? "caf"
               : container_ == aiff::Container::aifc ? "aifc"
                                                     : "aif aiff";
    }
    std::uint32_t getMaximumNumChannels() override {
        return 256;
    }
    std::vector<double> getSupportedSampleRates() override {
        return {};
    }
    std::vector<choc::audio::BitDepth> getSupportedBitDepths() override {
        using D = choc::audio::BitDepth;
        if (container_ == aiff::Container::aiff)
            return {D::int8, D::int16, D::int24, D::int32};
        return {D::int8, D::int16, D::int24, D::int32, D::float32, D::float64};
    }
    std::vector<std::string> getQualityLevels() override {
        return container_ == aiff::Container::aiff
                   ? std::vector<std::string>{}
                   : std::vector<std::string>{"big-endian", "little-endian"};
    }
    bool supportsWriting() const override {
        return true;
    }
    std::unique_ptr<choc::audio::AudioFileReader>
    createReader(std::shared_ptr<std::istream> s) override {
        if (!s)
            return {};
        auto r = std::make_unique<aiff::Reader>();
        r->container = container_;
        r->stream = std::move(s);
        return r->initialise() ? std::move(r) : nullptr;
    }
    std::unique_ptr<choc::audio::AudioFileWriter>
    createWriter(std::shared_ptr<std::ostream> s, choc::audio::AudioFileProperties p) override {
        const auto bits = choc::audio::getBytesPerSample(p.bitDepth) * 8;
        const auto floating = choc::audio::isFloat(p.bitDepth);
        // Plain AIFF is big-endian integer PCM only; the byte order is a quality of AIFC/CAF.
        if (!s || !*s || !p.numChannels || p.numChannels > 256 || !std::isfinite(p.sampleRate) ||
            p.sampleRate <= 0 || p.sampleRate > 1e9 || !bits ||
            (!p.quality.empty() && p.quality != "big-endian" && p.quality != "little-endian") ||
            (container_ == aiff::Container::aiff && (floating || !p.quality.empty())) ||
            (container_ == aiff::Container::aifc && floating && p.quality == "little-endian"))
            return {};
        auto w = std::make_unique<aiff::Writer>();
        w->container = container_;
        w->bits = bits;
        w->floating = floating;
        w->little = p.quality == "little-endian";
        w->stream = std::move(s);
        w->properties = std::move(p);
        if (!aiff::metadata(w->properties.metadata, container_, w->chunks))
            return {};
        w->properties.numFrames = 0;
        w->properties.formatName = container_ == aiff::Container::caf    ? "CAF"
                                   : container_ == aiff::Container::aifc ? "AIFC"
                                                                         : "AIFF";
        return w->initialise() ? std::move(w) : nullptr;
    }
};
} // namespace cyaudio::detail
