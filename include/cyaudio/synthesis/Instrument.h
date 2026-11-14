#pragma once
#include "cyaudio/core/DspTypes.h"
#include "cyaudio/io/AudioFile.h"
#include "cyaudio/synthesis/Synthesizer.h"
namespace cyaudio {
enum class InstrumentSource { oscillator, sample, wavetable };
struct InstrumentPreset {
    std::string name;
    SynthParameters parameters{};
    InstrumentSource source = InstrumentSource::oscillator;
    std::size_t voices = 64;
    unsigned lowerMpeMembers = 0, upperMpeMembers = 0, rootNote = 60;
    std::filesystem::path samplePath;
    std::uint64_t loopStart = 0, loopEnd = 0, crossfadeFrames = 0;
    std::vector<double> cycle;
};
// Original versioned CyDSP instrument JSON format. Link CyDSP::Files.
// Parse/serialize/load/save are control-thread operations, transactional in memory.
// save replaces the destination only by a complete file (sibling temporary + rename).
Status parseInstrumentPreset(std::string_view, InstrumentPreset&) noexcept;
Status serializeInstrumentPreset(const InstrumentPreset&, std::string&) noexcept;
Status loadInstrumentPreset(const std::filesystem::path&, InstrumentPreset&) noexcept;
Status saveInstrumentPreset(const std::filesystem::path&, const InstrumentPreset&) noexcept;
// prepare loads a sample preset's audio file: non-finite samples are rejected, and the
// frame limit also bounds memory (frames x channels may not exceed twice the limit).
// A preset from an untrusted source can name any path, including a network share: with
// confineSamplePath the sample must be a relative path that stays lexically inside
// baseDirectory (no root, no drive, no leading ".."); symbolic links are not resolved.
template <Sample T> class Instrument {
  public:
    Instrument();
    ~Instrument();
    Instrument(Instrument&&) noexcept;
    Instrument& operator=(Instrument&&) noexcept;
    Status prepare(const ProcessSpec&, const InstrumentPreset&,
                   const std::filesystem::path& baseDirectory = {},
                   CpuBackend = CpuBackend::automatic,
                   std::size_t maximumSampleFrames = 67108864,
                   bool confineSamplePath = false) noexcept;
    Status process(AudioBlock<T>, const MidiBuffer* = nullptr, const UmpBuffer* = nullptr,
                   bool add = false) noexcept;
    void reset() noexcept;
    [[nodiscard]] Synthesizer<T>* synthesizer() noexcept;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
extern template class Instrument<float>;
extern template class Instrument<double>;
} // namespace cyaudio
