#pragma once
// Private ABI and deterministic test seam. third_party/licenses/portaudio.txt
#include "detail/AudioProvider.h"
#include <portaudio.h>
#include <mutex>
namespace cyaudio::detail {
struct PortAudioApi {
    decltype(&Pa_Initialize) initialize = &Pa_Initialize;
    decltype(&Pa_Terminate) terminate = &Pa_Terminate;
    decltype(&Pa_GetHostApiCount) hostCount = &Pa_GetHostApiCount;
    decltype(&Pa_GetDefaultHostApi) defaultHost = &Pa_GetDefaultHostApi;
    decltype(&Pa_GetHostApiInfo) hostInfo = &Pa_GetHostApiInfo;
    decltype(&Pa_GetDeviceCount) deviceCount = &Pa_GetDeviceCount;
    decltype(&Pa_GetDeviceInfo) deviceInfo = &Pa_GetDeviceInfo;
    decltype(&Pa_IsFormatSupported) formatSupported = &Pa_IsFormatSupported;
    decltype(&Pa_OpenStream) open = &Pa_OpenStream;
    decltype(&Pa_CloseStream) close = &Pa_CloseStream;
    decltype(&Pa_StartStream) start = &Pa_StartStream;
    decltype(&Pa_StopStream) stop = &Pa_StopStream;
    decltype(&Pa_AbortStream) abort = &Pa_AbortStream;
    decltype(&Pa_IsStreamActive) active = &Pa_IsStreamActive;
    decltype(&Pa_SetStreamFinishedCallback) finished = &Pa_SetStreamFinishedCallback;
    decltype(&Pa_GetStreamInfo) streamInfo = &Pa_GetStreamInfo;
};
struct PortAudioRuntime {
    explicit PortAudioRuntime(PortAudioApi functions = {}) : api(functions) {}
    PortAudioApi api;
    // Control only; recursive for RAII cleanup on failed operations. Never taken by callbacks.
    std::recursive_mutex gate;
    std::size_t leases = 0, streams = 0;
    bool initialized = false;
    // A catalogue refresh is a full terminate + initialize of every host API, run on the
    // control owner. It happens at most once per interval, whatever the caller's scan
    // period; in between, enumeration answers from the current snapshot. Zero disables
    // the limit. clock is a test seam (null: steady clock, nanoseconds).
    std::uint64_t (*clock)() noexcept = nullptr;
    std::uint64_t refreshIntervalNs = 2000000000ull, refreshedAt = 0;
    std::uint64_t now() const noexcept;
    Status acquire() noexcept;
    void release() noexcept;
    Status refresh() noexcept;
};
Status portAudioStatus(PaError) noexcept;
Status createPortAudioContext(const AudioContextOptions&, const std::shared_ptr<PortAudioRuntime>&,
                              std::shared_ptr<AudioProviderContext>&) noexcept;
} // namespace cyaudio::detail
