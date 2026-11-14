#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS // std::getenv below.
#endif
#include "../../src/plugins/PluginWire.h"
#include <chrono>
#include <cstdlib>
#include <cyaudio/plugins/PluginWorker.h>
#include <string_view>
#include <thread>
namespace {
using namespace cyaudio;
using namespace cyaudio::detail::wire;
// Regression only: a hostile worker that owns a writable view of the mapping and
// lies in the shared header. Selected by CYAUDIO_TEST_WORKER_TAMPER, never by the
// command line, so the normal worker entry point stays untouched.
//   boot: reports a successful boot with a self-consistent but different geometry.
//   run:  boots honestly, then answers audio and control with out-of-bounds sizes
//         after rewriting the header dimensions the client must not trust.
//   mute: boots honestly and keeps its heartbeat, but never answers audio or control.
int hostile(std::string_view mapping, std::string_view mode) {
    Mapping memory;
    if (memory.open(mapping) != Status::ok)
        return 3;
    auto& h = *reinterpret_cast<Header*>(memory.data);
    const auto offsets = layout(h);
    h.inputCount = h.outputCount = 1;
    h.inputChannels[0] = h.outputChannels[0] = 2;
    if (mode == "boot") {
        h.frames = h.frames / 2;
        h.mappingBytes = layout(h).total;
    }
    h.boot.store(1, std::memory_order_release);
    const bool mute = mode == "mute";
    unsigned block = 0;
    while (!h.shutdown.load(std::memory_order_acquire)) {
        h.heartbeat.fetch_add(1, std::memory_order_relaxed);
        if (mute) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (h.audioState.load(std::memory_order_acquire) == 1) {
            // Dimensions a client that re-read the header would now believe.
            h.frames = 65536;
            h.events = 65536;
            h.payload = 16 * 1024 * 1024;
            h.channels = 256;
            h.outputEvents = h.outputBytes = 0;
            h.audioStatus = 0;
            auto* records = reinterpret_cast<Event*>(memory.data + offsets.outputEvents);
            const auto kind = block++ % 4;
            if (kind == 0) { // More events and payload than the mapping holds.
                h.outputEvents = 60000;
                h.outputBytes = 16 * 1024 * 1024;
            } else if (kind == 1) { // A record pointing outside the payload.
                records[0] = {};
                records[0].kind = std::uint32_t(PluginEventKind::sysex);
                records[0].port = 0;
                records[0].offset = 0x7fffff00u;
                records[0].size = 0x1000u;
                h.outputEvents = 1;
                h.outputBytes = 8;
            } else if (kind == 2) { // A status outside the Status range.
                h.audioStatus = 0x7fffffffu;
            } else { // A frame beyond the block.
                records[0] = {};
                records[0].kind = std::uint32_t(PluginEventKind::parameterValue);
                records[0].frame = 0x10000u;
                h.outputEvents = 1;
            }
            h.audioState.store(2, std::memory_order_release);
        }
        if (h.controlState.load(std::memory_order_acquire) == 1) {
            h.stateBytes = 64 * 1024 * 1024;
            h.controlBytes = 64 * 1024 * 1024;
            h.controlStatus = 0;
            h.controlState.store(2, std::memory_order_release);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return 0;
}
} // namespace
int main(int argc, char** argv) {
    if (argc != 3 || std::string_view(argv[1]) != "--cydsp-worker")
        return 2;
    if (const char* mode = std::getenv("CYAUDIO_TEST_WORKER_TAMPER"))
        return hostile(argv[2], mode);
    return cyaudio::runPluginWorker(argv[2]);
}
