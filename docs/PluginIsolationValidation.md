# Plugin process isolation validation — 2026-11-14

Compiler inputs: `b5d24ee6c7a48393a075bcfaf0a310b5e7fdc9aff2f69a5b5f273b019e1d7fd1`.

The dependency remains the immutable CyDSP 0.3.0 source snapshot at
`e3d719f8f8753a4090ee75c2df2e399c0778dea9`, input SHA-256
`02e436cccbd8cf8cdfe79d4ab0607b1e7be245ec05ca5d96e1b60fed03ab70be`.
The sibling projects were not modified. Independent CyDSP tests were not built/run.

| Check | Result |
| --- | --- |
| Windows x64 MSVC 19.40 Release, PortAudio ON | All 27 Core suites passed |
| Windows x64 MSVC AddressSanitizer, PortAudio ON | All 27 Core suites passed |
| Windows x64 MSVC Release, PortAudio OFF | All 26 Core suites passed |
| Independent public headers | Compiled in all three configurations |
| Installed/relocated CMake consumer | Compiled, linked and ran with PortAudio ON/OFF |
| Official headless reference plugins, isolated Release host | All 11 passed |
| Same reference plugins, isolated ASan host | All 11 passed |
| Source/provenance and public-only DSP dependency | Passed |

The production helper option was explicitly ON for native validation. The
`pluginIsolation` suite used `cyaudio_plugin_worker` built from the shipped example;
the older protocol-tamper suite retained its deliberately hostile fixture worker.
All original fixture DLLs, Core code and worker helpers were instrumented in ASan.
The upstream reference module was the already pinned **uninstrumented Release**
headless build documented in [CLAP validation](ClapValidation.md). ASan coverage
of that campaign concerns the host/worker, not every foreign DLL instruction.

## Behaviour exercised

- Missing worker configuration, rejected relative module paths and no local fallback.
- Disposable module scans and per-instance hosts for CLAP, VST3, LV2 and JSFX.
- Abrupt controlled exits during module init, instance creation and CLAP processing;
  RPC timeout during init; successful subsequent scans and fresh instances.
- Same-cycle float32/float64 audio with 1, 17, 64, 127 and 128 frames, correct bus
  routing, output samples, copied events, ticket rejection and monotonic frame clocks.
- Deliberately delayed audio, silence on missing results, explicit discard, and no
  replay of abandoned events in a later cycle.
- Allocation audit of prepared host submit/collect; existing nonblocking mailbox,
  cadence, stalled main-thread and malformed-header/payload regressions.
- Fixed-block processing with mapping capacity larger than the active block, first
  silent block, correct subsequent samples and native-plus-pipeline timing.
- CLAP configuration/layout changes, spatial metadata, voices, note names, remote
  pages, track context, text conversion, indications, contextual state and presets.
- Reverse host menu callbacks, deferred preset notifications, floating editor/title,
  active flush, live port activation and the worker-local serial task executor.
- Isolated CLAP preset providers/catalogue/metadata and bounded malformed-list decoding.

The existing LV2 asynchronous-worker test now drains responses until observable
completion with a bounded deadline, instead of assuming completion after 20 ms.
No production DSP algorithm changed for this test correction.

## Limits

This is functional and memory-safety validation; it is not a latency/performance
benchmark. Same-cycle operation adds no fixed block delay but needs application
graph scheduling and an explicit deadline. It does not promise completion of an
arbitrary foreign plugin within a device period. Pipelined process() still adds
one block per worker boundary. No complete application graph scheduler was added.

No physical audio/MIDI device was opened. No installed commercial plugin was
scanned or launched. Windows ARM64, x86, macOS/Linux runtime and their native editor
integration were not executed. The workers preserve account privileges; OS privilege
restriction is outside this feature. Foreign DSP retains its own real-time behaviour.

Accepted reports/logs are local under `build/plugin-isolation`, `build/native-logs`
and the installed-consumer reports in `build`. See [integration and contracts](PluginIsolation.fr.md).
Nothing in the default production configuration runs these validation tools.
