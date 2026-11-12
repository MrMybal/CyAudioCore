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
thread scheduling, plugin isolation policy and filesystem/device access.

See [migration](Migration.fr.md), [plugins](Plugins.md), [MIDI2](Midi2.md),
[property exchange](CiProperty.md), [DAW contracts](DawIntegration.md),
[coverage](Coverage.md), [licensing](Licensing.md) and [validation](Validation.md).
