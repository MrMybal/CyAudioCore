#include "detail/AudioCallbackAdapter.h"
#include <algorithm>
#include <cmath>
namespace cyaudio::detail {
Status AudioCallbackAdapter::prepare(const AudioDeviceConfig& c, AudioDeviceCallback cb,
    AudioDeviceContextCallback extended, void* user, std::size_t inputs, std::size_t outputs) noexcept {
    try {
        config_ = c; callback_ = cb; extended_ = extended; user_ = user;
        inputs_ = inputs; outputs_ = outputs;
        inputViews_.resize(c.inputChannels); outputViews_.resize(c.outputChannels);
        auto r = VectorOps::create(CpuBackend::automatic, ops_);
        if (r != Status::ok) return r;
        if (inputs && (r = input_.resize(inputs, c.maxCallbackFrames)) != Status::ok) return r;
        if (outputs && (r = output_.resize(outputs, c.maxCallbackFrames)) != Status::ok) return r;
        return Status::ok;
    } catch (...) { return Status::allocation_failed; }
}
namespace {
AudioTimingValue foreignTime(std::uint64_t entry, double current, double time, bool known,
                             std::uint64_t offset) noexcept {
    if (!known || !isFinite(current) || !isFinite(time)) return {};
    const auto translated = static_cast<long double>(entry) +
        (static_cast<long double>(time) - current) * 1e9L + offset;
    if (!std::isfinite(translated) || translated < 0 || translated >= static_cast<long double>(UINT64_MAX)) return {};
    return {static_cast<std::uint64_t>(translated), AudioMeasurementSource::backendClockEstimate, true};
}
}
void AudioCallbackAdapter::render(const void* input, void* output, std::size_t frames, bool planar,
    std::uint64_t entry, const AudioNativeTimes& times, bool xrun) noexcept {
    if (xrun) xruns_.fetch_add(1, std::memory_order_relaxed);
    const auto* const* inPlanes = planar ? static_cast<const float* const*>(input) : nullptr;
    auto* const* outPlanes = planar ? static_cast<float* const*>(output) : nullptr;
    for (std::size_t first = 0; first < frames;) {
        const auto n = std::min(config_.maxCallbackFrames, frames - first);
        auto ib = inputs_ ? input_.block(n) : AudioBlock<float>(nullptr, 0, n);
        auto ob = outputs_ ? output_.block(n) : AudioBlock<float>(nullptr, 0, n);
        bool valid = true;
        if (!planar && input && inputs_) {
            valid = deinterleave(std::span<const float>(static_cast<const float*>(input) + first * inputs_, n * inputs_), ib) == Status::ok;
        }
        for (std::size_t ch = 0; ch < config_.inputChannels; ++ch) {
            const auto native = config_.activeInputChannels.empty() ? ch : config_.activeInputChannels[ch];
            if (planar && inPlanes && inPlanes[native]) inputViews_[ch] = inPlanes[native] + first;
            else {
                if (!input || planar || !valid) (void)ops_.fill(ib.channel(native), 0.0f);
                inputViews_[ch] = ib.channel(native).data();
            }
        }
        // Clear every native output, including channels not selected by the application.
        for (std::size_t ch = 0; ch < outputs_; ++ch) {
            auto span = planar && outPlanes && outPlanes[ch] ?
                std::span<float>(outPlanes[ch] + first, n) : ob.channel(ch);
            (void)ops_.fill(span, 0.0f);
        }
        for (std::size_t ch = 0; ch < config_.outputChannels; ++ch) {
            const auto native = config_.activeOutputChannels.empty() ? ch : config_.activeOutputChannels[ch];
            outputViews_[ch] = planar && outPlanes && outPlanes[native] ? outPlanes[native] + first : ob.channel(native).data();
        }
        AudioBlock<const float> in(inputViews_.data(), inputViews_.size(), n);
        AudioBlock<float> out(outputViews_.data(), outputViews_.size(), n);
        AudioCallbackContext timing;
        const auto offset = static_cast<std::uint64_t>(static_cast<double>(first) * 1e9 / config_.sampleRate);
        if (extended_) {
            timing.clock = {entry + offset, position_, double(config_.sampleRate)};
            timing.generation = config_.generation; timing.clockValid = true;
            timing.nativeCallbackSizeKnown = frames <= UINT32_MAX;
            timing.nativeCallbackFrames = timing.nativeCallbackSizeKnown ? static_cast<std::uint32_t>(frames) : 0;
            timing.callbackTime = {entry + offset, AudioMeasurementSource::callbackSteadyClock, true};
            timing.adcTime = foreignTime(entry, times.current, times.adc, times.adcKnown && input, offset);
            timing.dacTime = foreignTime(entry, times.current, times.dac, times.dacKnown && output, offset);
        }
        const auto r = !valid ? Status::invalid_argument : extended_ ? extended_(user_, in, out, timing) : callback_(user_, in, out, position_);
        if (r != Status::ok) {
            errors_.fetch_add(1, std::memory_order_relaxed);
            for (std::size_t ch = 0; ch < out.channels(); ++ch) (void)ops_.fill(out.channel(ch), 0.0f);
        }
        if (!planar && output && outputs_) {
            const auto destination = std::span<float>(static_cast<float*>(output) + first * outputs_, n * outputs_);
            if (interleave(ob, destination) != Status::ok) {
                std::fill(destination.begin(), destination.end(), 0.0f);
                errors_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        position_ += n; first += n;
    }
    calls_.fetch_add(1, std::memory_order_relaxed);
    if (config_.diagnostics && entry) {
        const auto now = audioSteadyNow();
        const auto elapsed = now >= entry ? now - entry : 0;
        const auto bounded = static_cast<std::uint32_t>(std::min<std::uint64_t>(elapsed, UINT32_MAX));
        last_.store(bounded, std::memory_order_relaxed);
        if (bounded > maximum_.load(std::memory_order_relaxed)) maximum_.store(bounded, std::memory_order_relaxed);
        if (elapsed > UINT32_MAX) clamped_.store(1, std::memory_order_relaxed);
        if (double(elapsed) > double(frames) * 1e9 / config_.sampleRate) deadlines_.fetch_add(1, std::memory_order_relaxed);
    }
}
AudioCallbackDiagnostics AudioCallbackAdapter::diagnostics(bool known) const noexcept {
    return {calls_.load(), errors_.load(), deadlines_.load(), xruns_.load(), last_.load(), maximum_.load(), known, clamped_.load() != 0};
}
} // namespace cyaudio::detail
