<table width="100%">
  <tr>
    <td width="300" align="left" valign="top">
      <a href="docs/Modules.md">Documentation</a><br>
      <a href="docs/Migration.fr.md">Migration guide (French)</a><br><br>
      <sub>README</sub><br>
      <a href="README.md">English</a> · <a href="README.fr.md">Français</a>
    </td>
    <td width="400" align="center" valign="middle"><strong>CyAudioCore</strong><br><sub>C++20 audio</sub></td>
    <td width="300" align="right" valign="top">
      <p><sub>Library license</sub><br><a href="LICENSE"><strong>MIT</strong></a></p>
      <p><sub>Version</sub><br><a href="VERSION.md"><strong>0.1.0</strong></a><br><strong>IN DEVELOPMENT</strong></p>
    </td>
  </tr>
</table>

<h1 align="center">CyAudioCore</h1>

A C++20 audio framework independent of any GUI toolkit. Uses CyDSP for numerics and provides devices, files, MIDI, transport, instruments and plugin hosts.

> **IN DEVELOPMENT · 0.1.0**
>
> Source only; no binary release is published. Read the [version notes](VERSION.md).

## About CyAudioCore

The application chooses its GUI and may use CyUI. Shared buffers, types and optimized processors come from CyDSP; plugin SDKs and codecs stay private. Plugin workers belong to CyAudioCore.

## Features

- Audio/MIDI: devices, reconnection, clocks, MIDI1/MPE/UMP/MIDI2/MIDI-CI.
- Files: codecs, metadata, PCM proxy, waveform cache and instruments.
- Sources: background playback, transport and recording.
- Plugins: CLAP/VST3/LV2/JSFX, parameters, states, native editors and process isolation.

---

## Installation

Build the static archives for the target ABI using CMake 3.24+ and a C++20 compiler. CyAudioCore requires CyDSP 0.3.0. See the [migration guide](docs/Migration.fr.md).

Currently validated CyDSP version: 0.3.0. See the [dependency and archive update](docs/CyDSPCurrent.fr.md).

```cmake
find_package(CyAudioCore 0.1 CONFIG REQUIRED)
target_link_libraries(MyApplication PRIVATE CyAudioCore::CyAudioCore)
```

## Source, build and required libraries

```sh
cmake -S . -B build/release -G "Visual Studio 17 2022" -A x64 -DCYAUDIOCORE_CYDSP_SOURCE_DIR=../CyDSP
cmake --build build/release --config Release
cmake --install build/release --config Release --prefix build/install
```

Tests and measurements are off by default. scripts/run_native.py explicitly enables Windows validation; --sanitize enables AddressSanitizer. Platform backends require their system SDKs. Configuration never downloads dependencies.

---

Audio devices default to miniaudio. `CYAUDIOCORE_WITH_PORTAUDIO=ON` adds a private static provider behind the same API. `AudioProvider` selects the provider; `AudioBackend` selects the system API. See [options, example and limitations](docs/PortAudio.fr.md).

## License

CyAudioCore uses the [MIT license](LICENSE). Dependencies retain their own terms; complete notice guidance is in [Licensing.md](docs/Licensing.md).

| Library · source | License · terms | Use / scope |
| --- | --- | --- |
| [CyDSP](https://github.com/MrMybal/CyDSP) | MIT | Separate numerical dependency; private repository |
| [miniaudio](https://github.com/mackron/miniaudio) | [MIT-0](third_party/licenses/miniaudio.txt) | Unmodified private hardware-audio backend, built without engine, resource manager, decoders, encoders or generators; selected MIT-0 alternative. |
| [PortAudio](https://github.com/PortAudio/portaudio) | [MIT](third_party/licenses/portaudio.txt) | Optional private static audio I/O provider |
| [rtmidi](https://github.com/thestk/rtmidi) | [MIT](third_party/licenses/rtmidi.txt) | Unmodified private backend for physical MIDI input/output; nonbinding contribution request is not a license condition. |
| [choc](https://github.com/Tracktion/choc) | [ISC AND BSD-3-Clause AND LicenseRef-minimp3-Public-Domain](third_party/licenses/choc.txt) | Private pinned file codecs; separately adapted WAV reader/writer preserves LIST subtype/raw metadata and validates RIFF padding/lengths. Pristine upstream headers retained byte-exact. ISC wrapper and embedded permissive codec notices retained. |
| [windows-midi](https://github.com/microsoft/MIDI) | [MIT](third_party/licenses/windows-midi.txt) | Only native Windows MIDI Services COM ABI declarations and endpoint GUIDs adapted; no service, driver, SDK binary, synthesizer or GUI imported. Original interface definitions retained for pinned provenance. Stream/Function Block identity and text bit-packing adapted into original bounded portable helpers; no WinRT implementation/runtime linked. |
| [clap](https://github.com/free-audio/clap) | [MIT](third_party/licenses/clap.txt) | Pinned unmodified interface headers and minimal ABI implementation files only; original CyDSP host/export/editor adapters use private SDK types. No external plugin or GUI framework code. |
| [vst3-interfaces](https://github.com/steinbergmedia/vst3_pluginterfaces) | [MIT](third_party/licenses/vst3-interfaces.txt) | Pinned unmodified interface headers and minimal ABI implementation files only; original CyDSP host/export/editor adapters use private SDK types. No external plugin or GUI framework code. |
| [lv2](https://github.com/lv2/lv2) | [ISC](third_party/licenses/lv2.txt) | Private original CyDSP LV2 hosting dependencies. |
| [lilv](https://github.com/lv2/lilv) | [ISC](third_party/licenses/lilv.txt) | Private original CyDSP LV2 hosting dependencies. |
| [serd](https://github.com/drobilla/serd) | [ISC](third_party/licenses/serd.txt) | Private original CyDSP LV2 hosting dependencies. |
| [sord](https://github.com/drobilla/sord) | [ISC](third_party/licenses/sord.txt) | Private original CyDSP LV2 hosting dependencies. |
| [sratom](https://github.com/lv2/sratom) | [ISC](third_party/licenses/sratom.txt) | Private original CyDSP LV2 hosting dependencies. |
| [zix](https://github.com/drobilla/zix) | [ISC](third_party/licenses/zix.txt) | Private original CyDSP LV2 hosting dependencies. |
| [ysfx](https://github.com/jpcima/ysfx) | [Apache-2.0 AND Zlib AND MIT AND ISC AND (GPL-2.0-or-later WITH Bison-exception-2.2)](third_party/licenses/ysfx.txt) | Private JSFX compiler/runtime only, no JUCE/plugin application. Embedded WDL/EEL2, dr_libs and stb retain their notices. Original runtime adaptation fixes sparse-slider state serialization; keeps original notices. Generated EEL2 parser retains the Bison 2.2 exception permitting non-GPL larger applications; exception and GPL text included in the notice. Private Android API24 mutex adaptation only requests priority inheritance on platforms where that pthread API is available. |

## References and links

- [Documentation](docs/Modules.md)
- [Architecture](docs/Architecture.md)
- [Migration](docs/Migration.fr.md)
