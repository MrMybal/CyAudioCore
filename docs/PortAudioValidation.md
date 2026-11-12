# PortAudio validation — 2026-11-12

Subsequent compatibility checks against the completed CyDSP optimization commit,
including its independent tests, are recorded in [CyDSP update](CyDSPUpdate.md).

Windows x64, MSVC 19.40 / Visual Studio 2022, Windows SDK 10.0.26100.0.
CyDSP was read from `../CyDSP` and built into this repository's build
directories. No source in CyDSP, CyDAW_Project, CyUI or CyCode was edited.

| Check | Result |
| --- | --- |
| PortAudio ON, Release, full CyAudioCore regression executable | Passed |
| PortAudio OFF, Release, full CyAudioCore regression executable | Passed |
| PortAudio ON, AddressSanitizer, full CyAudioCore regression executable | Passed, no sanitizer finding |
| Independent compilation of every public CyAudioCore header, ON/OFF/ASan | Passed |
| Compiled minimal provider example, ON/OFF/ASan | Passed; function not invoked |
| Installed static packages, ON and OFF, copied to a different prefix | Passed |
| Independent installed `CyAudioCore::Devices` consumer, compile/link/run | Passed for both relocated prefixes, null backend only |
| Provider SDK headers absent from public/installed headers | Passed |
| Source hashes, notices, include policy and opt-in checks | Passed, 14 service components |
| Public-only CyDSP boundary / no reverse dependency | Passed |
| `git diff --check`, original integration code/docs | Passed; byte-exact vendor sources/notices excluded from whitespace normalization |

Final local build trees: `build/portaudio-on`, `build/portaudio-off`,
`build/portaudio-asan`. Logs for configuration, compilation, regression execution,
installation and consumer execution are in `build/portaudio-logs`. Installed and
relocated copies are in `build/portaudio-install` and `build/portaudio-relocated`.
Build artifacts and detailed logs remain local and ignored.

The regression suites use an injected private PortAudio function table: they do
not call real `Pa_Initialize` or enumerate/open real PortAudio devices. The Windows
PortAudio WASAPI and WinMM implementation is compiled, linked and ASan-instrumented,
but hardware processing is simulated. Existing audio tests use miniaudio null;
existing plugin tests use original fixtures and hidden windows. No physical audio
or MIDI device is opened by this validation.

The final vendor subset contains 67 byte-exact upstream files. The unused
`wasapi/mingw-include` compatibility SDK directory is excluded from the import;
the recipe uses the selected platform/compiler SDK, including the installed
Windows SDK for the executed builds.

Deterministic checks cover provider/backend separation and unavailable selections,
provider-scoped selectors and duplicate names, reordered enumeration, no false
persistent IDs or hardware capabilities, native error translation, initialization
and failure cleanup, shared runtime leases, planar direct views, interleaved
fallback at format-check and open time, variable and oversized callbacks, missing
inputs/planes/outputs, active channel mappings, callback failure silence, complete
native output, callback frame positions, translated and unknown foreign clocks,
declared stream latency, xrun flags, idempotent start, control-thread stop/abort,
finished notifications, manager generation/reconnection and failed restart cleanup.
Allocation/free audits surround the prepared callback bridge. The fake API records
an error if a PortAudio function is invoked from an audio/finished callback.

CyDSP's independent regressions passed in an initial Release build. During later
matrix builds its working tree changed externally; its independent test target
temporarily failed to link `performanceRegression`, and its compile-check archive
reported duplicate measurement entry points. These files were left untouched.
Final builds disable `CYDSP_BUILD_TESTS` / `CYDSP_COMPILE_CHECKS` and validate the
CyAudioCore suites and public headers against the current dependency library.
No fresh claim is made for the independent CyDSP test suite after that change.

To repeat the services matrix with a stable dependency checkout:

```sh
python scripts/run_native.py --services-only
python scripts/run_native.py --services-only --portaudio
python scripts/run_native.py --services-only --portaudio --sanitize
python scripts/verify_sources.py
python scripts/verify_separation.py
python scripts/audit_secrets.py --gitleaks /path/to/gitleaks
```

Unexecuted: physical audio, duplex synchronization, actual ADC/DAC latency and clock
drift, hotplug/backend disconnect behavior, exclusive-mode acceptance, actual native
layout negotiation, DirectSound optional build, macOS/CoreAudio, Linux ALSA/JACK,
Windows ARM64/x86 and other architectures. Android/WebAudio and ASIO/WDM-KS/OSS
are outside this PortAudio recipe. No universal real-time behavior of OS drivers
or foreign plugins is inferred. No push, publication or GitHub Actions was used.
