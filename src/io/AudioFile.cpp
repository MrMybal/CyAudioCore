#include "cyaudio/core/DspTypes.h"
// Private CHOC codecs with owned WAV metadata adaptation; notices: third_party/licenses/choc.txt
#include "cyaudio/io/AudioFile.h"
#include "AiffCodec.h"
#include "PlatformAudioCodec.h"
#include "cydsp/core/AudioBuffer.h"
#include <algorithm>
#include <choc/audio/choc_AudioFileFormat_FLAC.h>
#include <choc/audio/choc_AudioFileFormat_MP3.h>
#include <choc/audio/choc_AudioFileFormat_Ogg.h>
#include "WavCodec.h"
#include <cmath>
#include <fstream>
#include <limits>
#include <system_error>
namespace cyaudio {
namespace {
using Depth = choc::audio::BitDepth;
Depth depth(AudioEncoding e) {
    return static_cast<Depth>(static_cast<unsigned>(e) + 1);
}
AudioEncoding encoding(Depth d) {
    return static_cast<AudioEncoding>(static_cast<unsigned>(d) - 1);
}
std::unique_ptr<choc::audio::AudioFileFormat> format(AudioFileFormat f) {
    switch (f) {
    case AudioFileFormat::wav:
        return std::make_unique<choc::audio::CyDSPWavFormat<true>>();
    case AudioFileFormat::flac:
        return std::make_unique<choc::audio::FLACAudioFileFormat<true>>();
    case AudioFileFormat::oggVorbis:
        return std::make_unique<choc::audio::OggAudioFileFormat<true>>();
    case AudioFileFormat::aiff:
        return std::make_unique<detail::AiffCodec>();
    case AudioFileFormat::aifc:
        return std::make_unique<detail::AiffCodec>(detail::aiff::Container::aifc);
    case AudioFileFormat::caf:
        return std::make_unique<detail::AiffCodec>(detail::aiff::Container::caf);
    case AudioFileFormat::mp3:
        return std::make_unique<choc::audio::MP3AudioFileFormat>();
    case AudioFileFormat::m4a:
    case AudioFileFormat::aac:
    case AudioFileFormat::wma:
        return {};
    }
    return {};
}
choc::audio::AudioFileFormatList formats() {
    choc::audio::AudioFileFormatList list;
    for (auto f :
         {AudioFileFormat::wav, AudioFileFormat::flac, AudioFileFormat::oggVorbis,
          AudioFileFormat::aiff, AudioFileFormat::aifc, AudioFileFormat::caf, AudioFileFormat::mp3})
        list.addFormat(format(f));
    return list;
}
// Integer width of an encoding, zero for float data.
unsigned integerBits(AudioEncoding e) {
    constexpr unsigned bits[]{8, 16, 24, 32};
    const auto n = static_cast<unsigned>(e);
    return n <= static_cast<unsigned>(AudioEncoding::pcm32) ? bits[n] : 0;
}
bool valid(const AudioFileInfo& info, std::size_t max) {
    return isFinite(info.sampleRate) && info.sampleRate > 0 && info.sampleRate <= 1e9 &&
           info.channels > 0 && info.channels <= 256 && max > 0 && max <= 1048576 &&
           static_cast<unsigned>(info.encoding) <= static_cast<unsigned>(AudioEncoding::float64);
}
// A source without metadata is reported as an empty string; "null" (older reader
// output) is accepted as the same thing.
bool emptyMetadata(std::string_view json) {
    return json.empty() || json == "null";
}
// Empty selects the codec default. Otherwise the text must be one of the advertised
// levels, or the leading token of one ("0" for FLAC's "0 (Fastest)").
bool knownQuality(choc::audio::AudioFileFormat& codec, const std::string& quality) {
    if (quality.empty())
        return true;
    for (const auto& level : codec.getQualityLevels())
        if (level == quality || level.substr(0, level.find(' ')) == quality)
            return true;
    return false;
}
// Everything that can be rejected before a destination is created or written.
struct WritePlan {
    std::unique_ptr<choc::audio::AudioFileFormat> codec;
    choc::audio::AudioFileProperties properties;
    unsigned flacBits = 0;
};
Status plan(AudioFileFormat f, const AudioFileInfo& info, std::string quality, WritePlan& out) {
    out.codec = format(f);
    if (!out.codec || !out.codec->supportsWriting())
        return Status::unsupported_format;
    const auto depths = out.codec->getSupportedBitDepths();
    if ((f == AudioFileFormat::oggVorbis
             ? info.encoding != AudioEncoding::float32
             : std::find(depths.begin(), depths.end(), depth(info.encoding)) == depths.end()) ||
        info.channels > out.codec->getMaximumNumChannels() || !knownQuality(*out.codec, quality))
        return Status::unsupported_format;
    auto& p = out.properties;
    p.sampleRate = info.sampleRate;
    p.numChannels = static_cast<std::uint32_t>(info.channels);
    p.bitDepth = f == AudioFileFormat::oggVorbis ? Depth::int16 : depth(info.encoding);
    p.quality = std::move(quality);
    if (!emptyMetadata(info.metadataJson)) {
        try {
            p.metadata = choc::json::parse(info.metadataJson);
        } catch (const std::bad_alloc&) {
            throw;
        } catch (...) {
            return Status::invalid_argument;
        }
    }
    // CHOC's FLAC codec scales by 2^(bits-1)-1 and truncates; see writeImpl/readImpl.
    if (f == AudioFileFormat::flac)
        out.flacBits = integerBits(info.encoding);
    return Status::ok;
}
// Removes an unpublished temporary unless its path was taken over. Declared before the
// objects that hold the file open, so that it runs after they have closed it.
struct Discard {
    std::filesystem::path path;
    ~Discard() {
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }
};
} // namespace
std::vector<AudioFormatCapabilities> audioFileFormats() {
    std::vector<AudioFormatCapabilities> result;
    for (auto f : {AudioFileFormat::wav, AudioFileFormat::flac, AudioFileFormat::oggVorbis,
                   AudioFileFormat::aiff, AudioFileFormat::aifc, AudioFileFormat::caf,
                   AudioFileFormat::mp3}) {
        auto codec = format(f);
        AudioFormatCapabilities c{f,
                                  true,
                                  codec->supportsWriting(),
                                  std::min<std::size_t>(codec->getMaximumNumChannels(),
                                                        f == AudioFileFormat::flac ? 8 : 256),
                                  {},
                                  codec->getQualityLevels()};
        if (c.write)
            for (auto d : codec->getSupportedBitDepths())
                if (d != Depth::unknown)
                    c.writableEncodings.push_back(encoding(d));
        if (f == AudioFileFormat::oggVorbis)
            c.writableEncodings = {AudioEncoding::float32};
        result.push_back(std::move(c));
    }
    for (auto f : {AudioFileFormat::m4a, AudioFileFormat::aac, AudioFileFormat::wma})
        result.push_back({f,
                          detail::platformDecodingAvailable(f),
                          detail::platformEncodingAvailable(f),
                          2,
                          {AudioEncoding::pcm16},
                          {"128", "192", "256", "320"}});
    for (auto& c : result)
        if (c.format == AudioFileFormat::mp3 && detail::platformEncodingAvailable(c.format)) {
            c.write = true;
            c.writableEncodings = {AudioEncoding::pcm16};
            c.qualityLevels = {"128", "192", "256", "320"};
        }
    return result;
}
struct AudioFileReader::State {
    std::unique_ptr<choc::audio::AudioFileReader> reader;
    AudioFileInfo info;
    std::size_t max = 0;
    // FLAC only: CHOC returns code / (2^(bits-1)-1). The exact code is recovered and
    // renormalised to the documented code / 2^(bits-1).
    double flacMaximum = 0, flacScale = 0;
    std::vector<float*> f;
    std::vector<double*> d;
    Status initialise(std::size_t blockFrames) {
        const auto& p = reader->getProperties();
        info = {p.formatName,
                p.sampleRate,
                p.numFrames,
                p.numChannels,
                p.bitDepth == Depth::unknown ? AudioEncoding::float32 : encoding(p.bitDepth),
                p.metadata.isVoid() ? std::string{} : choc::json::toString(p.metadata)};
        if (p.formatName == "Ogg-Vorbis" || p.formatName == "MP3")
            info.encoding = AudioEncoding::float32;
        if (!valid(info, blockFrames))
            return Status::invalid_argument;
        if (p.formatName == "FLAC" && p.bitDepth != Depth::unknown)
            if (const auto bits = integerBits(info.encoding)) {
                flacMaximum = std::ldexp(1.0, static_cast<int>(bits) - 1) - 1;
                flacScale = std::ldexp(1.0, 1 - static_cast<int>(bits));
            }
        f.resize(p.numChannels);
        d.resize(p.numChannels);
        max = blockFrames;
        return Status::ok;
    }
};
AudioFileReader::AudioFileReader() = default;
AudioFileReader::~AudioFileReader() = default;
AudioFileReader::AudioFileReader(AudioFileReader&&) noexcept = default;
AudioFileReader& AudioFileReader::operator=(AudioFileReader&&) noexcept = default;
Status AudioFileReader::open(const std::filesystem::path& path, std::size_t max) noexcept {
    if (!max || max > 1048576)
        return Status::invalid_argument;
    try {
        auto file = std::make_shared<std::ifstream>(path, std::ios::binary);
        if (!*file)
            return Status::io_error; // Missing or unreadable: not an argument error.
        const auto portable = open(std::move(file), max);
        if (portable != Status::unsupported_format)
            return portable;
        auto next = std::make_unique<State>();
        next->reader = detail::platformReader(path);
        if (!next->reader)
            return Status::unsupported_format;
        const auto ready = next->initialise(max);
        if (ready != Status::ok)
            return ready;
        state_.swap(next);
        return Status::ok;
    } catch (const std::bad_alloc&) {
        return Status::allocation_failed;
    } catch (...) {
        return Status::io_error;
    }
}
Status AudioFileReader::open(std::shared_ptr<std::istream> stream, std::size_t max) noexcept {
    if (!stream || !*stream || !max || max > 1048576)
        return Status::invalid_argument;
    try {
        auto next = std::make_unique<State>();
        next->reader = formats().createReader(std::move(stream));
        if (!next->reader)
            return Status::unsupported_format;
        const auto ready = next->initialise(max);
        if (ready != Status::ok)
            return ready;
        state_.swap(next);
        return Status::ok;
    } catch (const std::bad_alloc&) {
        return Status::allocation_failed;
    } catch (...) {
        return Status::io_error;
    }
}
void AudioFileReader::close() noexcept {
    state_.reset();
}
const AudioFileInfo* AudioFileReader::info() const noexcept {
    return state_ ? &state_->info : nullptr;
}
template <Sample T>
Status AudioFileReader::readImpl(std::uint64_t first, AudioBlock<T> out,
                                 std::size_t& count) noexcept {
    count = 0;
    if (!state_)
        return Status::not_prepared;
    auto& s = *state_;
    const auto status =
        validateProcessBlocks<T>({s.info.sampleRate, s.max, s.info.channels}, out, out);
    if (status != Status::ok)
        return status;
    if (!out.frames())
        return Status::ok;
    const auto available = first < s.info.frames ? s.info.frames - first : 0;
    const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(available, out.frames()));
    try {
        if (n) {
            auto& pointers = [&]() -> auto& {
                if constexpr (std::same_as<T, float>)
                    return s.f;
                else
                    return s.d;
            }();
            for (std::size_t ch = 0; ch < out.channels(); ++ch)
                pointers[ch] = out.channel(ch).data();
            if (!s.reader->readFrames(first, choc::buffer::createChannelArrayView(
                                                 pointers.data(),
                                                 static_cast<std::uint32_t>(out.channels()),
                                                 static_cast<std::uint32_t>(n))))
                return Status::io_error;
        }
        for (std::size_t ch = 0; ch < out.channels(); ++ch) {
            const auto channel = out.channel(ch);
            if (s.flacScale != 0)
                for (std::size_t i = 0; i < n; ++i)
                    channel[i] = static_cast<T>(
                        std::nearbyint(static_cast<double>(channel[i]) * s.flacMaximum) *
                        s.flacScale);
            std::fill(channel.begin() + static_cast<std::ptrdiff_t>(n), channel.end(), T{});
        }
        count = n;
        return n < out.frames() ? Status::end_of_stream : Status::ok;
    } catch (const std::bad_alloc&) {
        return Status::allocation_failed;
    } catch (...) {
        return Status::io_error;
    }
}
Status AudioFileReader::read(std::uint64_t first, AudioBlock<float> out, std::size_t& n) noexcept {
    return readImpl(first, out, n);
}
Status AudioFileReader::read(std::uint64_t first, AudioBlock<double> out, std::size_t& n) noexcept {
    return readImpl(first, out, n);
}
struct AudioFileWriter::State {
    std::unique_ptr<choc::audio::AudioFileWriter> writer;
    // Retained to observe write failures: a failed stream write only raises badbit,
    // which the codecs' own return values do not always report.
    std::shared_ptr<std::ostream> stream;
    // Path overload: the file is written here and replaces destination when finished.
    std::filesystem::path temporary, destination;
    ProcessSpec spec;
    std::uint64_t frames = 0;
    unsigned flacBits = 0;
    bool failed = false;
    AudioBuffer<double> scratch;
    std::vector<const float*> f;
    std::vector<const double*> d;
    ~State() {
        (void)finish(); // Best effort, as when the codec destructors finalised in place.
    }
    bool streamFailed() const {
        return stream && stream->fail();
    }
    static Status create(std::shared_ptr<std::ostream> stream, WritePlan plan,
                         const AudioFileInfo& info, std::size_t max,
                         std::unique_ptr<State>& out) {
        auto next = std::make_unique<State>();
        next->flacBits = plan.flacBits;
        if (next->flacBits) {
            const auto status = next->scratch.resize(info.channels, max);
            if (status != Status::ok)
                return status;
        }
        next->spec = {info.sampleRate, max, info.channels};
        next->f.resize(info.channels);
        next->d.resize(info.channels);
        next->stream = stream;
        next->writer = plan.codec->createWriter(std::move(stream), std::move(plan.properties));
        if (!next->writer)
            return Status::unsupported_format;
        if (next->streamFailed())
            return Status::io_error;
        out = std::move(next);
        return Status::ok;
    }
    // Finalises the codec, checks the stream and publishes a path-owned file. Idempotent.
    Status finish() noexcept {
        if (!writer)
            return failed ? Status::io_error : Status::ok;
        bool good = !failed;
        try {
            if (!writer->flush())
                good = false;
            if (auto* native = dynamic_cast<detail::PlatformAudioWriter*>(writer.get()))
                if (!native->finish())
                    good = false;
        } catch (...) {
            good = false;
        }
        writer.reset(); // Stream codecs write their last bytes here.
        if (stream) {
            try {
                // Only a stream this object opened is closed; a caller's stream is flushed.
                auto* file =
                    temporary.empty() ? nullptr : dynamic_cast<std::ofstream*>(stream.get());
                if (file)
                    file->close();
                else
                    stream->flush();
            } catch (...) {
                good = false;
            }
            if (stream->fail())
                good = false;
            stream.reset();
        }
        if (!temporary.empty()) {
            if (!good) {
                std::error_code ec;
                std::filesystem::remove(temporary, ec); // The destination stays untouched.
            } else if (!detail::replaceFile(temporary, destination))
                good = false; // Complete data is kept in the temporary rather than lost.
            temporary.clear();
        }
        failed = !good;
        return good ? Status::ok : Status::io_error;
    }
};
AudioFileWriter::AudioFileWriter() = default;
AudioFileWriter::~AudioFileWriter() = default;
AudioFileWriter::AudioFileWriter(AudioFileWriter&&) noexcept = default;
AudioFileWriter& AudioFileWriter::operator=(AudioFileWriter&&) noexcept = default;
Status AudioFileWriter::open(const std::filesystem::path& path, AudioFileFormat f,
                             const AudioFileInfo& info, std::string quality,
                             std::size_t max) noexcept {
    if (!valid(info, max))
        return Status::invalid_argument;
    try {
        Discard discard; // Destroyed last: after the stream/encoder below closed the file.
        std::unique_ptr<State> next;
        if (f == AudioFileFormat::mp3 || f == AudioFileFormat::m4a || f == AudioFileFormat::aac ||
            f == AudioFileFormat::wma) {
            if (info.channels > 2 || info.encoding != AudioEncoding::pcm16 ||
                info.sampleRate != std::floor(info.sampleRate) || info.sampleRate > 384000 ||
                !emptyMetadata(info.metadataJson) ||
                (!quality.empty() && quality != "128" && quality != "192" && quality != "256" &&
                 quality != "320"))
                return Status::unsupported_format;
            if (!detail::platformEncodingAvailable(f))
                return Status::unsupported_backend;
            choc::audio::AudioFileProperties props;
            props.sampleRate = info.sampleRate;
            props.numChannels = static_cast<std::uint32_t>(info.channels);
            props.bitDepth = Depth::int16;
            props.quality = std::move(quality);
            next = std::make_unique<State>();
            next->spec = {info.sampleRate, max, info.channels};
            next->f.resize(info.channels);
            next->d.resize(info.channels);
            discard.path = detail::temporarySibling(path);
            next->writer = detail::platformWriter(discard.path, f, std::move(props));
            if (!next->writer)
                return Status::unsupported_format;
        } else {
            // Format, encoding, quality and metadata are validated before any file exists.
            WritePlan prepared;
            auto status = plan(f, info, std::move(quality), prepared);
            if (status != Status::ok)
                return status;
            discard.path = detail::temporarySibling(path);
            auto file = std::make_shared<std::ofstream>(discard.path,
                                                        std::ios::binary | std::ios::trunc);
            if (!*file)
                return Status::io_error;
            status = State::create(std::move(file), std::move(prepared), info, max, next);
            if (status != Status::ok)
                return status;
        }
        next->temporary = std::move(discard.path);
        discard.path.clear();
        next->destination = path;
        state_.swap(next);
        return Status::ok;
    } catch (const std::bad_alloc&) {
        return Status::allocation_failed;
    } catch (...) {
        return Status::io_error;
    }
}
Status AudioFileWriter::open(std::shared_ptr<std::ostream> stream, AudioFileFormat f,
                             const AudioFileInfo& info, std::string quality,
                             std::size_t max) noexcept {
    if (!valid(info, max) || !stream || !*stream)
        return Status::invalid_argument;
    try {
        WritePlan prepared;
        auto status = plan(f, info, std::move(quality), prepared);
        if (status != Status::ok)
            return status;
        std::unique_ptr<State> next;
        status = State::create(std::move(stream), std::move(prepared), info, max, next);
        if (status != Status::ok)
            return status;
        state_.swap(next);
        return Status::ok;
    } catch (const std::bad_alloc&) {
        return Status::allocation_failed;
    } catch (...) {
        return Status::io_error;
    }
}
template <Sample T> Status AudioFileWriter::writeImpl(AudioBlock<const T> in) noexcept {
    if (!state_)
        return Status::not_prepared;
    auto& s = *state_;
    if (!in.isValid())
        return Status::invalid_argument;
    if (in.channels() != s.spec.channels)
        return Status::channel_mismatch;
    if (in.frames() > s.spec.maxBlockFrames ||
        in.frames() > std::numeric_limits<std::uint64_t>::max() - s.frames)
        return Status::block_too_large;
    if (s.failed || !s.writer)
        return Status::io_error; // The stream is unusable after a failed write.
    if (!in.frames())
        return Status::ok;
    // One branch-free pass per channel: x * 0 is zero for finite samples, NaN otherwise.
    for (std::size_t ch = 0; ch < in.channels(); ++ch) {
        T poison{};
        for (auto sample : in.channel(ch))
            poison += sample * T{};
        if (!(poison == T{}))
            return Status::invalid_argument;
    }
    try {
        bool written = false;
        if (s.flacBits) {
            // Round to nearest here, then aim at the middle of the interval CHOC
            // truncates to that code. The most negative code is outside its range.
            const auto scale = std::ldexp(1.0, static_cast<int>(s.flacBits) - 1);
            const auto maximum = scale - 1;
            auto scratch = s.scratch.block(in.frames());
            for (std::size_t ch = 0; ch < in.channels(); ++ch) {
                const auto source = in.channel(ch);
                const auto target = scratch.channel(ch);
                for (std::size_t i = 0; i < in.frames(); ++i) {
                    const auto code = static_cast<double>(
                        detail::aiff::quantise(static_cast<double>(source[i]), scale));
                    target[i] = code == 0 ? 0.0 : (code + (code > 0 ? 0.5 : -0.5)) / maximum;
                }
                s.d[ch] = target.data();
            }
            written = s.writer->appendFrames(choc::buffer::createChannelArrayView(
                s.d.data(), static_cast<std::uint32_t>(in.channels()),
                static_cast<std::uint32_t>(in.frames())));
        } else {
            auto& pointers = [&]() -> auto& {
                if constexpr (std::same_as<T, float>)
                    return s.f;
                else
                    return s.d;
            }();
            for (std::size_t ch = 0; ch < in.channels(); ++ch)
                pointers[ch] = in.channel(ch).data();
            written = s.writer->appendFrames(choc::buffer::createChannelArrayView(
                pointers.data(), static_cast<std::uint32_t>(in.channels()),
                static_cast<std::uint32_t>(in.frames())));
        }
        if (!written || s.streamFailed()) {
            s.failed = true;
            return Status::io_error;
        }
        s.frames += in.frames();
        return Status::ok;
    } catch (const std::bad_alloc&) {
        return Status::allocation_failed;
    } catch (...) {
        s.failed = true;
        return Status::io_error;
    }
}
Status AudioFileWriter::write(AudioBlock<const float> in) noexcept {
    return writeImpl(in);
}
Status AudioFileWriter::write(AudioBlock<const double> in) noexcept {
    return writeImpl(in);
}
Status AudioFileWriter::flush() noexcept {
    if (!state_ || !state_->writer)
        return Status::not_prepared;
    auto& s = *state_;
    try {
        if (!s.failed && s.writer->flush() && !s.streamFailed())
            return Status::ok;
    } catch (...) {
    }
    s.failed = true;
    return Status::io_error;
}
Status AudioFileWriter::close() noexcept {
    if (!state_)
        return Status::ok;
    const auto status = state_->finish();
    state_.reset();
    return status;
}
std::uint64_t AudioFileWriter::framesWritten() const noexcept {
    return state_ ? state_->frames : 0;
}
} // namespace cyaudio
