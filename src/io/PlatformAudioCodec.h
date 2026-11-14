#pragma once
#include "cyaudio/core/DspTypes.h"
#include "cyaudio/io/AudioFile.h"
#include <choc/audio/choc_AudioFileFormat.h>
namespace cyaudio::detail {
class PlatformAudioWriter : public choc::audio::AudioFileWriter {
  public:
    virtual bool finish() = 0;
};
bool platformEncodingAvailable(AudioFileFormat) noexcept;
bool platformDecodingAvailable(AudioFileFormat) noexcept;
std::unique_ptr<choc::audio::AudioFileWriter>
platformWriter(const std::filesystem::path&, AudioFileFormat, choc::audio::AudioFileProperties);
std::unique_ptr<choc::audio::AudioFileReader> platformReader(const std::filesystem::path&);
// formatName reported by platformReader on this system; empty when there is none.
const char* platformFormatName() noexcept;
// Whole-file publication used by every path-owning writer: the content is written to a
// sibling temporary (same directory and extension) and then atomically replaces the
// destination, which therefore never holds a partial file.
std::filesystem::path temporarySibling(const std::filesystem::path& destination);
bool replaceFile(const std::filesystem::path& temporary,
                 const std::filesystem::path& destination) noexcept;
} // namespace cyaudio::detail
