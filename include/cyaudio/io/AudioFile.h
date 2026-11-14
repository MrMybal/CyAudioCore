#pragma once
#include "cyaudio/core/DspTypes.h"
#include "cydsp/core/AudioBlock.h"
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>
namespace cyaudio {
enum class AudioFileFormat { wav, flac, oggVorbis, aiff, mp3, aifc, caf, m4a, aac, wma };
enum class AudioEncoding { pcm8, pcm16, pcm24, pcm32, float32, float64 };
struct AudioFileInfo {
    std::string formatName;
    double sampleRate = 0;
    std::uint64_t frames = 0;
    std::size_t channels = 0;
    AudioEncoding encoding = AudioEncoding::pcm16;
    std::string metadataJson;
};
struct AudioFormatCapabilities {
    AudioFileFormat format;
    bool read = false, write = false;
    std::size_t maximumChannels = 0;
    std::vector<AudioEncoding> writableEncodings;
    std::vector<std::string> qualityLevels;
};
// Integer PCM is normalized by 2^(bits-1); float data keeps its stored scale.
// Integer export rounds to nearest and clips to the integer range: +1.0 becomes the
// largest code (one LSB below full scale); FLAC additionally cannot store the most
// negative code. WAV/AIFF/AIFC/CAF integer data round-trips bit-exactly.
// metadataJson is empty when the source has none ("null" is accepted as empty).
// A failed write is reported by write/flush/close and is sticky until close.
// All file/stream methods are control/worker-thread operations (I/O and allocation).
[[nodiscard]] std::vector<AudioFormatCapabilities> audioFileFormats();
class AudioFileReader {
  public:
    AudioFileReader();
    ~AudioFileReader();
    AudioFileReader(AudioFileReader&&) noexcept;
    AudioFileReader& operator=(AudioFileReader&&) noexcept;
    // Failed open preserves an existing reader. Streams must be binary and seekable.
    // A missing or unreadable path is io_error. WAV/AIFF/CAF parsing stays bounded by the
    // stream but tolerates benign defects: sizes beyond the end are clamped, a partial
    // last frame is dropped, damaged or oversized optional chunks are skipped.
    Status open(const std::filesystem::path&, std::size_t maximumBlockFrames = 4096) noexcept;
    Status open(std::shared_ptr<std::istream>, std::size_t maximumBlockFrames = 4096) noexcept;
    void close() noexcept;
    [[nodiscard]] const AudioFileInfo* info() const noexcept;
    // Successful short reads zero-pad the tail and return end_of_stream. FramesRead is exact.
    Status read(std::uint64_t firstFrame, AudioBlock<float>, std::size_t& framesRead) noexcept;
    Status read(std::uint64_t firstFrame, AudioBlock<double>, std::size_t& framesRead) noexcept;

  private:
    struct State;
    std::unique_ptr<State> state_;
    template <Sample T> Status readImpl(std::uint64_t, AudioBlock<T>, std::size_t&) noexcept;
};
class AudioFileWriter {
  public:
    AudioFileWriter();
    ~AudioFileWriter();
    AudioFileWriter(AudioFileWriter&&) noexcept;
    AudioFileWriter& operator=(AudioFileWriter&&) noexcept;
    // Format, encoding, quality (empty or one of the format's qualityLevels) and metadata
    // are validated before anything is written. The path overload writes a sibling
    // temporary file and replaces the destination atomically on close (or destruction,
    // best effort): a failed open or write leaves an existing destination untouched, and
    // the destination only ever appears complete. If the final replacement itself fails,
    // close returns io_error and the complete temporary is left beside the destination.
    // flush finalises the headers of the file being written.
    Status open(const std::filesystem::path&, AudioFileFormat, const AudioFileInfo&,
                std::string quality = {}, std::size_t maximumBlockFrames = 4096) noexcept;
    Status open(std::shared_ptr<std::ostream>, AudioFileFormat, const AudioFileInfo&,
                std::string quality = {}, std::size_t maximumBlockFrames = 4096) noexcept;
    Status write(AudioBlock<const float>) noexcept;
    Status write(AudioBlock<const double>) noexcept;
    Status flush() noexcept;
    Status close() noexcept;
    [[nodiscard]] std::uint64_t framesWritten() const noexcept;

  private:
    struct State;
    std::unique_ptr<State> state_;
    template <Sample T> Status writeImpl(AudioBlock<const T>) noexcept;
};
} // namespace cyaudio
