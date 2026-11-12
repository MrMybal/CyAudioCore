#pragma once
#include "cyaudio/io/AudioDevice.h"
namespace cyaudio::detail {
// Only control calls cross this interface. Providers own their native stream lifetime;
// the public wrapper retains the context until after stream destruction.
class AudioProviderStream {
  public:
    virtual ~AudioProviderStream() = default;
    virtual Status description(AudioStreamDescription&) const noexcept = 0;
    virtual AudioCallbackDiagnostics diagnostics() const noexcept = 0;
    virtual Status start() noexcept = 0;
    virtual Status stop() noexcept = 0;
    virtual bool running() const noexcept = 0;
    virtual std::uint32_t sampleRate() const noexcept = 0;
    virtual std::uint32_t takeNotifications() noexcept = 0;
};
class AudioProviderContext {
  public:
    virtual ~AudioProviderContext() = default;
    virtual AudioBackend backend() const noexcept = 0;
    virtual std::string backendName() const = 0;
    virtual Status enumerate(std::vector<AudioDeviceInfo>&) noexcept = 0;
    virtual Status capabilities(std::string_view, bool, AudioDeviceCapabilities&) noexcept = 0;
    virtual Status open(const AudioDeviceConfig&, AudioDeviceCallback, AudioDeviceContextCallback,
                        void*, std::unique_ptr<AudioProviderStream>&) noexcept = 0;
};
Status validateAudioConfig(const AudioDeviceConfig&, AudioDeviceCallback, AudioDeviceContextCallback) noexcept;
Status createMiniaudioContext(const AudioContextOptions&, std::shared_ptr<AudioProviderContext>&) noexcept;
Status createPortAudioContext(const AudioContextOptions&, std::shared_ptr<AudioProviderContext>&) noexcept;
} // namespace cyaudio::detail
