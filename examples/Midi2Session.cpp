#include <cyaudio/CyAudioCore.h>

// Original, compile-only responder example. The host owns opened native port(s).
// CI has one serialized control owner. Musical UMP goes through a separate SPSC
// to the audio thread, where the host maps timestamps into sample offsets.
struct Midi2Session {
    cyaudio::MidiCiDevice ci;
    cyaudio::CiPropertyStore properties;
    cyaudio::Midi2Device* output = nullptr;

    cyaudio::Status prepare(cyaudio::Midi2Device& openedOutput, cyaudio::CiConfiguration config,
                          const cyaudio::CiDeviceInformation& identity,
                          std::span<const cyaudio::CiPropertyDefinition> resources = {},
                          cyaudio::CiPropertyStoreCallbacks resourceCallbacks = {}) {
        output = &openedOutput;
        cyaudio::CiCallbacks callbacks;
        callbacks.context = this;
        callbacks.send = [](void* context, unsigned group,
                            std::span<const std::uint8_t> payload) noexcept {
            return static_cast<Midi2Session*>(context)->output->sendSysEx7(group, payload);
        };
        callbacks.property = [](void* context, const cyaudio::CiPropertyMessage& message) noexcept {
            auto& self = *static_cast<Midi2Session*>(context);
            if (message.type == 0x35 || message.type == 0x37)
                return cyaudio::Status::ok;
            return self.properties.process(message);
        };
        callbacks.invalidated = [](void* context, std::uint32_t muid) noexcept {
            static_cast<Midi2Session*>(context)->properties.invalidate(muid);
        };
        config.propertyExchange = true;
        auto status = ci.prepare(config, callbacks);
        if (status == cyaudio::Status::ok)
            status = properties.prepare(ci, identity, resources, {}, resourceCallbacks);
        return status == cyaudio::Status::ok ? ci.discover() : status;
    }
    cyaudio::Status receiveCi(const cyaudio::Midi2TimedPacket& packet, std::uint64_t nowMs) noexcept {
        return ci.processUmp(packet.packet, nowMs);
    }
    void tick(std::uint64_t nowMs) noexcept {
        properties.poll(nowMs); // Also polls CI; flush retained replies and subscription updates.
    }
    // Separate single audio producer. A full queue is reported; never spin in audio.
    cyaudio::Status noteOn(unsigned group, unsigned channel, unsigned note, std::uint16_t velocity,
                         std::uint64_t timestampNs) noexcept {
        return output
                   ? output->send(cyaudio::umpNote(true, group, channel, note, velocity), timestampNs)
                   : cyaudio::Status::not_prepared;
    }
};
