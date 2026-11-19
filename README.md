<table width="100%">
  <tr>
    <td width="300" align="left" valign="top">
      <a href="https://mrmybal.github.io/CyAudioCore/#/en/overview">Manual</a><br>
      <a href="https://github.com/MrMybal/CyAudioCore">Repository</a><br><br>
      <sub>README</sub><br>
      <a href="README.md">English</a> · <a href="README.fr.md">Français</a>
    </td>
    <td width="400" align="center" valign="middle"><strong>CyAudioCore</strong><br><sub>C++20 audio</sub></td>
    <td width="300" align="right" valign="top">
      <p><sub>Library license</sub><br><a href="LICENSE"><strong>MIT</strong></a></p>
      <p><sub>Version</sub><br><a href="VERSION.md"><strong>0.2.0</strong></a><br><strong>IN DEVELOPMENT</strong></p>
    </td>
  </tr>
</table>

<h1 align="center">CyAudioCore</h1>

A C++20 audio library for developers building audio engines, workstations and music applications. It provides devices, files, MIDI, transport, instruments and plugin hosts, with CyDSP supplying numerical processing.

> **IN DEVELOPMENT · 0.2.0**
>
> Available as source. Read the [changes and known limitations](VERSION.md) before integrating it.

## About CyAudioCore

Assemble an audio stream from memory or a file, control playback and recording, connect MIDI inputs and host plugins. Modules link as static libraries; the application chooses its GUI and scheduler.

Buffers, views and numerical processors come from CyDSP. Device SDKs, codecs and plugin interfaces remain private. Prepare resources on the control thread before audio processing, and keep borrowed objects and buffers alive throughout their use.

## Features

- **Audio devices:** miniaudio by default; optional PortAudio behind the same API. Select the I/O provider separately from the system backend.
- **MIDI:** devices and reconnection, MIDI 1, MPE, UMP/MIDI 2, MIDI-CI and property exchange.
- **Files and sources:** codecs, metadata, editing PCM proxies, waveform caching, read-ahead playback and background recording.
- **Transport and instruments:** play, pause, seek, loops, tempo, polyphonic synthesis and CyDSP voice orchestration.
- **Plugins:** CLAP as the primary format, with explicit VST3/LV2/JSFX alternatives; parameters, states, presets and native editors. Automatic sleep for CLAP and eligible VST3 effects; explicit policies for other processors.
- **Isolation:** plugin scans and instances run in separate processes. Pipelined mode adds one block; same-cycle mode uses `submit`/`collect`/`discard` with an application scheduler.

---

## Installation

CyAudioCore requires CMake 3.24+, a C++20 compiler and CyDSP 0.3.x or 0.4.x (minimum 0.3.0). The current dependency is CyDSP 0.4.6. Rebuild matching headers and archives for the target ABI. Current Core validation covers Windows x64. See the [version notes](VERSION.md) for platform and hardware coverage.

With CyAudioCore and CyDSP installed in the same prefix, add this to your application's `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.24)
project(MyAudioApp LANGUAGES CXX)

