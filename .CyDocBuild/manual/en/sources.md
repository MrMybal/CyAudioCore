---
id: sources
title: Files, playback and recording
description: Keep file access in workers and render from prepared sources.
section: Guides
order: 3
revision: 1
parent: overview
---

## Select the right boundary {#files}

`AudioFileReader` and `AudioFileWriter` perform I/O and can allocate. Use them on control or worker threads. Query `audioFileFormats()` for actual read/write capabilities, channel limits and writable encodings. Platform codecs depend on the installed OS encoders; an enum entry does not guarantee an available encoder.

WAV/AIFF/AIFC/CAF integer PCM uses normalized sample conventions. Integer exports round and clip to the destination range. Empty metadata remains empty; path writes commit a completed temporary file on close. Check `write`, `flush` and `close` failures, including final replacement errors.

## Prepare file playback {#playback}

`ReadAheadSource` owns decoding/resampling work in its worker. Prepare and prefill while its audio consumer is idle. This helper prepares a stereo source and transport; it does not start playback or open an audio device.

```cpp
#include <cyaudio/sources/ReadAheadSource.h>
#include <cyaudio/sources/AudioTransport.h>

cyaudio::Status preparePlayback(const std::filesystem::path& path,
                               cyaudio::ReadAheadSource<float>& source,
                               cyaudio::AudioTransport<float>& transport) {
    cyaudio::ReadAheadOptions options;
    options.outputRate = 48000;
    options.outputChannels = 2;
    auto status = source.prepare(path, options);
    if (status != cyaudio::Status::ok) return status;
    status = source.prefill(0, 512);
    if (status != cyaudio::Status::ok) return status;
    return transport.prepare({48000, 512, 2}, source);
}
```

Enqueue `TransportAction::play` from the single control producer; `AudioTransport::process` applies commands on its audio owner. Stop the consumer before closing the source. The source outlives the transport. Short source reads report their exact frame count and zero-pad their tail.

`ReadAheadOptions` also controls page capacity, output channels/rate, time maps and stretching policy. `SourceStretchMode` distinguishes automatic, resampling and stretching choices; inspect `stretchInfo()` rather than assuming a mode or latency. Prefill can wait and must stay outside the callback. Track underruns and choose the transport's `UnderrunPolicy` deliberately.

## Record without callback I/O {#recording}

Prepare `BackgroundRecorder` with file format, encoding, maximum block size and queue capacity. Its audio producer calls `push`; a block is accepted or rejected as a whole. Monitor `droppedBlocks()`. After stopping the producer, call `finish()` on the control owner to drain/join the worker and finalize the file. Destruction is not an audio-thread operation.

Memory sources, PCM proxies, waveform caches and instrument assets use the same public [buffers and ownership conventions](doc:api#buffers).
