// Explicit foreign-plugin acceptance tool. Never part of the automatic regression suite.
#include <algorithm>
#include <cmath>
#include <cyaudio/plugins/ClapExtensions.h>
#include <iostream>
#include <stdexcept>

namespace {
using namespace cyaudio;
void ok(Status s) {
    if (s != Status::ok)
        throw std::runtime_error("status " + std::to_string(unsigned(s)));
}
struct Instance {
    PluginInstance value;
    ~Instance() {
        if (value.lifecycle() == PluginLifecycle::processing)
            (void)value.stop();
        if (value.lifecycle() == PluginLifecycle::active)
            (void)value.deactivate();
    }
};
} // namespace
int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: cyaudio_clap_acceptance <absolute-module> <plugin-id>\n";
        return 2;
    }
    try {
        const auto path =
            std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(argv[1])));
        const std::string id(argv[2]);
        std::unique_ptr<PluginProvider> provider;
        ok(createPluginProvider(provider));
        std::vector<PluginDescriptor> descriptors;
        ok(provider->scan(path, descriptors));
        if (std::none_of(descriptors.begin(), descriptors.end(),
                         [&](const auto& d) { return d.id == id; }))
            throw std::runtime_error("plugin id absent");
        unsigned violations = 0;
        ClapHostServices services;
        services.context = &violations;
        services.track = ClapTrackInfo{};
        services.track->name = "Acceptance";
        services.track->channels = 2;
        services.track->portType = "stereo";
        services.log = [](void* p, std::int32_t severity, std::string_view message) noexcept {
            if (severity >= 3)
                std::cerr << "CLAP diagnostic " << severity << ": " << message << '\n';
            if (severity == 5)
                ++*static_cast<unsigned*>(p);
        };
        PluginHostCallbacks callbacks;
        callbacks.clap = &services;
        Instance holder;
        auto& plugin = holder.value;
        ok(provider->create(path, id, callbacks, plugin));
        auto* ext = plugin.clapExtensions();
        if (!ext)
            throw std::runtime_error("missing CLAP facade");
        const auto* desc = plugin.description();
        PluginPrepareOptions options;
        options.maximumFrames = 256;
        ok(plugin.activate(options));
        std::vector<AudioBuffer<float>> input(desc->inputs.size()), output(desc->outputs.size());
        std::vector<AudioBlock<const float>> ins;
        std::vector<AudioBlock<float>> outs;
        for (std::size_t i = 0; i < input.size(); ++i) {
            ok(input[i].resize(desc->inputs[i].channels, 256));
            for (std::size_t ch = 0; ch < input[i].block().channels(); ++ch)
                std::fill_n(input[i].block().channel(ch).data(), 256, ch & 1 ? -.25f : .125f);
            ins.push_back(input[i].block());
        }
        for (std::size_t i = 0; i < output.size(); ++i) {
            ok(output[i].resize(desc->outputs[i].channels, 256));
            outs.push_back(output[i].block());
        }
        PluginEventBuffer events, returned;
        ok(events.prepare(128, 65536));
        ok(returned.prepare(128, 65536));
        if (!desc->noteInputs.empty() && (desc->noteInputs[0].dialects & clapNotes)) {
            PluginEvent e;
            e.kind = PluginEventKind::noteOn;
            e.noteId = 123;
            e.port = 0;
            e.channel = 0;
            e.key = 60;
            e.value = .8;
            ok(events.add(e));
        }
        PluginTransport transport;
        transport.hasTempo = transport.hasBeats = transport.hasSeconds = transport.hasSignature =
            true;
        transport.playing = true;
        transport.tempo = 123;
        std::int64_t steady = 0;
        double energy = 0;
        ok(plugin.start());
        for (unsigned block = 0; block < 64; ++block) {
            const unsigned lengths[]{1, 17, 64, 127, 256};
            const auto frames = lengths[block % 5];
            transport.beats = double(steady) * 123 / (60 * options.sampleRate);
            transport.seconds = double(steady) / options.sampleRate;
            PluginProcess<float> call{ins, outs, frames, &events, &returned, &transport, steady};
            ok(plugin.process(call));
            events.reset();
            steady += frames;
            for (const auto& bus : outs)
                for (std::size_t ch = 0; ch < bus.channels(); ++ch)
                    for (std::size_t f = 0; f < frames; ++f) {
                        const auto sample = bus.channel(ch)[f];
                        if (!std::isfinite(sample))
                            throw std::runtime_error("nonfinite plugin output");
                        energy += double(sample) * sample;
                        if (id == "com.github.free-audio.clap.gain" &&
                            std::abs(sample - (ch & 1 ? -.25f : .125f)) > .00005f)
                            throw std::runtime_error("reference unity gain mismatch");
                    }
            std::uint32_t requests = 0;
            ok(plugin.pumpMainThread(requests));
        }
        ok(plugin.stop());
        ok(plugin.deactivate());
        if (desc->state) {
            std::vector<std::uint8_t> state;
            ok(ext->saveState(ClapStateContext::project, state));
            ok(ext->loadState(ClapStateContext::project, state));
            ok(plugin.refresh());
        }
        std::uint32_t requests = 0;
        ok(plugin.pumpMainThread(requests));
        if (violations)
            throw std::runtime_error("CLAP helper reported host misbehaviour");
        if (id == "com.github.free-audio.clap.synth" && energy <= 0)
            throw std::runtime_error("synth produced no audio");
        ok(plugin.close());
        std::cout << "PASS reference " << id << " blocks=64 frames=" << steady
                  << " energy=" << energy << '\n';
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << argv[2] << ": " << e.what() << '\n';
        return 1;
    }
}
