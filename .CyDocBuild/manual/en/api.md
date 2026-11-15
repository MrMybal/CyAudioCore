---
id: api
title: Public modules and buffers
description: Find the public headers and common audio conventions.
section: Reference
order: 8
revision: 1
---

## Select modules {#modules}

Public headers live under `include/cyaudio` and declarations use namespace `cyaudio`. `cyaudio/CyAudioCore.h` aggregates the service interfaces. Prefer a specific public header when you need a smaller include boundary.

| CMake target | Role |
| --- | --- |
| `CyAudioCore::CyAudioCore` | Interface aggregate of the configured static modules |
| `CyAudioCore::Core` | MIDI protocols, synthesis orchestration, memory sources and transport |
| `CyAudioCore::Files` | Codecs, metadata, PCM proxies, waveforms and instrument assets |
| `CyAudioCore::Devices` | Audio/MIDI devices and connection managers |
| `CyAudioCore::Sources` | Read-ahead, stretching integration and background recording; requires Files |
| `CyAudioCore::Plugins` | CLAP/VST3/LV2/JSFX hosting and separate-process workers |

The aggregate is an INTERFACE target, not a new combined binary archive. The installed package resolves its CyDSP and platform dependencies through CMake. Native plugin/device SDK types stay private.

## Buffers, views and formats {#buffers}

`AudioBuffer<T>`, `AudioBlock<T>`, `ProcessSpec`, `Status`, `VectorOps` and the layout conversions are the exact public CyDSP types exposed through aliases. `AudioBuffer` owns prepared storage; `AudioBlock` borrows channel pointers and frame ranges. A view does not extend its storage lifetime. Use `AudioBlock<const T>` for read-only inputs.

Prepare with a positive sample rate, channel count and maximum block size. Respect the active view dimensions and return statuses. Device callbacks retain float32; file, source and supported plugin processors can use float or double. Do not infer float64 support from a float32 plugin, or treat interleaved storage as planar channel pointers. Numerical processing and SIMD dispatch belong to CyDSP.

## Useful public entry points {#headers}

| Header | Main interfaces |
| --- | --- |
| `cyaudio/core/DspTypes.h` | Shared CyDSP types and buffer/layout aliases |
| `cyaudio/io/AudioDeviceManager.h` | `AudioDeviceManager`, reconnection and driver contracts |
| `cyaudio/io/AudioFile.h` | `AudioFileReader`, `AudioFileWriter`, format capabilities |
| `cyaudio/sources/AudioTransport.h` | `AudioTransport`, commands, tempo and position |
| `cyaudio/sources/ReadAheadSource.h` | `ReadAheadSource`, `BackgroundRecorder` |
| `cyaudio/io/MidiConnection.h` | Queued MIDI connection, clocks and generations |
| `cyaudio/plugins/Plugin.h` | Provider selection, instance lifecycle and audio exchange |
| `cyaudio/plugins/ClapExtensions.h` | Portable stable CLAP controls and preset discovery |
| `cyaudio/plugins/PluginWorker.h` | Worker process helper and lower-level IPC control |

Read [ownership and error handling](doc:lifecycle) before connecting these interfaces to an audio callback. The examples in this manual show public usage rather than implementation source listings.
