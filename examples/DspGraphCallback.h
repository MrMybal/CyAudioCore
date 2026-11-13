#pragma once
#include <cyaudio/io/AudioDevice.h>
#include <cydsp/processors/MultiBusGraph.h>
#include <memory>
#include <stdexcept>
#include <vector>

namespace cyaudio::examples {
// Application example, not an installed service API. Graph and node contexts are
// borrowed at stable addresses. Serialize prepare/process/reset/destruction, and
// stop device callbacks before changing the graph or destroying these objects.
template <Sample T> class DspGraphCallback {
  public:
    Status prepare(cydsp::MultiBusGraph<T>& graph) noexcept {
        if (!graph.prepared()) return Status::not_prepared;
        try {
            auto next = std::make_unique<State>();
            next->graph = &graph;
            next->spec = graph.specification();
            for (const auto& bus : graph.inputBuses()) next->inputChannels += bus.channels;
            for (const auto& bus : graph.outputBuses()) next->outputChannels += bus.channels;
            if (next->inputChannels > 256 || next->outputChannels > 256)
                return Status::invalid_spec;
            next->inputPointers.resize(next->inputChannels);
            next->outputPointers.resize(next->outputChannels);
            next->inputs.resize(graph.inputBuses().size());
            next->outputs.resize(graph.outputBuses().size());
            if (next->inputChannels) {
                const auto status = next->silence.resize(next->inputChannels, next->spec.maxBlockFrames);
                if (status != Status::ok) return status;
            }
            state_.swap(next);
            return Status::ok;
        } catch (const std::bad_alloc&) { return Status::allocation_failed; }
          catch (const std::length_error&) { return Status::invalid_spec; }
    }

    // Buses occupy consecutive application channels, in graph declaration order.
    // Device active-channel selection/mapping happens before this callback. A
    // zero-channel input denotes missing capture; the graph receives prepared zeroes.
    // The device adapter splits native callbacks at the configured maximum size.
    Status process(AudioBlock<const T> input, AudioBlock<T> output) noexcept {
        if (!state_) return Status::not_prepared;
        auto& s = *state_;
        if (!input.isValid() || !output.isValid()) return Status::invalid_argument;
        if (output.channels() != s.outputChannels ||
            (input.channels() && input.channels() != s.inputChannels)) return Status::channel_mismatch;
        if (input.frames() != output.frames()) return Status::invalid_argument;
        if (output.frames() > s.spec.maxBlockFrames) return Status::block_too_large;
        if (!input.channels() && s.inputChannels)
            input = s.silence.block().subBlock(0, output.frames());
        for (std::size_t ch = 0; ch < s.inputChannels; ++ch)
            s.inputPointers[ch] = input.channel(ch).data();
        for (std::size_t ch = 0; ch < s.outputChannels; ++ch)
            s.outputPointers[ch] = output.channel(ch).data();
        std::size_t channel = 0, index = 0;
        for (const auto& bus : s.graph->inputBuses()) {
            s.inputs[index++] = {s.inputPointers.data() + channel, bus.channels, output.frames()};
            channel += bus.channels;
        }
        channel = index = 0;
        for (const auto& bus : s.graph->outputBuses()) {
            s.outputs[index++] = {s.outputPointers.data() + channel, bus.channels, output.frames()};
            channel += bus.channels;
        }
        return s.graph->process(s.inputs, s.outputs);
    }
    void reset() noexcept { if (state_) state_->graph->reset(); }
    [[nodiscard]] cydsp::DspTiming timing() const noexcept {
        return state_ ? state_->graph->timing() : cydsp::DspTiming{};
    }
    static Status callback(void* context, AudioBlock<const float> input,
                           AudioBlock<float> output, const AudioCallbackContext&) noexcept
        requires std::same_as<T, float> {
        return context ? static_cast<DspGraphCallback*>(context)->process(input, output)
                       : Status::invalid_argument;
    }
  private:
    struct State {
        cydsp::MultiBusGraph<T>* graph = nullptr;
        ProcessSpec spec;
        std::size_t inputChannels = 0, outputChannels = 0;
        AudioBuffer<T> silence;
        std::vector<const T*> inputPointers;
        std::vector<T*> outputPointers;
        std::vector<AudioBlock<const T>> inputs;
        std::vector<AudioBlock<T>> outputs;
    };
    std::unique_ptr<State> state_;
};
} // namespace cyaudio::examples
