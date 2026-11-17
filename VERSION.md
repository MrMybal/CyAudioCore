# CyAudioCore 0.1.0

IN DEVELOPMENT. Source only, no binary release.

CyAudioCore provides audio services and uses the public CyDSP API for numerical processing. Existing persistent formats are preserved. See [architecture](docs/Architecture.md) and [validation](docs/Validation.md).

## Changes since the initial import

- CyDSP 0.4.5 compatibility: explicit HRTF common-delay removal and direct/FFT rendering through the transport and graph; matching installed archives, headers and workers. Support for installed 0.4 packages and exact static dependency version checks remains in place. See the [current integration](docs/CyDSPCurrent.fr.md).
- CyDSP 0.3.2 compatibility: rebuilt archives and exact-version installed-package validation; convolution composition through transport and graph callbacks with variable blocks and aliased buffers. See the [current dependency](docs/CyDSPCurrent.fr.md).

- Public plugin providers isolate every instance and scan; portable CLAP controls and preset discovery use IPC, with optional same-cycle scheduling and a dedicated worker executable. See [configuration and usage](docs/PluginIsolation.fr.md).

- CLAP becomes the primary format: stable 1.2.10 extension controls, port configurations, polyphonic modulation, presets, metadata and application services; see the [contract and boundaries](docs/Clap.fr.md).

- Requires CyDSP 0.3.0. The synthesizer renders envelopes and sampler voices through the CyDSP block forms `Adsr::process` and `SampleData::render`, with unchanged output; see [measurements](docs/Measurements.md).
- Sources: `ReadAheadSource::readAt` clears only the frames it does not write, and the read-ahead and recorder workers are woken by a non-blocking notification instead of polling every millisecond.
- Files: write failures are reported, integer PCM round-trips exactly and exports round to nearest, a file without metadata can be re-exported, path writes replace the destination only when complete, and benign RIFF/AIFF deviations are tolerated within the same bounds.
- MIDI: native MIDI 1 input no longer disconnects on an empty poll, an event that cannot fit the destination buffer is dropped and counted instead of blocking input, MIDI-CI evicts its least recently seen peer, Property Exchange completes rejected replies, and Flex Data Set Chord Name uses the standard packing (wire change).
- Plugins: the worker process runs audio on its own thread with an event wake-up (protocol version 4), keeps its bounds private from the child, recovers from a failed load and republishes latency; JSFX keeps @serialize state across activation and no longer runs @init at transport start; a rejected output event no longer silences the block.
- Plugins: automatic CLAP/finite-tail VST3 sleep, explicit quiet/disabled policies, generation-based wake requests and local silent worker blocks; protocol 4 requires matching rebuilt helpers.
- Tests: one CTest entry per suite; `cyaudio_tests <suite>...`, `--keep-going` and `--list` select and continue.

macOS and desktop Linux execution remain deferred. Physical audio/MIDI and universal plugin compatibility are unverified. SVE requires CPU/OS support. Previous monolithic validation does not establish this version’s results.
JSFX uses the portable bytecode backend; its native canvas is Windows-only. AU/AAX are provider boundaries, not built-in hosts. VST3 program/unit enumeration remains an integration gap.