find_package(CyAudioCore 0.2 CONFIG REQUIRED)
add_executable(MyAudioApp main.cpp)
target_link_libraries(MyAudioApp PRIVATE CyAudioCore::CyAudioCore)
```

Pass the installation prefix through `CMAKE_PREFIX_PATH`. You can also link only the targets you need: `CyAudioCore::Core`, `Files`, `Devices`, `Sources` or `Plugins`. See the [module index](docs/Modules.md).

## Source, build and required libraries

Sources are in the [CyAudioCore repository](https://github.com/MrMybal/CyAudioCore). Supply a separate [CyDSP](https://github.com/MrMybal/CyDSP) checkout. This Windows example uses neighboring folders:

```sh
cmake -S . -B build/release -G "Visual Studio 17 2022" -A x64 -DCYAUDIOCORE_CYDSP_SOURCE_DIR=../CyDSP -DCYAUDIOCORE_BUILD_PLUGIN_WORKER=ON
cmake --build build/release --config Release
cmake --install build/release --config Release --prefix build/install
```

Configuration never downloads dependencies. Production libraries are static; tests, measurements and compile checks are off by default. Native backends require their system SDKs.

| CMake option | Default | Purpose |
| --- | --- | --- |
| `CYAUDIOCORE_CYDSP_SOURCE_DIR` | Empty | Explicit CyDSP checkout; otherwise use an installed package |
| `CYAUDIOCORE_WITH_PORTAUDIO` | OFF | Adds the PortAudio provider to `Devices` |
| `CYAUDIOCORE_BUILD_PLUGIN_WORKER` | OFF | Builds and installs the `cyaudio_plugin_worker` executable |
| `CYAUDIOCORE_BUILD_FILES`, `CYAUDIOCORE_BUILD_DEVICES`, `CYAUDIOCORE_BUILD_SOURCES`, `CYAUDIOCORE_BUILD_PLUGINS` | ON | Enables the corresponding modules |

Built-in plugin providers require an absolute worker path in `PluginIsolationOptions`. The worker must match the library ABI and protocol. The application manages audio deadlines; a separate process isolates crashes without guaranteeing third-party execution time.

See [miniaudio/PortAudio selection and backend dependencies](https://mrmybal.github.io/CyAudioCore/#/en/audio/providers), [CLAP contracts](https://mrmybal.github.io/CyAudioCore/#/en/clap) and [worker configuration](https://mrmybal.github.io/CyAudioCore/#/en/workers).

---

## License

CyAudioCore uses the [MIT license](LICENSE). Dependencies retain their own terms; keep their full notices when distributing linked code. The [license guide](docs/Licensing.md) and [provenance manifest](third_party/manifest.json) detail the components and their revisions.

| Library · source | License · terms | Use / scope |
| --- | --- | --- |
| [CyDSP](https://github.com/MrMybal/CyDSP) | MIT | Separate numerical dependency; required by every module. |
| [miniaudio](https://github.com/mackron/miniaudio) | [MIT-0](third_party/licenses/miniaudio.txt) | Default private audio I/O provider. |
| [PortAudio](https://github.com/PortAudio/portaudio) | [MIT](third_party/licenses/portaudio.txt) | Optional private static audio I/O provider. |
| [rtmidi](https://github.com/thestk/rtmidi) | [MIT](third_party/licenses/rtmidi.txt) | Private MIDI 1 device backend. |
| [choc](https://github.com/Tracktion/choc) | [ISC AND BSD-3-Clause AND LicenseRef-minimp3-Public-Domain](third_party/licenses/choc.txt) | Private audio file codecs, including their embedded permissive components. |
| [windows-midi](https://github.com/microsoft/MIDI) | [MIT](third_party/licenses/windows-midi.txt) | Private Windows MIDI Services declarations and portable MIDI 2 builders. |
| [clap](https://github.com/free-audio/clap) | [MIT](third_party/licenses/clap.txt) | Private CLAP 1.2.10 interfaces; primary plugin host. |
| [vst3-interfaces](https://github.com/steinbergmedia/vst3_pluginterfaces) | [MIT](third_party/licenses/vst3-interfaces.txt) | Private VST3 interfaces and minimal ABI implementation. |
| [lv2](https://github.com/lv2/lv2) | [ISC](third_party/licenses/lv2.txt) | Private LV2 plugin interfaces. |
| [lilv](https://github.com/lv2/lilv) | [ISC](third_party/licenses/lilv.txt) | Private LV2 discovery and hosting. |
| [serd](https://github.com/drobilla/serd) | [ISC](third_party/licenses/serd.txt) | Private RDF/Turtle parsing for LV2. |
| [sord](https://github.com/drobilla/sord) | [ISC](third_party/licenses/sord.txt) | Private RDF indexing for LV2. |
| [sratom](https://github.com/lv2/sratom) | [ISC](third_party/licenses/sratom.txt) | Private Atom/RDF serialization for LV2. |
| [zix](https://github.com/drobilla/zix) | [ISC](third_party/licenses/zix.txt) | Private LV2 support primitives. |
| [ysfx](https://github.com/jpcima/ysfx) | [Apache-2.0 AND Zlib AND MIT AND ISC AND (GPL-2.0-or-later WITH Bison-exception-2.2)](third_party/licenses/ysfx.txt) | Private JSFX/EEL2 runtime; bundled components and the Bison parser exception retain their full notices. |
| [CyDoc](https://github.com/MrMybal/CyDoc) | [MIT](Documentation/DocData/assets/licenses/CyDoc.txt) | Documentation reader and generation only; separate from the audio runtime. |

## References and links

- [Manual](https://mrmybal.github.io/CyAudioCore/#/en/overview)
- [Architecture](docs/Architecture.md)
- [Source repository](https://github.com/MrMybal/CyAudioCore)
- [Issue tracker](https://github.com/MrMybal/CyAudioCore/issues)
