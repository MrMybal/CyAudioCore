---
id: lifecycle
title: Lifetimes and failures
description: Prepare resources and separate control from audio ownership.
section: Reference
order: 9
revision: 1
parent: api
---

## Separate owners {#owners}

| Control/UI or worker | Serialized audio owner |
| --- | --- |
| Allocate/prepare storage; enumerate/open/close devices | Render prepared blocks and exchange bounded events |
| Load files, scan plugins and save/load state | Apply prepared transport commands and parameter events |
| Activate/deactivate plugins while audio is idle | Start/process/stop the active plugin lifecycle as documented |
| Poll managers, pump main requests and manipulate editors | Submit/collect/discard the current worker request |
| Join workers and destroy resources after audio stops | Reset processors only where the public contract permits |

Preparation can allocate and fail. The prepared Core audio path must not allocate/free, lock, wait, log or access files. Do not resize buffers, rebuild a graph, join a worker or destroy borrowed resources in the callback. Foreign plugins and scripts do not inherit this guarantee; process isolation does not change their algorithms.

Keep sources, sample/table storage, graph node contexts, callback contexts and host services alive for every user that borrows them. Serialize lifecycle changes with the owners named by each API. Before replacing a source, closing a device or destroying a plugin, stop its audio access and finish pending control work. SPSC queues have one producer and one consumer.

## Handle the returned status {#errors}

| Status | Application response |
| --- | --- |
| `not_prepared` | Prepare the object before the operation |
| `invalid_argument`, `channel_mismatch`, `block_too_large` | Correct the request, layout or prepared capacity |
| `unsupported_backend`, `unsupported_format` | Choose an explicitly supported implementation or capability |
| `would_block`, `transition_in_progress` | Respect the nonblocking state/deadline; do not wait in audio |
| `end_of_stream` | Inspect the exact frame count and documented zero padding |
| `disconnected`, `timed_out`, `io_error` | Apply the service's control-thread recovery policy |
| `allocation_failed` | Report preparation failure and retain valid existing state where promised |

Failure behavior is specific to each operation. Many control preparations preserve the previous valid object; that is not a promise to roll back already processed audio or emitted events. Device/plugin runtime paths provide documented silence on missing/failed output. Never assume every invalid-input call can safely write to an invalid output view.

## Keep epochs and timing explicit {#epochs}

Do not mix frame clocks across stream or MIDI connection generations. Preserve unknown timing/capability flags rather than replacing them with guessed values. Account for plugin pipeline/native latency and finite/infinite/unknown tails in your own graph. For deadline behavior, see [same-cycle scheduling](doc:workers#same-cycle).
