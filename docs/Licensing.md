# Licensing and provenance

Original CyAudioCore interfaces, service algorithms and host/editor/IPC code use
the MIT license. Copyright (c) 2026 MrMybal CyberAlien. The root LICENSE is installed
with the package. CyDSP is a separately distributed MIT dependency; retain its notices
when distributing its linked code. No JUCE or CyDAW implementation was imported.

## Optional private backends

Original CyDSP interfaces/adapters remain MIT. Dependencies retain their own grants;
permissive dependencies do not require opening the consuming application's code.
The installed `share/CyDSP/licenses` directory must accompany binary distributions
as required by the retained notices. No JUCE implementation, header or binary is
used by these modules.

| Component | Exact license selection | Built use |
| --- | --- | --- |
| miniaudio | MIT-0 alternative | Audio devices; no engine or built-in codecs compiled |
| RtMidi | MIT | MIDI1 devices, private WinMM/CoreMIDI/optional ALSA backend |
| Microsoft Windows MIDI Services native declarations | MIT | Adapted private COM ABI/GUID declarations; no service/driver binary bundled |
| CHOC | ISC | WAV/FLAC/Ogg/MP3 wrappers and required header closure |
| FLAC embedded in CHOC | BSD 3-clause | FLAC decoding and encoding |
| Ogg/Vorbis embedded in CHOC | BSD 3-clause, plus retained Degener/Bormann notice | Vorbis decoding and encoding |
| minimp3 embedded in CHOC | Upstream public-domain dedication | MP3 decoding only |
| CLAP pinned C interfaces | MIT | Original host adapter and original ABI fixture plugin |
| Current Steinberg VST3 pluginterfaces | MIT | Original processor/controller/editor host adapter and original ABI fixture |

The manifest pins every imported file, including embedded codec headers, by commit,
URL and SHA-256. Optional IPP, Accelerate and PFFFT integrations are not enabled.
AIFF PCM, MIDI protocol/SMF, effects and new public adapters are original CyAudioCore code.
Pinned MIT CLAP and VST3 interface headers are imported privately; no GUI framework is imported. RtAudio, nlohmann/json, pugixml and
rigtorp/SPSCQueue are candidates rather than dependencies: the current modules use
miniaudio, CHOC JSON internally and an original bounded SPSC queue.

Native UMP adapters, MIDI-CI state machines and UMP Stream negotiation are original
CyAudioCore implementations. The private Windows ABI header retains Microsoft's copyright
and full MIT notice with its pinned IDL provenance. Platform SDKs are build/runtime
requirements rather than vendored dependencies. No MIDI specification PDF or code
from another MIDI-CI implementation is included in the package.


The original AIFC/CAF and native Media Foundation/AudioToolbox adapters are MIT
CyAudioCore code. OS codecs are discovered at runtime and are not redistributed in this
repository. Missing native encoders return an unsupported status. No JUCE,
LAME/FFmpeg or other copyleft implementation was imported for this expansion.
The bundled Windows MIDI builder references remain byte-exact,
pinned and covered by their original MIT notices and manifest records.


Plugin interfaces and the original worker keep the original MIT grant. Exact SDK
revisions and notices are pinned in the manifest. No AAX/ASIO/VST2 SDK, JUCE implementation, JUCE JSFX wrapper or copyleft LV2
host is bundled. External providers retain
their own terms. OS/native or separately licensed SDK access is not granted by
CyDSP MIT. The built-in hosts are not a GUI framework or a complete DAW.


## LV2 and JSFX

LV2, Lilv, Serd, Sord, Sratom and Zix are pinned ISC components compiled privately
inside CyAudioCore::Plugins. ysfx is Apache2.0, with embedded WDL/EEL2/LICE (Zlib-style), ISC
Base64 helpers, MIT dr_libs and stb's published licensing choices. All original
source notices remain intact; the installed ysfx notice collects embedded grants.

The generated EEL2 parser has GPL-2.0-or-later WITH Bison-exception-2.2. The
exception explicitly permits a larger work under its own license when it is not a
parser generator using this skeleton. CyAudioCore uses it as a language runtime; it is
not a Bison-style parser generator. The complete exception and GPL2 text are
retained. This does not require CyDSP or a consuming commercial DAW to adopt GPL.
The complete combined expression and per-file hashes are in the manifest; an
Apache repository label alone is not substituted for the dependency review.

Original CyAudioCore host/editor/IPC code remains MIT. Adapted runtime files retain their
upstream grants, marked modifications and notice links. The complete distribution
therefore includes licenses besides MIT; imported components are not relicensed
by changing the library's top-level license. Plugin/script redistribution is a
separate question from implementing the host. No third-party effects are bundled;
fixtures are original CyDSP test code.

The DSP-specific Signalsmith production code and upstream comparison sources belong
to CyDSP, not this repository. The manifest here inventories only the 13 service
components; it is the authority for files actually bundled here.
