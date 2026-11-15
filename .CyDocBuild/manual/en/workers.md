---
id: workers
title: Schedule plugin workers
description: Choose pipeline latency or nonblocking same-cycle submission.
section: Plugins
order: 6
revision: 2
parent: plugins
---

## Prepared capacities {#capacity}

Set `PluginIsolationOptions` before creating the provider. Default mailbox limits are 8192 frames, 4096 events, 64 channels per direction, 64 KiB of event payload and a 16 MiB control/state envelope. Activation bounds must fit these prepared resources. Large states also include serialization overhead; the envelope is not a promise that a plugin payload of exactly that size will fit.

All shared audio storage and control resources are prepared outside processing. The prepared host path does not allocate, free, wait or poll process health. It can issue a nonblocking wake-up when the child audio owner sleeps. Plugin code inside the worker retains its own behavior.

## Pipelined processing {#pipelined}

`PluginScheduling::pipelined` is the default. `process()` submits the current block and returns the previous completed block; the initial output is silence. Its block size equals the activation's `maximumFrames`, even when mailbox capacity is larger. Each serial worker boundary contributes a block of pipeline latency.

`PluginInstance::timing()` includes the pipeline when the native latency is known. Unknown native timing stays unknown; lower-level `PluginWorkerInfo` reports native and pipeline values separately. Account for this timing in your graph's latency compensation.

## Same-cycle processing {#same-cycle}

Select `PluginScheduling::sameCycle` before provider creation. Use variable block sizes within activation bounds and one serialized audio owner:

1. Call `submit(block, ticket)` to copy the audio/events into prepared shared storage. Only one request can be in flight.
2. Run other ready nodes of the application's graph.
3. Call `collect(block, ticket)` for that same ticket. `would_block` means the result is missing and the output is silent. Retry only while there is useful independent work and time before the deadline.
4. At the deadline, call `discard(ticket)`. A late result is never reused as the next block. A new submission may remain `would_block` until the abandoned work finishes.

These calls never wait. Do not spin or sleep around `collect` in a device callback. Ordinary `process()` is invalid for this mode. There is no fixed pipeline delay, but a plugin can still miss its deadline; the library does not supply your application's graph scheduler.

## Control and failure {#control}

Run activation, state RPC, editor operations and health checks outside the callback. Read-only control calls do not consume an audio ticket. Stop audio access before changing layouts or destroying/replacing the instance. On a stall or crash, choose the engine's silence/bypass/recovery policy explicitly. See [lifecycle contracts](doc:lifecycle).

## Automatic sleep {#sleep}

Keep calling the normal audio API. `PluginPrepareOptions::sleep` uses `automatic`
by default, with 2048 quiet frames and an exact-zero threshold. CLAP follows its
`sleep`, `continueIfNotQuiet`, `tail` and `continue` statuses. VST3 uses a conservative
finite-tail policy for declared audio effects without MIDI/event ports, generators,
instruments, analysers or network categories. Quiet input includes every sidechain.
Known latency and tail are drained before heuristic suspension.

LV2, JSFX and insufficient contracts remain active automatically. Select
`PluginSleepMode::whenQuiet` only when the application knows the processor can
safely be suspended on silence, including an unknown tail. A known infinite tail
prevents heuristic sleep. An incorrect opt-in can suppress future autonomous audio
or events. `disabled` keeps processing, as does offline mode. Raising
`silenceThreshold` above zero explicitly permits discarding quieter signals.

Sleep retains the loaded plugin, editor, control loop and crash surveillance. The
worker avoids audio copies, submissions and wake calls. Pipelined mode returns the
last completed audio before the following zero blocks, preserving its block delay.
Same-cycle submission creates a local ticket for normal collection/discard.

Audio on any input, MIDI/automation/input events, changed transport or a seek,
native processing/edit requests and `PluginInstance::wake()` resume processing on
the next audio block. The control thread remains responsible for pumping requests
and health; `wake()` never runs the plugin on its caller. CLAP stop/start processing
runs on the audio owner. Native requests carry generations across the process
boundary, independently of main-thread polling.

Use `sleepInfo()` to inspect `sleeping`, `processedBlocks` and `skippedBlocks`.
Counters wrap at 32 bits and reset on activation/explicit start. These are block
counts, not a measurement of project CPU consumption. The low-level `PluginWorker`
has the same inspection and wake calls. See the [activation example](doc:plugins).
