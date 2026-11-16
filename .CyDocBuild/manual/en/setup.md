---
id: setup
title: Build and link
description: Install the static libraries and render your first block.
section: Guides
order: 1
revision: 2
parent: overview
---

## Requirements {#requirements}

Use CMake 3.24+, a C++20 compiler and CyDSP 0.3.x or 0.4.x (minimum 0.3.0). The current dependency is 0.4.0. Obtain CyDSP separately. Keep headers, archives, C++ runtime and target architecture consistent across the application and workers. Rebuild consumers when a dependency changes its C++ object layout. The installed Core package requests the exact CyDSP version used to build its static archives.

## Build from source {#build}

From the CyAudioCore directory, with a neighboring CyDSP checkout on Windows x64:

```sh
cmake -S . -B build/release -G "Visual Studio 17 2022" -A x64 -DCYAUDIOCORE_CYDSP_SOURCE_DIR=../CyDSP -DCYAUDIOCORE_BUILD_PLUGIN_WORKER=ON
cmake --build build/release --config Release
cmake --install build/release --config Release --prefix build/install
```

An explicit `CYAUDIOCORE_CYDSP_SOURCE_DIR` consumes that checkout. Without it, CMake looks for an installed CyDSP package; an existing `CyDSP::CyDSP` target can also be supplied by a parent project. Configuration downloads nothing. Production libraries are static; tests and measurements remain opt-in. System backends require their native SDKs.

`CYAUDIOCORE_BUILD_PLUGIN_WORKER=ON` builds and installs `cyaudio_plugin_worker`; it does not execute it. Enable `CYAUDIOCORE_WITH_PORTAUDIO=ON` if you want the additional [audio provider](doc:audio#providers). `Sources` requires `Files` and uses the optional CyDSP stretching target when available.

## Link an application {#link}

With the packages installed in a common prefix, place this beside `main.cpp`:

```cmake
cmake_minimum_required(VERSION 3.24)
project(MyAudioApp LANGUAGES CXX)
find_package(CyAudioCore 0.1 CONFIG REQUIRED)
add_executable(MyAudioApp main.cpp)
target_link_libraries(MyAudioApp PRIVATE CyAudioCore::CyAudioCore)
```

Set `CMAKE_PREFIX_PATH` to the installation prefix when configuring your application. The aggregate target links the modules present in that package. Request required modules through `find_package(... COMPONENTS Core Files Devices Sources Plugins)` or link their [individual targets](doc:api#modules).

## Render in memory {#first-block}

This complete `main.cpp` opens no device or file. Preparation allocates its storage; the transport renders from a borrowed memory source.

```cpp
#include <cyaudio/sources/AudioTransport.h>

int main() {
    using namespace cyaudio;
    AudioBuffer<float> samples, output;
    if (samples.resize(2, 128) != Status::ok ||
        output.resize(2, 64) != Status::ok) return 1;
    for (std::size_t ch = 0; ch < 2; ++ch)
        for (auto& sample : samples.block().channel(ch)) sample = .25f;
    MemoryAudioSource<float> source;
    AudioTransport<float> transport;
    if (source.prepare(samples.block(), 48000) != Status::ok ||
        transport.prepare({48000, 64, 2}, source) != Status::ok ||
        transport.enqueue({TransportAction::play}) != Status::ok ||
        transport.process(output.block()) != Status::ok) return 2;
    return output.block().channel(0)[0] == .25f ? 0 : 3;
}
```

The source must outlive the transport and its samples must remain immutable. In a callback, reuse the prepared output storage and check each returned status. Continue with [audio devices](doc:audio) or [file playback](doc:sources).

## Optional HRTF assets and SOFA {#hrtf}

With CyDSP 0.4, prepare HRTF banks, analytic HRIRs and HOA decoders through its public API, then use their `process` calls in your graph callbacks. Preparation belongs on the control thread. Propagate the processor's reported latency and tail to the graph; the spectral HOA design and fractional-delay policies can add latency. Core does not choose a spatial quality profile or apply a renderer automatically.

Decoded HRTF assets/cache and SOFA readers are separate, optional CyDSP targets. For a source build, `CYDSP_BUILD_HRTF_ASSETS=ON` enables `CyDSP::HrtfAssets`; `CYDSP_BUILD_SOFA=ON` also enables `CyDSP::Sofa`. Both are off by default. The compact reader is selected by `CYDSP_SOFA_COMPACT`; the complete reader requires the explicit `CYDSP_SOFA_COMPLETE` option and external HDF5/ZLIB SDKs. Obtain datasets separately and retain their licenses plus the notices distributed with CyDSP.

For an installed Core built against CyDSP 0.4 with SOFA enabled:

```cmake
find_package(CyAudioCore 0.1 CONFIG REQUIRED)
find_package(CyDSP ${CyAudioCore_CYDSP_VERSION} EXACT CONFIG REQUIRED COMPONENTS Sofa)
target_link_libraries(MyAudioApp PRIVATE CyAudioCore::CyAudioCore CyDSP::Sofa)
```

Core's aggregate does not link the reader. The host explicitly imports from memory and prepares the resulting data before processing; file access, ownership and dataset selection remain the host's responsibility.
