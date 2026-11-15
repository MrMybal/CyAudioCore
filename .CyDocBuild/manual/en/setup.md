---
id: setup
title: Build and link
description: Install the static libraries and render your first block.
section: Guides
order: 1
revision: 1
parent: overview
---

## Requirements {#requirements}

Use CMake 3.24+, a C++20 compiler and CyDSP 0.3.0 or later in its 0.3 series. The current dependency is 0.3.2. Obtain CyDSP separately; its repository requires access. Keep headers, archives, C++ runtime and target architecture consistent across the application and workers. Rebuild consumers when a dependency changes its C++ object layout.

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
