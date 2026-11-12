#include <cyaudio/io/AudioDeviceManager.h>

// Compile-only example: the caller explicitly chooses whether to open a device.
// All calls here are on the control owner. The callback renders a prepared engine.
cyaudio::Status openAudioOutputExample(cyaudio::AudioDeviceManager& audio,
    cyaudio::AudioProvider provider, cyaudio::AudioBackend backend,
    cyaudio::AudioDeviceContextCallback render, void* user) noexcept {
    cyaudio::AudioContextOptions options;
    options.provider = provider;
    options.backend = backend;
    const auto prepared = audio.prepare(options);
    if (prepared != cyaudio::Status::ok) return prepared;
    cyaudio::AudioDeviceConfig config;
    config.sampleRate = 48000;
    config.outputChannels = 2;
    config.maxCallbackFrames = 512;
    return audio.openWithContext(config,render,user);
    // Caller services audio.poll(nowMs) on control, and closes before destroying user.
}
