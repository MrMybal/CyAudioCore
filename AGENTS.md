# CyAudioCore development

C++20, namespace cyaudio, public headers under include/cyaudio. Use only public
CyDSP APIs; no reverse dependency, no UI toolkit and no changes to CyDAW/CyUI.
Original code is MIT; preserve exact upstream notices and pinned provenance.
Production archives are static; tests/measurements are explicit opt-in. Native
validation is authorized by the owner (2026-11-11). No physical devices, publishing,
pushes, CI, or antivirus changes. Prepared audio must not allocate/free/block/I/O.
Foreign plugins/scripts do not inherit the host’s real-time guarantee.
