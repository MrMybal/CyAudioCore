---
id: workers
title: Schedule plugin workers
description: Choose pipeline latency or nonblocking same-cycle submission.
section: Plugins
order: 6
revision: 1
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
