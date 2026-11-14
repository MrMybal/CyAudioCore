# CyAudioCore modules

| Target | Scope |
| --- | --- |
| CyAudioCore::CyAudioCore | Interface aggregate of configured static modules |
| CyAudioCore::Core | MIDI1/SMF/MPE/UMP/MIDI-CI/property services, synthesis orchestration, memory sources, transport/time maps |
| CyAudioCore::Files | Codecs, metadata, editing PCM proxy, waveform/IR assets, instrument presets/loading |
| CyAudioCore::Devices | Audio/MIDI devices, device/connection managers and native MIDI2 transports |
| CyAudioCore::Sources | File read-ahead, stretching integration and background recording ; requires Files |
| CyAudioCore::Plugins | CLAP/VST3/LV2/JSFX hosts, native editor contract and separate-process workers |

Every target consumes public CyDSP declarations. The application chooses its UI,
thread scheduling and filesystem/device access. Built-in plugin providers require
a worker and isolate discovery and instances; see [isolation](PluginIsolation.fr.md).

CLAP is the primary plugin format. See the [stable extension matrix and examples](Clap.fr.md).

See the [current validated CyDSP dependency](CyDSPCurrent.fr.md) for its revision,
archive rebuild and package compatibility checks.

See [migration](Migration.fr.md), [plugins](Plugins.md), [MIDI2](Midi2.md),
[property exchange](CiProperty.md), [DAW contracts](DawIntegration.md),
[coverage](Coverage.md), [licensing](Licensing.md) and [validation](Validation.md).

Devices supports miniaudio by default and opt-in static PortAudio behind the same public API. See [audio providers](PortAudio.fr.md).

The expanded CyDSP processors compose with these services through the same public
buffers and targets. See the [integration guide](CyDSPExpansion.fr.md) for multibus
callbacks, sidechains, analysis, granular/custom voices, spatial channels and lifetimes.
