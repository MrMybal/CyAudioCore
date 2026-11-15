---
id: plugins
title: Host plugins
description: Load CLAP and select the plugin lifecycle.
section: Plugins
order: 5
revision: 2
---

## Configure isolation first {#load}

Built-in CLAP, VST3, LV2 and JSFX providers always use separate processes. Scans are disposable workers and each live instance has its own worker, including its editor. CLAP preset discovery is isolated too. Supply an absolute executable path; a missing path returns `invalid_argument`. No failure falls back to loading a foreign module inside the engine.

Build `cyaudio_plugin_worker` with `CYAUDIOCORE_BUILD_PLUGIN_WORKER=ON`, or provide an application helper linked to `CyAudioCore::Plugins` that recognizes `--cydsp-worker <mapping>` and calls `runPluginWorker`. Match its ABI and protocol version 4 to the library.

This control-thread helper takes absolute paths and loads the first descriptor returned by the chosen module. A user-facing browser should let the user select a descriptor instead.

```cpp
#include <cyaudio/plugins/Plugin.h>

cyaudio::Status loadClap(const std::filesystem::path& worker,
                        const std::filesystem::path& module,
                        std::unique_ptr<cyaudio::PluginProvider>& provider,
                        cyaudio::PluginInstance& instance) {
    cyaudio::PluginIsolationOptions isolation;
    isolation.workerExecutable = worker;
    auto status = cyaudio::createPluginProvider(provider, isolation);
    if (status != cyaudio::Status::ok) return status;
    std::vector<cyaudio::PluginDescriptor> plugins;
    status = provider->scan(module, plugins);
    if (status != cyaudio::Status::ok) return status;
    if (plugins.empty()) return cyaudio::Status::unsupported_format;
    return provider->create(module, plugins.front().id, {}, instance);
}

cyaudio::Status activatePlugin(cyaudio::PluginInstance& instance) {
    cyaudio::PluginPrepareOptions options;
    options.sampleRate = 48000;
    options.maximumFrames = 512;
    options.sleep.mode = cyaudio::PluginSleepMode::automatic;
    options.sleep.quietFrames = 2048;
    options.sleep.silenceThreshold = 0;
    return instance.activate(options);
}
```

The overload without `PluginFormat` selects CLAP. Other formats require an explicit choice; there is no silent format fallback. The public interface exposes no upstream plugin SDK types. `PluginInstance::adopt` accepts trusted application backends and is not a foreign-module loader.

## Activate and process {#lifecycle}

Inspect `description()` to prepare the plugin's audio buses and event buffers. Configure inactive ports and parameters as needed. Call `activate` with sample rate and frame bounds while the audio owner is idle; keep capacities within the worker's prepared limits. Start/process/stop use one serialized audio owner. Float32/float64 support is negotiated with the plugin, not assumed.

The default [pipelined mode](doc:workers#pipelined) uses `process()` with a fixed activation block size. [Same-cycle mode](doc:workers#same-cycle) uses split-phase calls and needs application scheduling. Pump `pumpMainThread()` from the UI/control owner for deferred requests, diagnostics and native-editor work. Native window operations stay on the appropriate UI owner; your application supplies its event loop.

## Preserve state and recover {#state}

Save/load state and presets on the serialized control owner, with audio parked when required. Check failures and capacity limits. On a worker crash, audio output is silenced and health becomes disconnected; your application decides whether to bypass, recreate the instance and restore a previously saved state. There is no automatic restart loop or substitute plugin.

Isolation contains process crashes. It does not restrict OS privileges or make arbitrary plugin code meet a real-time deadline. Read [CLAP extension negotiation](doc:clap) and [worker scheduling](doc:workers).
