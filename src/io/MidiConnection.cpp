#include "cyaudio/core/DspTypes.h"
#include "cyaudio/io/MidiConnection.h"
#include "cydsp/core/SpscQueue.h"
#include "cyaudio/io/MidiDevice.h"
#include <algorithm>
#include <atomic>
namespace cyaudio {
struct MidiConnection::State {
    // Outgoing records are split by size so the audio thread moves a few dozen bytes for
    // UMP and channel messages; only SysEx pays for a full buffer. One producer sequence
    // spans both queues and restores the send order.
    struct Short {
        UmpPacket packet;
        std::uint64_t timestampNs = 0, sequence = 0;
        std::uint32_t epoch = 0;
        std::array<std::uint8_t, 3> bytes{};
        std::uint8_t size = 0;
        bool ump = false;
    };
    struct Long {
        std::array<std::uint8_t, 4096> bytes{};
        std::size_t size = 0;
        std::uint64_t timestampNs = 0, sequence = 0;
        std::uint32_t epoch = 0;
    };
    static constexpr std::size_t maximumQueuedLong = 64;
    MidiConnectionDriver driver;
    MidiReconnectOptions options;
    MidiConnectionCallbacks callbacks;
    MidiConnectionSelection selection;
    MidiEventCollector collector;
    SpscQueue<Short> outgoing;
    SpscQueue<Long> outgoingLong;
    std::uint64_t produced = 0, consumed = 0; // Audio producer / worker consumer sequences.
    MidiInput input;
    MidiOutput output;
    Midi2Device midi2;
    std::vector<MidiConnectionPort> ports;
    MidiDevicePorts nativePorts;
    std::vector<Midi2EndpointInfo> native2;
    std::atomic<std::uint32_t> epoch{0}, active{0};
    std::uint64_t lastScan = 0, lastTry = 0;
    bool use2 = false, wanted = false, attempted = false, pending = false;
    MidiConnectionMessage incoming, sending;
    Status error = Status::device_unavailable;
    void notify(MidiConnectionEvent e, Status r) noexcept {
        if (callbacks.changed)
            callbacks.changed(callbacks.context, e, r);
    }
    void disconnect() noexcept {
        active.store(0, std::memory_order_release);
        epoch.fetch_add(1, std::memory_order_acq_rel);
        collector.invalidate();
        driver.close(driver.context);
        pending = false;
    }
    static Status list(void* p, std::vector<MidiConnectionPort>& out) noexcept {
        auto& s = *static_cast<State*>(p);
        try {
            out.clear();
            if (s.use2) {
                auto r = enumerateMidi2Endpoints(s.native2);
                if (r != Status::ok)
                    return r;
                for (auto& e : s.native2)
                    out.push_back({e.id, e.name, e.receive, e.send});
            } else {
                auto r = enumerateMidiPorts(s.nativePorts);
                if (r != Status::ok)
                    return r;
                auto append = [&](const auto& list, bool receive) {
                    for (std::size_t i = 0; i < list.size(); ++i) {
                        unsigned occurrence = 0;
                        for (std::size_t j = 0; j < i; ++j)
                            if (list[j].name == list[i].name)
                                ++occurrence;
                        // Length prefix makes arbitrary port names unambiguous.
                        out.push_back({std::string(receive ? "I" : "O") +
                                           std::to_string(list[i].name.size()) + ":" +
                                           list[i].name + ":" + std::to_string(occurrence),
                                       list[i].name, receive, !receive});
                    }
                };
                append(s.nativePorts.inputs, true);
                append(s.nativePorts.outputs, false);
            }
            return Status::ok;
        } catch (...) {
            return Status::allocation_failed;
        }
    }
    static Status nativeOpen(void* p, const MidiConnectionSelection& v) noexcept {
        auto& s = *static_cast<State*>(p);
        if (s.use2) {
            if (!v.inputId.empty() && !v.outputId.empty() && v.inputId != v.outputId)
                return Status::invalid_argument;
            for (auto e : s.native2)
                if (e.id == (v.inputId.empty() ? v.outputId : v.inputId)) {
                    e.receive = !v.inputId.empty();
                    e.send = !v.outputId.empty();
                    return s.midi2.open(e);
                }
            return Status::device_unavailable;
        }
        auto index = [&](const std::string& id, bool receive) -> int {
            for (std::size_t i = 0; i < s.ports.size(); ++i)
                if (s.ports[i].id == id) {
                    std::size_t offset = receive ? i : i - s.nativePorts.inputs.size();
                    const auto& list = receive ? s.nativePorts.inputs : s.nativePorts.outputs;
                    return offset < list.size() ? int(list[offset].index) : -1;
                }
            return -1;
        };
        if (!v.inputId.empty()) {
            auto i = index(v.inputId, true);
            if (i < 0)
                return Status::device_unavailable;
            auto r = s.input.open(std::uint32_t(i), s.options.queuedMessages);
            if (r != Status::ok)
                return r;
        }
        if (!v.outputId.empty()) {
            auto i = index(v.outputId, false);
            if (i < 0)
                return Status::device_unavailable;
            auto r = s.output.open(std::uint32_t(i));
            if (r != Status::ok) {
                s.input.close();
                return r;
            }
        }
        return Status::ok;
    }
    static void nativeClose(void* p) noexcept {
        auto& s = *static_cast<State*>(p);
        s.input.close();
        s.output.close();
        s.midi2.close();
    }
    static Status nativeRead(void* p, MidiConnectionMessage& v) noexcept {
        auto& s = *static_cast<State*>(p);
        if (s.selection.inputId.empty())
            return Status::would_block;
        // Only the fields a consumer reads: clearing the whole 4 KB message on every poll,
        // including the idle ones, was pure overhead.
        v.size = 0;
        v.ump = false;
        v.timestampNs = 0;
        if (s.use2) {
            Midi2TimedPacket packet;
            auto r = s.midi2.read(packet);
            if (r == Status::ok) {
                v.ump = true;
                v.packet = packet.packet;
                v.timestampNs = packet.timestampNs;
            }
            return r;
        }
        MidiInputTimestamp time;
        auto r = s.input.readTimed(v.bytes, v.size, time);
        v.timestampNs = time.arrivalNanoseconds;
        // MidiInput reports an empty queue as end_of_stream; this driver contract uses would_block.
        return r == Status::end_of_stream ? Status::would_block : r;
    }
    static Status nativeSend(void* p, const MidiConnectionMessage& v) noexcept {
        auto& s = *static_cast<State*>(p);
        if (s.selection.outputId.empty())
            return Status::device_unavailable;
        return s.use2
                   ? (v.ump ? s.midi2.send(v.packet, v.timestampNs) : Status::unsupported_format)
                   : (v.ump ? Status::unsupported_format : s.output.send({v.bytes.data(), v.size}));
    }
    static Status nativeHealth(void* p) noexcept {
        auto& s = *static_cast<State*>(p);
        return s.use2 ? s.midi2.lastError() : Status::ok;
    }
};
MidiConnection::MidiConnection() = default;
MidiConnection::~MidiConnection() {
    close();
}
MidiConnection::MidiConnection(MidiConnection&&) noexcept = default;
MidiConnection& MidiConnection::operator=(MidiConnection&& other) noexcept {
    if (this != &other) {
        close();
        state_ = std::move(other.state_);
    }
    return *this;
}
Status MidiConnection::prepare(bool midi2, MidiReconnectOptions o,
                               MidiConnectionCallbacks c) noexcept {
    MidiConnectionDriver d;
    d.enumerate = &State::list;
    d.open = &State::nativeOpen;
    d.close = &State::nativeClose;
    d.read = &State::nativeRead;
    d.send = &State::nativeSend;
    d.health = &State::nativeHealth;
    auto r = prepare(d, o, c);
    if (r == Status::ok) {
        state_->use2 = midi2;
        state_->driver.context = state_.get();
    }
    return r;
}
Status MidiConnection::prepare(MidiConnectionDriver d, MidiReconnectOptions o,
                               MidiConnectionCallbacks c) noexcept {
    if (!d.enumerate || !d.open || !d.close || !d.read || !d.send || !d.health ||
        !o.scanIntervalMs || !o.retryIntervalMs || !o.queuedMessages || o.queuedMessages > 65536 ||
        !o.maximumReadPerPoll || o.maximumReadPerPoll > 65536)
        return Status::invalid_spec;
    try {
        auto s = std::make_unique<State>();
        s->driver = d;
        s->options = o;
        s->callbacks = c;
        auto r = s->outgoing.prepare(o.queuedMessages);
        if (r != Status::ok)
            return r;
        r = s->outgoingLong.prepare(std::min(o.queuedMessages, State::maximumQueuedLong));
        if (r != Status::ok)
            return r;
        MidiCollectorOptions collect;
        collect.messages = o.queuedMessages;
        collect.maximumMessageBytes = 4096;
        r = s->collector.prepare(collect);
        if (r != Status::ok)
            return r;
        close();
        state_.swap(s);
        return Status::ok;
    } catch (...) {
        return Status::allocation_failed;
    }
}
Status MidiConnection::enumerate(std::vector<MidiConnectionPort>& out) noexcept {
    if (!state_)
        return Status::not_prepared;
    return state_->driver.enumerate(state_->driver.context, out);
}
Status MidiConnection::open(const MidiConnectionSelection& select, std::uint64_t now) noexcept {
    if (!state_)
        return Status::not_prepared;
    if (select.inputId.empty() && select.outputId.empty())
        return Status::invalid_argument;
    try {
        auto copy = select;
        close();
        auto& s = *state_;
        s.selection = std::move(copy);
        s.wanted = true;
        s.attempted = false;
        s.lastScan = now;
        return poll(now);
    } catch (...) {
        return Status::allocation_failed;
    }
}
Status MidiConnection::poll(std::uint64_t now) noexcept {
    if (!state_)
        return Status::not_prepared;
    auto& s = *state_;
    if (!s.wanted)
        return Status::ok;
    auto exists = [&](const std::string& id, bool receive) {
        return id.empty() || std::any_of(s.ports.begin(), s.ports.end(), [&](const auto& p) {
                   return p.id == id && (receive ? p.receive : p.send);
               });
    };
    if (!s.active.load() || now < s.lastScan || now - s.lastScan >= s.options.scanIntervalMs) {
        const auto r = s.driver.enumerate(s.driver.context, s.ports);
        s.lastScan = now;
        const bool present = r == Status::ok && exists(s.selection.inputId, true) &&
                             exists(s.selection.outputId, false);
        if (s.active.load() && (!present || s.driver.health(s.driver.context) != Status::ok)) {
            s.disconnect();
            s.error = Status::device_unavailable;
            s.notify(MidiConnectionEvent::disconnected, s.error);
        }
        if (!present) {
            s.error = r == Status::ok ? Status::device_unavailable : r;
            return s.error;
        }
    }
    if (!s.active.load()) {
        if (s.attempted && now >= s.lastTry && now - s.lastTry < s.options.retryIntervalMs)
            return s.error;
        s.lastTry = now;
        s.attempted = true;
        s.error = s.driver.open(s.driver.context, s.selection);
        if (s.error != Status::ok) {
            s.driver.close(s.driver.context);
            s.notify(MidiConnectionEvent::failed, s.error);
            return s.error;
        }
        s.epoch.fetch_add(1, std::memory_order_release);
        s.active.store(1, std::memory_order_release);
        s.notify(MidiConnectionEvent::connected, Status::ok);
    }
    auto inbound = Status::ok;
    for (std::size_t i = 0; i < s.options.maximumReadPerPoll; ++i) {
        if (!s.pending) {
            auto r = s.driver.read(s.driver.context, s.incoming);
            // An empty input is not a failure, whichever of the two idle codes a driver uses.
            if (r == Status::would_block || r == Status::end_of_stream)
                break;
            if (r != Status::ok) {
                s.disconnect();
                s.error = r;
                s.notify(MidiConnectionEvent::disconnected, r);
                return r;
            }
            s.pending = true;
        }
        auto& m = s.incoming;
        auto r = m.size > m.bytes.size() ? Status::invalid_argument
                 : m.ump                 ? s.collector.pushUmp(m.timestampNs, m.packet)
                         : s.collector.pushMidi(m.timestampNs, {m.bytes.data(), m.size});
        if (r == Status::update_queue_full) {
            inbound = r; // Collector full: the message stays pending for the next poll.
            break;
        }
        // One malformed or oversized device message is dropped and reported at the end; it
        // must not hide the messages behind it or starve the output queue below.
        s.pending = false;
        if (r != Status::ok && inbound == Status::ok)
            inbound = r;
    }
    for (std::size_t i = 0; i < s.options.queuedMessages; ++i) {
        const auto* brief = s.outgoing.peek();
        const auto* large = s.outgoingLong.peek();
        // Sequences are consecutive, so the next record is unambiguous even when the producer
        // is pushing between the two peeks.
        const bool useLarge = large && large->sequence == s.consumed;
        if (!useLarge && !(brief && brief->sequence == s.consumed))
            break;
        const auto discard = [&]() noexcept {
            (void)(useLarge ? s.outgoingLong.discard() : s.outgoing.discard());
            ++s.consumed;
        };
        if ((useLarge ? large->epoch : brief->epoch) != s.epoch.load(std::memory_order_acquire)) {
            discard();
            continue;
        }
        auto& out = s.sending;
        out.ump = !useLarge && brief->ump;
        out.packet = out.ump ? brief->packet : UmpPacket{};
        out.timestampNs = useLarge ? large->timestampNs : brief->timestampNs;
        out.size = useLarge ? large->size : out.ump ? 0 : brief->size;
        if (useLarge)
            std::copy_n(large->bytes.begin(), out.size, out.bytes.begin());
        else
            std::copy_n(brief->bytes.begin(), out.size, out.bytes.begin());
        auto r = s.driver.send(s.driver.context, out);
        if (r == Status::would_block || r == Status::update_queue_full)
            return r;
        discard();
        if (r != Status::ok) {
            s.disconnect();
            s.error = r;
            s.notify(MidiConnectionEvent::disconnected, r);
            return r;
        }
    }
    return inbound;
}
void MidiConnection::close() noexcept {
    if (state_) {
        state_->wanted = false;
        state_->disconnect();
    }
}
Status MidiConnection::send(const MidiConnectionMessage& m) noexcept {
    if (!state_)
        return Status::not_prepared;
    if (m.size > m.bytes.size() ||
        (m.ump ? !m.packet.valid() : !MidiMessageView({m.bytes.data(), m.size}).valid()))
        return Status::invalid_argument;
    auto& s = *state_;
    const auto e = s.epoch.load(std::memory_order_acquire);
    if (!s.active.load(std::memory_order_acquire))
        return Status::device_unavailable;
    bool queued = false;
    if (m.ump || m.size <= 3) {
        State::Short r;
        r.ump = m.ump;
        if (m.ump)
            r.packet = m.packet;
        else {
            std::copy_n(m.bytes.begin(), m.size, r.bytes.begin());
            r.size = static_cast<std::uint8_t>(m.size);
        }
        r.timestampNs = m.timestampNs;
        r.sequence = s.produced;
        r.epoch = e;
        queued = s.outgoing.push(r);
    } else {
        State::Long r;
        std::copy_n(m.bytes.begin(), m.size, r.bytes.begin());
        r.size = m.size;
        r.timestampNs = m.timestampNs;
        r.sequence = s.produced;
        r.epoch = e;
        queued = s.outgoingLong.push(r);
    }
    if (!queued)
        return Status::update_queue_full;
    ++s.produced;
    return Status::ok;
}
Status MidiConnection::collect(const AudioClockAnchor& a, std::uint32_t n, MidiBuffer& m,
                               UmpBuffer& u) noexcept {
    return state_ ? state_->collector.collect(a, n, m, u) : Status::not_prepared;
}
bool MidiConnection::connected() const noexcept {
    return state_ && state_->active.load(std::memory_order_acquire);
}
std::uint32_t MidiConnection::generation() const noexcept {
    return state_ ? state_->epoch.load(std::memory_order_acquire) : 0;
}
} // namespace cyaudio
