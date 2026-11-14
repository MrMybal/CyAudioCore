#pragma once
#include "cyaudio/core/DspTypes.h"
#include "cyaudio/midi/Ump.h"
#include <memory>
namespace cyaudio {
struct AudioClockAnchor {
    std::uint64_t monotonicNanoseconds = 0, firstFrame = 0;
    double sampleRate = 48000;
};
enum class LateMidiPolicy { clampToFirstFrame, drop };
struct MidiCollectorOptions {
    std::size_t messages = 1024, maximumMessageBytes = 4096;
    LateMidiPolicy late = LateMidiPolicy::clampToFirstFrame;
};
// One device/control producer, one audio consumer. Arrival timestamps and anchors
// share steady_clock nanoseconds. No OS clock is queried by collection itself.
class MidiEventCollector {
  public:
    MidiEventCollector();
    ~MidiEventCollector();
    MidiEventCollector(MidiEventCollector&&) noexcept;
    MidiEventCollector& operator=(MidiEventCollector&&) noexcept;
    Status prepare(const MidiCollectorOptions& = {}) noexcept;
    Status pushMidi(std::uint64_t nanoseconds, std::span<const std::uint8_t>) noexcept;
    Status pushUmp(std::uint64_t nanoseconds, const UmpPacket&) noexcept;
    // Appends to prepared output buffers. Future events remain queued; stable
    // arrival ordering resolves equal timestamps. Output saturation retains the event
    // (update_queue_full). An event larger than the whole MidiBuffer can never be delivered:
    // it is dropped and counted, and block_too_large is returned after the rest was collected.
    Status collect(const AudioClockAnchor&, std::uint32_t frames, MidiBuffer&, UmpBuffer&) noexcept;
    // Consumer. Takes the arrival/invalidation snapshot the next collect will use and returns
    // its epoch, so an epoch change can be handled before the first event of the new epoch.
    std::uint32_t syncEpoch() noexcept;
    void invalidate() noexcept; // Producer invalidates old connection events.
    void reset() noexcept;      // Both owners idle; retains storage.
    [[nodiscard]] std::uint32_t overflows() const noexcept;
    [[nodiscard]] std::uint32_t lateMessages() const noexcept;
    [[nodiscard]] std::uint32_t oversizedMessages() const noexcept;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace cyaudio
