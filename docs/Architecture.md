# Architecture

Application -> CyAudioCore -> CyDSP. The application independently selects its UI,
including CyUI. Neither CyDSP nor CyAudioCore depends on CyUI or an application.
CyDSP has no reverse dependency on the service layer.

All service declarations live under include/cyaudio, namespace cyaudio. Shared
sample types/buffers/status/spec/backend are using-declarations of the exact CyDSP
types. Numerics, SIMD dispatch, interpolation, FFT, stretching and mixing remain
owned by CyDSP. Services call those processors, never include private DSP sources.

The static Core archive owns MIDI state and instrument orchestration. Devices,
Files, Sources and Plugins are independent static archives linked through public
CMake targets. CyAudioCore::CyAudioCore is a convenience INTERFACE aggregate.
Codecs, platform ABI declarations and plugin runtimes remain private.

Preparation, scanning, loading, file/device access, worker lifecycle and native
editor operations run on control/UI threads. Prepared audio exchanges use bounded
storage and ownership queues; foreign plugins/scripts retain their own behavior.
The worker API preserves its single-plugin mailbox protocol and fixed one-block
pipeline latency. Application-side grouping/scheduling and security sandboxing are
separate responsibilities. See [migration](Migration.fr.md) and [plugins](Plugins.md).

CLAP is the primary plugin format. Its stable advanced control interfaces are
exposed through original portable types in ClapExtensions.h; the upstream C ABI
stays private. Application menus, track context and an optional task executor are
negotiated services, with bounded main-thread delivery of native diagnostics.
See [CLAP contracts, worker boundary and validation](Clap.fr.md).

AudioProvider selects miniaudio (default) or the opt-in PortAudio implementation;
AudioBackend remains the system API. Private context/stream interfaces own native
resources. Both providers share a prepared planar/interleaved callback adapter
using only public CyDSP buffers, views, VectorOps and layout conversions.
See [provider contracts and limits](PortAudio.fr.md).
