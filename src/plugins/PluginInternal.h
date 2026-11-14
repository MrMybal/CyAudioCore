#pragma once
#include "cyaudio/core/DspTypes.h"
#include "cyaudio/plugins/Plugin.h"
#include <atomic>
#include <thread>
namespace cyaudio::detail {
extern thread_local const PluginBackend* currentAudioPlugin;
// An output event that cannot be translated or stored is dropped and counted on the
// audio owner; it never fails the block. The UI owner reports pluginEventsDropped
// from pumpMainThread whenever the count moved since its previous pump.
struct DroppedEvents {
    std::atomic<std::uint32_t> count{0};
    std::uint32_t reported = 0; // UI owner only.
    void add(std::uint32_t n = 1) noexcept {
        if (n)
            count.fetch_add(n, std::memory_order_relaxed);
    }
    std::uint32_t take() noexcept {
        const auto now = count.load(std::memory_order_relaxed);
        const auto moved = now != reported;
        reported = now;
        return moved ? std::uint32_t(pluginEventsDropped) : 0u;
    }
};
struct PluginAudioScope {
    const PluginBackend* previous;
    explicit PluginAudioScope(const PluginBackend* p) noexcept : previous(currentAudioPlugin) {
        currentAudioPlugin = p;
    }
    ~PluginAudioScope() {
        currentAudioPlugin = previous;
    }
};
// Worker service only. Saves on the UI owner while the instance stays in
// `processing`: the caller guarantees that its audio owner is parked outside
// process/start/stop for the whole call, so no stop()/start() cycle (which resets
// DSP state in some plugins) is needed around every save.
struct PluginInstanceAccess {
    static Status saveParked(PluginInstance&, std::vector<std::uint8_t>&,
                             std::size_t maximumBytes) noexcept;
};
std::unique_ptr<PluginProvider> makeClapProvider();
std::unique_ptr<PluginProvider> makeVst3Provider();
std::unique_ptr<PluginProvider> makeLv2Provider();
std::unique_ptr<PluginProvider> makeJsfxProvider();
class SharedModule {
  public:
    SharedModule() = default;
    ~SharedModule();
    SharedModule(const SharedModule&) = delete;
    SharedModule& operator=(const SharedModule&) = delete;
    Status open(const std::filesystem::path&) noexcept;
    void* symbol(const char*) const noexcept;
    void* nativeHandle() const noexcept {
        return handle_;
    }

  private:
    void* handle_ = nullptr;
#if defined(__APPLE__)
    bool bundle_ = false;
#endif
};
bool validPluginTransport(const PluginTransport&) noexcept;
} // namespace cyaudio::detail
