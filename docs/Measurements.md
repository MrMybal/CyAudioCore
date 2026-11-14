# Measurements

The subsequent [transport output optimization](TransportOutputOptimization.fr.md)
removes a redundant clear in the production transport, with PCM regression checks.

See [isolated service path profiling](CorePathProfiling.fr.md) for the subsequent
Windows campaign covering buffers, callback adaptation, transports, DSP, synthesis,
plugin calls and worker accounting, with a read-only application header probe.

## Synthesis measurements

The synthesis measurement target is `CYAUDIOCORE_BUILD_MEASUREMENTS`, disabled by
default. `scripts/run_native.py --measure` explicitly runs oscillator, wavetable
and sampler sources with 16/64 voices, float/double, 32/128/512 stereo frames at 48 kHz.
CyDSP owns the numerical/FFT/stretch benchmarks. This is not a JUCE comparison or
an audio-device benchmark.

## Block envelopes and sampler playback with CyDSP 0.3.0

2026-11-14, Windows x64, MSVC 19.40 Release, automatic VectorOps backend **AVX512** on
this run. The baseline is CyAudioCore `c75a63c` with CyDSP `04f94e2`: one
`SampleData::value` call per frame and channel, one `Adsr::next` call per frame.
The updated path calls the CyDSP 0.3.0 block forms `SampleData::render` and
`Adsr::process` once per voice and MIDI-delimited span. The measurement source is
the same file in both executables; it now also runs a sampler source.

| 128 stereo frames | Before us/block | After us/block | Speedup |
| --- | ---: | ---: | ---: |
| sampler, float, 16 voices | 69.062 | 13.231 | 5.22x |
| sampler, float, 64 voices | 286.544 | 53.644 | 5.34x |
| sampler, double, 16 voices | 66.737 | 13.675 | 4.88x |
| sampler, double, 64 voices | 285.519 | 54.638 | 5.23x |
| oscillator, float, 16 voices | 12.037 | 10.650 | 1.13x |
| oscillator, float, 64 voices | 52.413 | 45.819 | 1.14x |
| oscillator, double, 16 voices | 11.838 | 10.481 | 1.13x |
| oscillator, double, 64 voices | 51.025 | 46.138 | 1.11x |
| wavetable, float, 16 voices | 17.306 | 15.875 | 1.09x |
| wavetable, float, 64 voices | 69.606 | 63.400 | 1.10x |
| wavetable, double, 16 voices | 16.825 | 15.369 | 1.09x |
| wavetable, double, 64 voices | 68.388 | 62.250 | 1.10x |

The sampler case plays one second of looped stereo material (256-frame loop
crossfade) at notes 36 and up with root 60, so voices read the native level and
several mip levels. Three paired trials alternate the two executables; each value
is the median of trial medians. The final-sample checksums are identical in all 36
cases, and CyDSP regressions check that both block forms return exactly the
per-call values. All rows, including 32 and 512 frames, are in
[synthesis-cydsp-0.3.0.json](measurements/synthesis-cydsp-0.3.0.json). These are
complete public `Synthesizer::process` costs on one machine, not callback
percentiles, and frequent events create shorter spans than the sustained notes
measured here.

## Integration with the optimized CyDSP wavetable processor

2026-11-12, Windows x64, MSVC 19.40 Release, `/O2`, strict FP inside the production
processing libraries, automatic VectorOps backend **AVX2** on this run. Both sides
use CyDSP commit `6c48a3f8814c308e58e705707e7b9faf57b45777`. The baseline is the
CyAudioCore `48fdb79` sample-by-sample wavetable path; the updated path uses the
public `WavetableOscillator::process` once per voice and MIDI-delimited span.
The measurement source and settings are identical in both executables.

| Wavetable, 128 stereo frames | Before us/block | After us/block | Speedup |
| --- | ---: | ---: | ---: |
| float, 16 voices | 47.319 | 18.788 | 2.52x |
| float, 64 voices | 201.037 | 83.431 | 2.41x |
| double, 16 voices | 46.509 | 18.831 | 2.47x |
| double, 64 voices | 189.422 | 77.019 | 2.46x |

These are complete public `Synthesizer::process` costs, including validation,
wave generation, envelopes and mixing. The 2048-point cycle has a DC component
only in regression tests; the measured cycle sums the first, third and seventh
harmonics. Notes are sustained at fixed pitch after the initial MIDI block.
Frequent events create shorter spans and are not covered by the timing table.

Each case warms up for 48 calls, then records 31 batches of 16 calls. Six paired
trials alternate executable order. The table uses the median of trial medians.
`p95BatchMeanNs` is a percentile of batch averages, not individual callbacks or a
real-time bound. The harness field named `checksum` is the final left-channel
sample, not a hash of all rendered samples. This sentinel matches for all 24
before/after cases in every pair; complete-sample equivalence is checked separately
by deterministic regressions. The wavetable speedups across all tested dimensions
range from 1.93x to 3.67x.

The oscillator-source controls range from 0.89x to 1.20x, with a median of 1.02x.
One control therefore has a higher median cost in this run; scheduling noise and
code layout can affect these observations. No improvement is claimed for that
source, the whole library, other CPUs or hardware I/O. No CPU affinity, frequency
policy or global floating-point setting was changed. No physical device was used.

The [result data](measurements/synthesis-cydsp-6c48a3f.json) retains all trial medians,
source/executable hashes and dimensions. Local executables and raw inputs are in
`build/cydsp-update-6c48a3f`. To reproduce, compile `cyaudio_measurements` before and
after the integration change with the same measurement source and CyDSP revision:

```sh
python scripts/compare_synthesis.py --before path/to/before.exe --after path/to/after.exe --output build/synthesis-comparison.json --trials 6
```

See [integration validation](CyDSPUpdate.md) for correctness and platform limits.
Earlier monolithic reports do not establish this version's performance.
