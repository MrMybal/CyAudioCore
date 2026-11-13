# CyDSP compatibility update — 2026-11-12

The subsequent DSP expansion at `cc18dca` is covered by the
[new integration guide](CyDSPExpansion.fr.md); this report records the earlier revision.

Validated dependency: CyDSP 0.2.0, commit
`6c48a3f8814c308e58e705707e7b9faf57b45777`. Its public API remains compatible with
the existing package requirement. No dependency version bump or public CyAudioCore
API change is needed. CyDSP, CyDAW_Project, CyUI and CyCode sources were left untouched.

## Service adaptation

The synthesizer previously looked up each wavetable sample through
`Wavetable::value`. That repeats harmonic-bank selection and bypasses the updated
CyDSP block optimization. Each voice now owns a prepared public
`WavetableOscillator`; `setWavetable` binds it on control, and MIDI/UMP frequency
changes update it before the next rendered span. Render calls the public block
processor using the existing retained mono scratch buffer.

Note starts, voice stealing and reset restart the oscillator phase. Pitch events
still split audio at their exact offsets. Borrowed table objects remain owned by
the application; replacement/moves must be serialized with readers. The public
CyDSP processor refreshes its table view each call. No private table view, FFT
source or numerical implementation is copied into CyAudioCore.

VectorOps calls already consume the updated copy/fill/addScaled implementation
through normal dispatch. Numerical dynamics improvements remain in CyDSP. No
global SIMD flag or CPU/FP state change is added by this integration.

## Executed checks

Windows x64, MSVC 19.40, Release:

| Check | Result |
| --- | --- |
| Full CyAudioCore regressions with PortAudio enabled | Passed |
| Full CyAudioCore regressions with PortAudio disabled | Passed |
| CyAudioCore regressions with AddressSanitizer and PortAudio | Passed |
| Independent CyDSP regressions, Release and AddressSanitizer | Passed |
| Independent public header compilation for both libraries | Passed in all three configurations |
| Installed and relocated CMake consumers, PortAudio ON/OFF | Passed, including wavetable synthesis |
| Matched before/after synthesis measurements, 6 paired trials | Passed, matching final-sample sentinels |

New float/double scalar/automatic tests compare against the former public
sample-lookup equations. They cover interleaved MIDI1/UMP event offsets, polyphony,
channel and per-note pitch, pan/volume, note-off/retrigger, additive output,
sub-block guard regions, empty/invalid/oversized blocks, phase continuity across
calls, voice-steal crossfades, reset, failed table binding, re-prepared/moved tables,
moved synthesizers and changed stream rates. Processing/reset allocation audits
remain active. Existing file/source/MIDI/plugin/device/worker tests also pass.

The independent CyDSP test target that was temporarily incomplete during the
earlier PortAudio integration now links and passes; it is included in these runs.
The earlier [PortAudio validation](PortAudioValidation.md) remains a historical record.

Build logs and reports are local under `build/native-logs`, `build/native-x64*.json`
and `build/cydsp-update-6c48a3f`. The [measurement report](Measurements.md) describes
the exact workload, observed gains and timing limitations.

Physical audio/MIDI and third-party installed plugins were not opened. PortAudio
uses its deterministic API fixture and miniaudio uses null devices. No current
macOS/Linux/Windows ARM64/x86/Android runtime result is claimed by this update.
No JUCE code or benchmark was imported. No CI, release or push was performed.
