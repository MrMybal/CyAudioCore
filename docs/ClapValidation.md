# CLAP validation — 2026-11-14

This records the earlier extended CLAP host baseline, before mandatory isolation.
For the current worker campaign, see [process isolation validation](PluginIsolationValidation.md).
It does not establish universal plugin compatibility.
The compiler-input SHA-256, including the pinned production vendor sources, is:

`2c7654064fedd7779aab181b590c3b6d51eb1468a3e11839fa908402ba037da8`

CyDSP 0.3.0 was consumed from an immutable local source snapshot of revision
`e3d719f8f8753a4090ee75c2df2e399c0778dea9`, input SHA-256
`02e436cccbd8cf8cdfe79d4ab0607b1e7be245ec05ca5d96e1b60fed03ab70be`.
This work neither modified the sibling dependency checkout nor built or ran its
independent test executable. Compiler inputs remained unchanged during the accepted checks.

| Check | Result |
| --- | --- |
| Windows x64 MSVC 19.40 Release, PortAudio enabled | All 26 Core suites passed |
| Windows x64 MSVC Release, PortAudio disabled | All 25 Core suites passed |
| Windows x64 AddressSanitizer, PortAudio enabled | All 26 Core suites passed, including instrumented original plugin fixtures |
| Public headers compiled independently | Passed in the three configurations |
| Installed and relocated CMake consumer | Compile/link/run passed with PortAudio on and off, including CLAP discovery/default-provider symbols |
| Public SDK boundary | No CLAP SDK include/type in public headers; no installed CLAP SDK headers |
| Provenance and DSP boundary | Pinned-source hashes/notices and public-only DSP use passed |
| Separate-process plugin tests | Existing crash/restart/state/UI/cadence/late-block tests passed; API-less floating GUI negotiation also passed |

The new deterministic suite checks native plain parameter values and flags,
per-note/port/channel/key automation and modulation, expressions and note-end,
transport ramps, UMP, variable blocks, float/double, sub-block offsets, disabled
input/output ports, unchanged input samples, port activation across refresh,
configuration changes, surround/ambisonics, voice information, native parameter text,
state contexts, menus, preset metadata and failed provider initialization cleanup.
It also checks bounded concurrent diagnostics, main-thread delivery, task execution
on a different thread, recursive pool rejection, callback ownership, native errors,
sleep/tail/quiet statuses, rejected output events, invalidated parameter cookies,
failed layout refresh and GUI destruction acknowledgement. Allocation auditing covers
the tested prepared host paths; the deliberately foreign threaded test executor is
tested separately from that audit.

## Independent reference plugins

The official [free-audio/clap-plugins](https://github.com/free-audio/clap-plugins)
repository was explicitly fetched and built headless at
`9fdaf3d667c25bb49ff7a04cd877491c48b56524`. Its source and compiled module remain in
the ignored local validation area; they are not included in the production library
or installed package. The upstream files were not modified. Its dependency pins are:

- CLAP: `195b42a004144fab0b3cf95e9c067187d15365b7`.
- clap-helpers: `a380c978f429103e08afc1d17e9efbf3e3c96ee2`.
- yas: `7e3409224fd59f9f81232aacf886a79d5dd1c3b6`.
- The repository's Wayland interface declaration: `d0bb348f1e0d244119898f3a345a63e2b7cca93c`; its GUI paths were disabled.

Upstream MIT notices and the embedded YAS Boost Software License notices were retained.
Qt, ImGui and JUCE were not linked into this headless build. CMake dependency downloads
were disabled; the required source dependencies were fetched explicitly beforehand.

Eleven reference plugins passed with both the Release host and the ASan host:
gain, DC offset, DC offset with latency, SVF, synth, ADSR, transport information,
track information, latency, offline latency and realtime requirement. Each ran
64 blocks (1/17/64/127/256 frames), 5,789 frames in total, with finite output and
state roundtrip where provided. Unity gain was checked against the expected samples;
the synth produced nonzero audio. The upstream helper's maximal checking reported
no host-misbehaviour diagnostic.

The reference plugin DLL was a Release build without ASan instrumentation; the
second run instrumented the host. This is distinct from the fully instrumented
original-fixture suite above. These are functional checks, not performance measurements.

## Boundaries

No physical audio/MIDI device or installed third-party plugin was opened. The GUI
tests use original fixtures and hidden Win32 parents; the headless reference plugins
do not establish compatibility with commercial native editors. macOS/Linux execution,
UIKit, Windows ARM64 runtime and broader commercial-plugin acceptance were not run.
No claim is made for experimental/third-party CLAP extensions or for malicious code.

The earlier campaign above predates mandatory isolation. The version-3 worker now
mirrors stable portable CLAP controls and copied host notifications, including
bounded reverse menu calls. The `pluginIsolation` suite additionally covers public
isolated providers, discovery/presets, both precisions, variable frames, split-phase
audio, late-result discard, abrupt child exits and RPC timeouts. Application task
function pointers are rejected; an optional worker-local serial executor is explicit.
See the [isolation contract](PluginIsolation.fr.md).

Accepted reports and logs are retained locally under `build/clap-completion`,
`build/clap-acceptance` and `build/native-logs`. Nothing in the production CMake
configuration runs these tools automatically.
