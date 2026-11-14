#pragma once
#include "cyaudio/core/DspTypes.h"
#include "PluginInternal.h"
#include <algorithm>
#include <cstring>
#include <memory>
namespace cyaudio::detail {
// Prepared scratch for one block of plugin output events. The payload storage is
// reserved without being zero-filled, so only bytes actually used are ever touched.
class PluginOutput {
    struct Record {
        PluginEvent event;
        std::size_t order;
    };
    std::vector<Record> events_;
    std::unique_ptr<std::uint8_t[]> bytes_;
    std::size_t capacity_ = 0, count_ = 0, used_ = 0;

  public:
    void prepare(std::size_t events, std::size_t bytes) {
        std::vector<Record> nextEvents(events);
        auto nextBytes = bytes ? std::make_unique_for_overwrite<std::uint8_t[]>(bytes) : nullptr;
        events_.swap(nextEvents);
        bytes_ = std::move(nextBytes);
        capacity_ = bytes;
        reset();
    }
    void reset() noexcept {
        count_ = used_ = 0;
    }
    Status add(PluginEvent e) noexcept {
        if (count_ == events_.size() || e.bytes.size() > capacity_ - used_)
            return Status::update_queue_full;
        if (!e.bytes.empty()) {
            std::memcpy(bytes_.get() + used_, e.bytes.data(), e.bytes.size());
            e.bytes = {bytes_.get() + used_, e.bytes.size()};
            used_ += e.bytes.size();
        }
        events_[count_] = {e, count_};
        ++count_;
        return Status::ok;
    }
    // Copies the events in stable frame order. Returns how many the destination
    // rejected (capacity or content); a rejected event never fails the audio block.
    std::uint32_t finish(PluginEventBuffer* result) noexcept {
        if (!result)
            return 0;
        std::sort(events_.begin(), events_.begin() + std::ptrdiff_t(count_),
                  [](const auto& a, const auto& b) {
                      return a.event.frame < b.event.frame ||
                             (a.event.frame == b.event.frame && a.order < b.order);
                  });
        std::uint32_t rejected = 0;
        for (std::size_t i = 0; i < count_; ++i)
            if (result->add(events_[i].event) != Status::ok)
                ++rejected;
        return rejected;
    }
};
} // namespace cyaudio::detail
