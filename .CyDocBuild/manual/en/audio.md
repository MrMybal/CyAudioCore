---
id: audio
title: Connect audio
description: Select a provider, backend and prepared callback.
section: Guides
order: 2
revision: 1
parent: overview
---

## Provider and system backend {#providers}

`AudioProvider` chooses miniaudio or PortAudio. `AudioBackend` chooses a system API such as WASAPI, CoreAudio, ALSA or JACK. They are separate settings. Existing default calls use miniaudio and automatic system selection.

PortAudio requires `CYAUDIOCORE_WITH_PORTAUDIO=ON`. Its static recipe enables WASAPI/WinMM on Windows, CoreAudio on macOS and ALSA on desktop Unix; JACK is an explicit Unix option with its development dependency. ASIO is not implemented by this recipe and its SDK is not bundled. Android and WebAudio are outside this PortAudio provider's scope. An explicit unavailable provider/backend fails; it does not silently select another one.

## Open and render {#callback}

Call this function from the serialized control owner with a provider/backend supported by your build. It opens and starts a silent output stream. The adapter prepares silence before invoking the callback; replace the callback body with your prepared engine. Keep the manager alive until you close it.

```cpp
#include <cyaudio/io/AudioDeviceManager.h>

cyaudio::Status openSilentAudio(cyaudio::AudioDeviceManager& audio,
                              cyaudio::AudioProvider provider,
                              cyaudio::AudioBackend backend) {
    cyaudio::AudioContextOptions options;
    options.provider = provider;
    options.backend = backend;
    auto status = audio.prepare(options);
    if (status != cyaudio::Status::ok) return status;
    cyaudio::AudioDeviceConfig config;
    config.sampleRate = 48000;
    config.outputChannels = 2;
    config.maxCallbackFrames = 512;
    return audio.openWithContext(config,
        [](void*, cyaudio::AudioBlock<const float>,
           cyaudio::AudioBlock<float>, const cyaudio::AudioCallbackContext&) noexcept {
            return cyaudio::Status::ok;
        }, nullptr);
}
```

Callbacks use float32 `AudioBlock` views. The private adapter handles native layouts and splits native blocks larger than `maxCallbackFrames` into prepared chunks. Missing input is represented by silence. `activeInputChannels` and `activeOutputChannels` select physical channels; their order determines application channel order. Unselected outputs are silent.

## Reconnect on the control owner {#reconnect}

Call `poll(nowMs)` regularly outside the audio thread. `AudioDeviceManager` has no hidden polling thread. React to its callbacks and `lastError()`. Default reconnection does not authorize a fallback to a different default device; that is an explicit `AudioReconnectOptions` choice.

Device identifiers are provider-specific. Preserve miniaudio IDs. A PortAudio enumeration index is not a persistent hardware identity; duplicated identities may be valid only for their catalogue snapshot, and ambiguous resolutions are refused. Re-enumerate rather than guessing a replacement.

## Clocks and capabilities {#timing}

Use `AudioCallbackContext` and the stream generation to relate frames and timestamps. A new stream generation restarts frame numbering. Timing values carry a `valid` flag and a measurement source; capability structures identify which fields are known. Preserve unknown values. Do not reinterpret a backend clock as a `steady_clock` date or infer native formats from a list of suggested settings.

Stop/close on the control owner before destroying borrowed callback state. See [lifetimes and failures](doc:lifecycle).
