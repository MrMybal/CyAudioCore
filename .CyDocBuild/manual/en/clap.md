---
id: clap
title: Use CLAP extensions
description: Negotiate stable capabilities and host services.
section: Plugins
order: 7
revision: 1
parent: plugins
---

## Query capabilities {#capabilities}

Include `cyaudio/plugins/ClapExtensions.h` and link `CyAudioCore::Plugins`. `PluginInstance::clapExtensions()` returns a borrowed interface for CLAP and null for other formats. Its lifetime ends when the instance closes or is replaced. The reference ABI is CLAP 1.2.10. This library is a host; it does not export CLAP plugins.

Check `capabilities()` before calling an optional extension. Missing capabilities return `unsupported_format`. Stable extensions are negotiated from what the actual plugin implements; draft and third-party extensions are not automatically advertised, and unknown custom event spaces are not translated.

| Need | Public integration |
| --- | --- |
| Audio/note ports | Separate buses, sidechains, stable IDs and supported dialects |
| Configurations | Enumerate/configure inactive layouts; query surround/ambisonic maps |
| Parameters | Native values, text conversion, gestures, automation and per-note modulation |
| State/presets | Bounded streams, state contexts, preset loading and discovery |
| Application context | Track information, note names, voice information and remote controls |
| Editors | Native GUI negotiation, sizing/DPI, floating title and context menus |
| Scheduling | Render mode, latency/tail, process status and deferred host requests |

## Provide application services {#services}

Supply `ClapHostServices` through `PluginHostCallbacks::clap` before instance creation when you want track information or host menu callbacks available during initialization. Keep its borrowed callback context alive and serialize its updates. Advanced controls are copied across the process boundary; pointers to application code are not transferred into the worker.

For isolated providers, `PluginIsolationOptions::clapThreadPool=true` enables a worker-local **serial** task executor. It is optional and does not imply parallel rendering. An arbitrary application `executeTasks` callback is unsupported across isolation. `flushAudio()` and live audio-port activation use nonblocking request/retry handshakes and can return `would_block`.

Pump main-thread services outside audio. Logs and rescan/preset notifications use bounded delivery on that pump. Timers and supported POSIX descriptor services belong to the worker's control owner; they do not become device-callback I/O.

## Discover presets {#presets}

`ClapPresetDiscovery` requires the same isolation configuration as plugin providers. It exposes declared providers, locations, soundpacks and copied metadata. It does not scan arbitrary disks or load presets on the user's behalf. Keep discovery and preset/state operations on the control owner and handle unsupported extensions, capacity limits and worker failures.

See [loading and state](doc:plugins#state) and [worker scheduling](doc:workers).
