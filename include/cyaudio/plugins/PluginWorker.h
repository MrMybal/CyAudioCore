#pragma once
#include "cyaudio/core/DspTypes.h"
#include "cyaudio/plugins/Plugin.h"
namespace cyaudio {
struct PluginWorkerOptions {
    PluginPrepareOptions processing;
    std::uint32_t maximumChannelsPerDirection = 64, maximumPayloadBytes = 65536,
                  maximumStateBytes = 16 * 1024 * 1024;
    unsigned startupTimeoutMs = 5000, controlTimeoutMs = 2000, heartbeatTimeoutMs = 3000;
    PluginScheduling scheduling = PluginScheduling::pipelined;
};
struct PluginWorkerInfo {
    std::vector<std::uint32_t> inputChannels, outputChannels;
    PluginTiming pluginTiming;
    std::uint32_t pipelineLatencyFrames = 0, protocolVersion = 3;
};
struct PluginWorkerBlockResult {
    std::uint64_t submittedSequence = 0, receivedSequence = 0;
    bool submitted = false, received = false, missedDeadline = false;
};
// Separate OS process with bounded shared-memory mailbox. Pipelined process()
// adds one fixed-size block; sameCycle uses submit/collect/discard with no fixed
// pipeline delay. Audio methods never wait/query OS state/allocate; a submitted
// block may issue one non-blocking wake-up signal to a sleeping worker. Late blocks
// are discarded; the output is silenced. Call poll() on control to detect crashes
// and to refresh info() latency/tail, which the worker republishes when they change.
// The worker runs the plugin's process on a dedicated audio thread, so a modal loop
// on the plugin's UI thread stops neither audio nor the heartbeat.
// launch/control/close require audio idle. The supplied worker executable calls
// runPluginWorker() below; production presets never create/run an executable.
class PluginWorker {
  public:
    PluginWorker();
    ~PluginWorker();
    PluginWorker(PluginWorker&&) noexcept;
    PluginWorker& operator=(PluginWorker&&) noexcept;
    Status launch(const std::filesystem::path& workerExecutable, PluginFormat,
                  const std::filesystem::path& plugin, std::string_view id,
                  const PluginWorkerOptions& = {}) noexcept;
    const PluginWorkerInfo* info() const noexcept;
    Status process(const PluginProcess<float>&, PluginWorkerBlockResult&) noexcept;
    Status process(const PluginProcess<double>&, PluginWorkerBlockResult&) noexcept;
    Status submit(const PluginProcess<float>&, std::uint64_t& ticket) noexcept;
    Status submit(const PluginProcess<double>&, std::uint64_t& ticket) noexcept;
    Status collect(const PluginProcess<float>&, std::uint64_t ticket) noexcept;
    Status collect(const PluginProcess<double>&, std::uint64_t ticket) noexcept;
    Status discard(std::uint64_t ticket) noexcept;
    Status poll() noexcept;
    Status poll(std::uint32_t& requests) noexcept;
    Status saveState(std::vector<std::uint8_t>&) noexcept;
    Status loadState(std::span<const std::uint8_t>) noexcept;
    // Stops, deactivates, refreshes, reactivates and restarts the plugin in the worker.
    // Serves pluginRestart/pluginLatencyChanged requests; channel_mismatch means the
    // ports changed and the worker must be relaunched. info() timing is refreshed.
    Status restart() noexcept;
    Status openEditor(const PluginWindow&, PluginEditorSize&) noexcept;
    Status resizeEditor(PluginEditorSize&) noexcept;
    Status scaleEditor(double) noexcept;
    Status showEditor(bool) noexcept;
    Status focusEditor(bool) noexcept;
    Status keyEditor(const PluginEditorKey&) noexcept;
    Status wheelEditor(float) noexcept;
    PluginEditorSize requestedEditorSize() const noexcept;
    Status closeEditor() noexcept;
    void close() noexcept;

  private:
    friend struct detail::PluginRemoteAccess;
    struct State;
    std::unique_ptr<State> state_;
    template <Sample T>
    Status processImpl(const PluginProcess<T>&, PluginWorkerBlockResult&) noexcept;
    template <Sample T> Status exchange(const PluginProcess<T>&, std::uint64_t&, bool) noexcept;
    Status launchMode(const std::filesystem::path&, PluginFormat, const std::filesystem::path&,
                      std::string_view, const PluginWorkerOptions&, std::uint32_t,
                      std::span<const std::uint8_t>, const PluginHostCallbacks* = nullptr,
                      const ClapHostServices* = nullptr) noexcept;
};
// Call only for explicitly supplied --cydsp-worker <mapping> command-line mode.
// Returns an exit code; serialized plugin main/audio operations and native Windows
// message pumping live in this process. No platform license/SDK is accepted here.
int runPluginWorker(std::string_view mappingName) noexcept;
} // namespace cyaudio
