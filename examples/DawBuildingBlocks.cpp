#include <cyaudio/CyAudioCore.h>

// Original CyDSP example. No device is opened or file read by compilation.
// Control thread: prepare/prefill before callbacks. Stop callbacks before destruction.
struct DawBuildingBlocks {
    cyaudio::ReadAheadSource<float> file;
    cyaudio::AudioTransport<float> transport;
    cyaudio::Synthesizer<float> synth;
    cyaudio::MidiEventCollector collector;
    cyaudio::MidiBuffer midi;
    cyaudio::UmpBuffer ump;

    cyaudio::Status prepare(const std::filesystem::path& path) {
        cyaudio::ReadAheadOptions options;
        options.outputRate = 48000;
        options.outputChannels = 2;
        options.pageFrames = 2048;
        options.pages = 8;
        options.durationRatio = 1;
        options.pitchSemitones = 0;
        auto status = file.prepare(path, options);
        if (status != cyaudio::Status::ok)
            return status;
        const cyaudio::ProcessSpec spec{48000, 512, file.info().channels};
        status = transport.prepare(spec, file);
        if (status != cyaudio::Status::ok)
            return status;
        status = synth.prepare(spec, 64);
        if (status != cyaudio::Status::ok)
            return status;
        status = collector.prepare();
        if (status != cyaudio::Status::ok)
            return status;
        status = midi.prepare(256, 4096);
        if (status != cyaudio::Status::ok)
            return status;
        status = ump.prepare(256);
        if (status != cyaudio::Status::ok)
            return status;
        return file.prefill(0, 4096); // May wait; control thread only.
    }
    // One control producer calls enqueue; the audio callback consumes commands.
    cyaudio::Status play() noexcept {
        return transport.enqueue({cyaudio::TransportAction::play});
    }
    // Device/control producer calls collector.pushMidi/pushUmp with steady-clock ns.
    // Audio owner supplies the matching callback anchor; lifecycle changes require
    // callbacks idle. Output saturation (update_queue_full) keeps the remainder queued for
    // the next block; block_too_large means an event larger than the whole MidiBuffer was
    // dropped. In both cases the events already collected are played and the buffers reset.
    cyaudio::Status process(cyaudio::AudioBlock<float> output, cyaudio::AudioClockAnchor clock) noexcept {
        const auto collected =
            collector.collect(clock, static_cast<std::uint32_t>(output.frames()), midi, ump);
        if (collected != cyaudio::Status::ok && collected != cyaudio::Status::update_queue_full &&
            collected != cyaudio::Status::block_too_large)
            return collected;
        auto status = transport.process(output, cyaudio::UnderrunPolicy::silenceAndAdvance);
        if (status == cyaudio::Status::ok)
            status = synth.process(output, &midi, &ump, true);
        midi.reset();
        ump.reset();
        return status == cyaudio::Status::ok ? collected : status;
    }
};
