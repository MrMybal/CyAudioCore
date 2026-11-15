---
id: midi
title: MIDI and instruments
description: Collect timestamped events and manage connection generations.
section: Guides
order: 4
revision: 1
parent: overview
---

## Choose a transport {#transports}

Use `MidiInput`/`MidiOutput` for low-level MIDI 1 ports, or `MidiConnection` for queued input/output and reconnection. MIDI 2 uses the native endpoint services available on each platform. MIDI-CI, profiles and property exchange are application protocols above that transport; supporting them does not prove that a particular device implements them.

Enumerate on the control owner and resolve a `MidiPortSelector` against the current catalogue. Indices are snapshots. Persist a supported identity, and enable name fallback only deliberately. Duplicate IDs/names are refused when ambiguous. Do not infer identity from a remembered enumeration position.

## Transfer events to audio {#events}

1. Prepare bounded MIDI/UMP buffers and the connection queues before playback.
2. Call connection `poll(nowMs)` on a serialized worker/control owner. There is no hidden polling thread.
3. On the audio owner, call `collect` with the current `AudioClockAnchor` and block length. Events use frame offsets within the destination block.
4. Pass the prepared events to the instrument or plugin alongside that block's audio and transport context.

Direct `MidiOutput::send` can block, especially for SysEx, and belongs on a worker. The prepared `MidiConnection` outgoing queue provides a separate audio producer boundary. Keep queue capacities explicit; inspect drop/oversize counters. For low-level input, a destination too small for a message does not consume it.

## Reconnect and release voices {#reconnect}

`MidiConnection::generation()` identifies connection epochs. After a disconnect, old-epoch events are discarded. Release held notes when the connected flag or generation changes; waiting for note-off messages from a lost port can leave stuck voices. Serialize open/close/poll outside the audio thread.

## Play an instrument {#instruments}

Prepare `Synthesizer<float>` or `Synthesizer<double>` with sample rate, maximum block size, channel count and voice capacity. Provide MIDI/UMP events at their frame offsets. Configure oscillator, wavetable, sample or custom rendering resources before use and keep borrowed tables/sample data alive. CyDSP handles the numerical generators, envelopes and sample rendering; Core orchestrates voices and MIDI behavior.

Use [public module targets](doc:api#modules) and respect [prepared audio lifetimes](doc:lifecycle#owners).
