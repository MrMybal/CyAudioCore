#pragma once
#include "cyaudio/core/DspTypes.h"
#include "cydsp/core/AudioBuffer.h"
#include "cydsp/core/Cpu.h"
#include "cydsp/synthesis/SampleData.h"
#include "cyaudio/midi/Mpe.h"
#include "cyaudio/midi/Ump.h"
#include "cydsp/processors/Generators.h"
#include "cydsp/synthesis/Wavetable.h"
#include <memory>
namespace cyaudio {
struct SynthVoiceInfo {
    std::size_t index = 0;
    std::uint64_t id = 0;
    unsigned group = 0, channel = 1, note = 60;
    double frequency = 440, velocity = 0, pressure = 0, timbre = .5;
    bool active = false, releasing = false;
};
struct SynthParameters {
    AdsrParameters envelope{};
    Waveform waveform = Waveform::sine;
    double gain = .2, pan = 0, pressureToGain = 0;
    std::size_t stealFadeFrames = 32;
    SampleInterpolation sampleInterpolation = SampleInterpolation::bandLimited;
};
Status validateSynthParameters(const SynthParameters&) noexcept;
template <Sample T> struct SynthVoiceRenderer {
    void* context = nullptr;
    void (*start)(void*, const SynthVoiceInfo&) noexcept = nullptr;
    void (*stop)(void*, const SynthVoiceInfo&) noexcept = nullptr;
    Status (*render)(void*, const SynthVoiceInfo&, AudioBlock<T> mono) noexcept = nullptr;
};
// Prepared polyphonic oscillator/sampler/custom voices, sample-accurate MIDI1/MPE/UMP events.
template <Sample T> class Synthesizer {
  public:
    Synthesizer();
    ~Synthesizer();
    Synthesizer(Synthesizer&&) noexcept;
    Synthesizer& operator=(Synthesizer&&) noexcept;
    Status prepare(const ProcessSpec&, std::size_t voices = 64,
                   CpuBackend = CpuBackend::automatic) noexcept;
    Status setParameters(const SynthParameters&) noexcept;
    Status setMpeZones(unsigned lowerMembers, unsigned upperMembers) noexcept;
    // Configuration and sample lifetime are control-thread responsibilities; voices must be idle.
    Status setSample(const SampleData<T>*) noexcept;
    Status setRenderer(SynthVoiceRenderer<T>) noexcept;
    Status setWavetable(const Wavetable<T>*) noexcept;
    Status process(AudioBlock<T>, const MidiBuffer* midi = nullptr, const UmpBuffer* ump = nullptr,
                   bool add = false) noexcept;
    void reset() noexcept;
    [[nodiscard]] std::size_t activeVoices() const noexcept;
    [[nodiscard]] std::uint64_t stolenVoices() const noexcept;
    [[nodiscard]] CpuBackend backend() const noexcept;
    [[nodiscard]] SynthVoiceInfo voice(std::size_t) const noexcept;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
extern template class Synthesizer<float>;
extern template class Synthesizer<double>;
} // namespace cyaudio
