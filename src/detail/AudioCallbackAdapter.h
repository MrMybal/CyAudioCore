#pragma once
#include "cyaudio/io/AudioDevice.h"
#include <atomic>
#include <chrono>
namespace cyaudio::detail {
inline std::uint64_t audioSteadyNow() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct AudioNativeTimes {
    // Relative seconds in a foreign clock, paired with the callback's steady-clock entry.
    double current = 0, adc = 0, dac = 0;
    bool adcKnown = false, dacKnown = false;
};
class AudioCallbackAdapter {
  public:
    Status prepare(const AudioDeviceConfig&, AudioDeviceCallback, AudioDeviceContextCallback,
                   void*, std::size_t nativeInputs, std::size_t nativeOutputs) noexcept;
    // Planar buffers are native arrays of channel pointers. No native API calls here.
    void render(const void* input, void* output, std::size_t frames, bool planar,
                std::uint64_t entry, const AudioNativeTimes& = {}, bool xrun = false) noexcept;
    AudioCallbackDiagnostics diagnostics(bool xrunsKnown = false) const noexcept;
    void resetPosition() noexcept { position_ = 0; } // Only while audio is idle.
  private:
    AudioDeviceConfig config_;
    AudioDeviceCallback callback_ = nullptr;
    AudioDeviceContextCallback extended_ = nullptr;
    void* user_ = nullptr;
    AudioBuffer<float> input_, output_;
    std::vector<const float*> inputViews_;
    std::vector<float*> outputViews_;
    VectorOps ops_;
    std::size_t inputs_ = 0, outputs_ = 0;
    std::uint64_t position_ = 0;
    std::atomic<std::uint32_t> calls_{0}, errors_{0}, deadlines_{0}, xruns_{0}, last_{0}, maximum_{0}, clamped_{0};
};
} // namespace cyaudio::detail
