---
id: overview
title: CyAudioCore
description: Choose the services for your audio application.
section: Guides
order: 0
revision: 2
---

## Build an audio application {#purpose}

CyAudioCore 0.2.0 is a C++20 library for audio engines, workstations and music applications. It supplies audio and MIDI devices, file services, transport, instruments and plugin hosting. CyDSP supplies the numerical processing. Your application owns its GUI, event loop and audio scheduler.

```diagram
{"type":"flow","title":"Library responsibilities","nodes":[{"id":"app","label":"Application","x":0,"y":0},{"id":"core","label":"CyAudioCore services","x":260,"y":0},{"id":"dsp","label":"CyDSP processing","x":520,"y":0}],"edges":[{"from":"app","to":"core"},{"from":"core","to":"dsp"}]}
```

This version is **in development** and is distributed as source. Build matching static archives for your application's ABI. Current Core runtime coverage is Windows x64; hardware devices and arbitrary installed plugins need evaluation in your application. Other desktop and mobile targets are not established by that coverage.

## Choose a starting point {#start}

- [Build and link](doc:setup): use the CMake package and render a block in memory.
- [Connect audio](doc:audio): select miniaudio or PortAudio and a system backend.
- [Read and record files](doc:sources): keep file I/O off the audio callback.
- [Connect MIDI](doc:midi): collect timestamped events and handle reconnects.
- [Host plugins](doc:plugins): configure mandatory process isolation and CLAP.
- [Public API and lifetimes](doc:api): select modules and respect ownership.

## Work with this manual {#navigation}

Open `Documentation.html` with the complete `DocData` folder beside it. Reading needs no server or Node.js. Use the page tree, contents, language selector and search; Ctrl+K opens search. English and French use the same page and section identifiers, so contextual links can change language without changing their destination.
