#include "TestPaths.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cyaudio/CyAudioCore.h>
#include <iostream>
#include <initializer_list>
#include <numbers>
#include <source_location>
#include <stdexcept>
#include <thread>
namespace {
using namespace cydsp;
using namespace cyaudio;
void require(bool v, const std::source_location at = std::source_location::current()) {
    if (!v)
        throw std::runtime_error("source policy line " + std::to_string(at.line()));
}
void ok(Status s, const std::source_location at = std::source_location::current()) {
    require(s == Status::ok, at);
}
struct PageGate {
    std::uint64_t pauseAt;
    std::atomic<bool> resume{false};
    std::atomic<std::uint64_t> startedAt{0};
    std::atomic<unsigned> primes{0};
};
template <class T> class FixtureProcessor final : public SourceStretchProcessor<T> {
  public:
    explicit FixtureProcessor(PageGate* gate) : gate_(gate) {}
    bool primed = false;
    SourceStretchCapabilities capabilities() const noexcept override {
        SourceStretchCapabilities c;
        c.available = true;
        c.timeCurves = c.pitchCurves = true;
        return c;
    }
    Status prepare(const ProcessSpec&, std::size_t, CpuBackend) noexcept override {
        return Status::ok;
    }
    std::size_t inputLatency() const noexcept override {
        return 0;
    }
    std::size_t outputLatency() const noexcept override {
        return 0;
    }
    Status prime(AudioBlock<const T> input, const SourceStretchControl& c) noexcept override {
        if (gate_)
            gate_->primes.fetch_add(1, std::memory_order_relaxed);
        primed = c.mapping && input.frames() == 0;
        return primed ? Status::ok : Status::invalid_argument;
    }
    Status process(AudioBlock<const T> in, AudioBlock<T> out,
                   const SourceStretchControl& c) noexcept override {
        if (!primed || !c.mapping || in.frames() != out.frames())
            return Status::invalid_argument;
        if (gate_) {
            gate_->startedAt.store(c.outputFirst, std::memory_order_release);
            if (c.outputFirst >= gate_->pauseAt)
                gate_->resume.wait(false, std::memory_order_acquire);
        }
        for (std::size_t ch = 0; ch < out.channels(); ++ch)
            std::copy(in.channel(ch).begin(), in.channel(ch).end(), out.channel(ch).begin());
        return Status::ok;
    }
  private:
    PageGate* gate_;
};
class FixtureProvider final : public SourceStretchProvider {
  public:
    PageGate* gate = nullptr;
    Status create(SourceStretchMode mode,
                  std::unique_ptr<SourceStretchProcessor<float>>& out) noexcept override {
        if (mode != SourceStretchMode::beats && mode != SourceStretchMode::texture)
            return Status::unsupported_format;
        try {
            out = std::make_unique<FixtureProcessor<float>>(gate);
            return Status::ok;
        } catch (...) {
            return Status::allocation_failed;
        }
    }
    Status create(SourceStretchMode mode,
                  std::unique_ptr<SourceStretchProcessor<double>>& out) noexcept override {
        if (mode != SourceStretchMode::beats && mode != SourceStretchMode::texture)
            return Status::unsupported_format;
        try {
            out = std::make_unique<FixtureProcessor<double>>(gate);
            return Status::ok;
        } catch (...) {
            return Status::allocation_failed;
        }
    }
};
void maps() {
    SourceTimeMap map;
    const std::array<SourceTimePoint, 3> points{{{0, 0, 0}, {100, 100, 12}, {200, 300, 0}}};
    ok(map.prepare(points));
    SourceTimePosition p;
    ok(map.evaluate(50, p));
    require(p.sourceFrame == 50 && p.sourceFramesPerOutputFrame == 1 && p.pitchSemitones == 6);
    ok(map.evaluate(150, p));
    require(p.sourceFrame == 200 && p.sourceFramesPerOutputFrame == 2 && p.pitchSemitones == 6);
    ok(map.evaluate(250, p));
    require(p.sourceFrame == 400 && p.pitchSemitones == 0);
    require(map.prepare(std::array<SourceTimePoint, 2>{{{0, 0, 0}, {1, 1000, 0}}}) ==
            Status::invalid_argument);
    ok(map.evaluate(150, p));
    require(p.sourceFrame == 200); // Failed prepare is transactional.
    ok(map.prepare(
        std::array<SourceTimePoint, 2>{{{0, 0, 0}, {1000000000000ull, 1125000000000., 0}}}));
    ok(map.evaluate(999999999999., p));
    require(std::abs(p.sourceFrame - 1124999999998.875) < .001);
}
void recoverUnderrun(const std::filesystem::path& path, std::span<const double> samples,
                     std::size_t prefix, std::size_t pages,
                     std::initializer_list<UnderrunPolicy> policies) {
    PageGate gate{prefix};
    FixtureProvider provider;
    provider.gate = &gate;
    ReadAheadOptions options;
    options.provider = &provider;
    options.mode = SourceStretchMode::beats;
    options.pageFrames = options.controlQuantumFrames = 64;
    options.pages = pages;
    ReadAheadSource<double> source;
    ok(source.prepare(path, options));
    // Release the worker even when a failed assertion unwinds past this scope.
    struct Resume {
        PageGate& gate;
        void release() {
            gate.resume.store(true, std::memory_order_release);
            gate.resume.notify_one();
        }
        ~Resume() { release(); }
    } worker{gate};
    if (prefix)
        ok(source.prefill(0, prefix));
    AudioTransport<double> transport;
    ok(transport.prepare({48000, 128, 1}, source));
    ok(transport.enqueue({TransportAction::play}));
    AudioBuffer<double> out;
    ok(out.resize(1, 128));
    const auto block = out.block().channel(0);
    std::size_t next = 0;
    for (auto policy : policies) {
        ok(transport.process(out.block(), policy));
        const auto valid = next < prefix ? prefix - next : 0;
        require(std::equal(block.begin(), block.begin() + valid, samples.begin() + next));
        require(std::all_of(block.begin() + valid, block.end(), [](double v) { return v == 0; }));
        next += policy == UnderrunPolicy::hold ? valid : block.size();
    }
    TransportPosition position;
    ok(transport.position(position));
    require(position.underruns == policies.size() && position.frame == next);
    worker.release();
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    if (pages > 2) {
        // Starting page five proves the first four pages have been published.
        while (gate.startedAt.load(std::memory_order_acquire) < 256 &&
               std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(gate.startedAt.load(std::memory_order_acquire) >= 256);
        ok(transport.process(out.block(), UnderrunPolicy::hold));
        require(std::equal(block.begin(), block.end(), samples.begin() + next));
        require(gate.primes.load() == 1);
    } else {
        // Two pages hold just one callback. Keep the clock advancing after the
        // long stall and require the decoder to catch up on that moving timeline.
        do {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            require(next + block.size() <= samples.size());
            ok(transport.process(out.block(), UnderrunPolicy::silenceAndAdvance));
            if (std::equal(block.begin(), block.end(), samples.begin() + next))
                return;
            next += block.size();
        } while (std::chrono::steady_clock::now() < until);
        require(false);
    }
}
void reader() {
    const auto path =
        cyaudio::testSupport::temporaryDirectory() /
        ("cydsp-source-policy-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }
    } cleanup{path};
    AudioFileInfo info;
    info.sampleRate = 48000;
    info.channels = 1;
    info.encoding = AudioEncoding::float64;
    AudioBuffer<double> raw;
    ok(raw.resize(1, 48000));
    const auto sample = [](double n) {
        return .4 * std::sin(2 * std::numbers::pi * 1000 * n / 48000);
    };
    for (std::size_t i = 0; i < 48000; ++i)
        raw.block().channel(0)[i] = sample(double(i));
    AudioFileWriter writer;
    ok(writer.open(path, AudioFileFormat::wav, info, {}, 48000));
    ok(writer.write(raw.block()));
    ok(writer.close());
    const auto hold = UnderrunPolicy::hold, advance = UnderrunPolicy::silenceAndAdvance;
    for (auto policy : {hold, advance})
        for (auto prefix : {0u, 64u})
            recoverUnderrun(path, raw.block().channel(0), prefix, 8, {policy});
    recoverUnderrun(path, raw.block().channel(0), 64, 2, {advance, advance, hold, advance, hold});
    ReadAheadSource<double> source;
    ReadAheadOptions options;
    options.pageFrames = 512;
    options.mode = SourceStretchMode::repitch;
    options.durationRatio = .5;
    ok(source.prepare(path, options));
    require(source.info().frames == 24000 && source.stretchInfo().repitchAntiAliasMaximumStep == 2);
    ok(source.prefill(1000, 512));
    AudioBuffer<double> output;
    ok(output.resize(1, 256));
    std::size_t count = 0;
    ok(source.readAt(1000, output.block(), count));
    require(count == 256);
    for (std::size_t i = 0; i < 256; ++i)
        require(std::abs(output.block().channel(0)[i] - sample(double((1000 + i) * 2))) < 1e-5);
    options.durationRatio = 1;
    options.timeMap = {{0, 0, 0}, {24000, 24000, 0}, {36000, 48000, 0}};
    ok(source.prepare(path, options));
    ok(source.prefill(25000, 512));
    ok(source.readAt(25000, output.block(), count));
    for (std::size_t i = 0; i < 256; ++i)
        require(std::abs(output.block().channel(0)[i] - sample(double(26000 + i * 2))) < 1e-5);
    options.mode = SourceStretchMode::complex;
    options.controlQuantumFrames = 32;
    options.timeMap = {{0, 0, 0}, {24000, 24000, 12}, {48000, 48000, 0}};
    options.tails = SourceTailPolicy::includeAlgorithmTail;
    ok(source.prepare(path, options));
    const auto policy = source.stretchInfo();
    require(policy.timelineFrames == 48000 && policy.tailFrames > 0 &&
            source.info().frames == 48000 + policy.tailFrames && policy.controlQuantumFrames == 32);
    ok(source.prefill(20000, 512));
    ok(source.readAt(20000, output.block(), count));
    double energy = 0;
    for (double v : output.block().channel(0)) {
        require(isFinite(v));
        energy += v * v;
    }
    require(energy > 1);
    options.timeMap.clear();
    options.mode = SourceStretchMode::complex;
    options.durationRatio = .125;
    options.tails = SourceTailPolicy::trimToTimeline;
    ok(source.prepare(path, options));
    require(source.info().frames == 6000);
    options.durationRatio = 8;
    ok(source.prepare(path, options));
    require(source.info().frames == 384000);
    options.mode = SourceStretchMode::beats;
    options.durationRatio = 1;
    require(source.prepare(path, options) == Status::unsupported_format &&
            source.info().frames == 384000);
    FixtureProvider provider;
    options.provider = &provider;
    ok(source.prepare(path, options));
    require(source.stretchInfo().externalProvider &&
            source.stretchInfo().mode == SourceStretchMode::beats);
    ok(source.prefill(1000, 512));
    ok(source.readAt(1000, output.block(), count));
    for (std::size_t i = 0; i < 256; ++i)
        require(output.block().channel(0)[i] == sample(double(1000 + i)));
}
} // namespace
void sourcePolicyRegression() {
    maps();
    reader();
    std::cout
        << "PASS integration source policies/time-pitch curves/repitch/prime/tails/providers\n";
}
