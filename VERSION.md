# CyAudioCore 0.1.0

IN DEVELOPMENT. Source only, no binary release.

Separate DSP and audio-service ownership, headers, namespaces and CMake targets. Existing persistent formats are preserved. See [migration](docs/Migration.fr.md) and [validation](docs/Validation.md).

macOS and desktop Linux execution remain deferred. Physical audio/MIDI and universal plugin compatibility are unverified. SVE requires CPU/OS support. Previous monolithic validation does not establish this version’s results.
JSFX uses the portable bytecode backend; its native canvas is Windows-only. AU/AAX are provider boundaries, not built-in hosts. VST3 program/unit enumeration remains an integration gap.
