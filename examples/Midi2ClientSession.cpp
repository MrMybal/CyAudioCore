#include <cyaudio/CyAudioCore.h>

// Original compile-only initiator. The host supplies already opened native ports
// and serializes receiveCi/tick/fetch/set/subscription operations on its control owner.
struct Midi2ClientSession {
    cyaudio::MidiCiDevice ci; // Must outlive properties.
    cyaudio::CiPropertyClient properties;
    cyaudio::Midi2Device* output = nullptr;
    std::uint64_t nowMs = 0;

    cyaudio::Status prepare(cyaudio::Midi2Device& openedOutput, cyaudio::CiConfiguration config,
                          cyaudio::CiPropertyClientCallbacks changes = {}) {
        output = &openedOutput;
        cyaudio::CiCallbacks callbacks;
        callbacks.context = this;
        callbacks.send = [](void* p, unsigned group, std::span<const std::uint8_t> data) noexcept {
            return static_cast<Midi2ClientSession*>(p)->output->sendSysEx7(group, data);
        };
        callbacks.property = [](void* p, const cyaudio::CiPropertyMessage& m) noexcept {
            auto& self = *static_cast<Midi2ClientSession*>(p);
            if (m.type == 0x34 || m.type == 0x36)
                return cyaudio::Status::ok; // No responder resources.
            return self.properties.process(m, self.nowMs);
        };
        callbacks.invalidated = [](void* p, std::uint32_t peer) noexcept {
            static_cast<Midi2ClientSession*>(p)->properties.invalidate(peer);
        };
        config.propertyExchange = true;
        auto r = ci.prepare(config, callbacks);
        cyaudio::CiPropertyClientOptions options;
        options.maximumBytes = config.maximumPropertyBytes;
        if (r == cyaudio::Status::ok)
            r = properties.prepare(ci, options, changes);
        return r == cyaudio::Status::ok ? ci.discover() : r;
    }
    cyaudio::Status receiveCi(const cyaudio::Midi2TimedPacket& packet, std::uint64_t now) noexcept {
        nowMs = now;
        return ci.processUmp(packet.packet, now);
    }
    void tick(std::uint64_t now) noexcept {
        nowMs = now;
        properties.poll(now);
    }
};
