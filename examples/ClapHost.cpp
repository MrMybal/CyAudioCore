#include <cyaudio/plugins/ClapExtensions.h>

namespace cyaudio::examples {
// Control/UI owner. The caller supplies its routing, serialized audio owner and GUI loop.
Status openClap(const std::filesystem::path& module, std::string_view id, PluginInstance& plugin,
                const PluginIsolationOptions& isolation) {
    std::unique_ptr<PluginProvider> provider;
    auto status = createPluginProvider(provider, isolation); // Primary format is CLAP.
    if (status != Status::ok)
        return status;
    ClapHostServices services;
    services.track = ClapTrackInfo{};
    services.track->name = "Instrument";
    services.track->channels = 2;
    services.track->portType = "stereo";
    PluginHostCallbacks callbacks;
    callbacks.clap = &services; // Strings/track/callback table are copied during create().
    status = provider->create(module, id, callbacks, plugin);
    if (status != Status::ok)
        return status;
    PluginPrepareOptions spec;
    spec.sampleRate = 48000;
    spec.maximumFrames = 256;
    return plugin.activate(spec);
}
Status loadClapPreset(PluginInstance& plugin, std::string_view key) {
    auto* clap = plugin.clapExtensions();
    if (!clap)
        return Status::unsupported_format;
    // The caller must stop and deactivate before changing a preset, then refresh,
    // rebuild routing if required and activate again before resuming audio.
    const auto loaded = clap->loadPreset({ClapPresetLocationKind::plugin, {}, std::string(key)});
    return loaded == Status::ok ? plugin.refresh() : loaded;
}
} // namespace cyaudio::examples

namespace cyaudio::examples {
// Called by the application's serialized graph scheduler. Between these calls the
// scheduler executes other READY nodes; it must not spin or sleep in the device callback.
Status submitClap(PluginInstance& plugin, const PluginProcess<float>& block,
                  std::uint64_t& ticket) {
    return plugin.submit(block, ticket);
}
Status collectClap(PluginInstance& plugin, const PluginProcess<float>& block, std::uint64_t ticket,
                   bool deadlineReached) {
    const auto status = plugin.collect(block, ticket);
    if (status == Status::would_block && deadlineReached)
        (void)plugin.discard(ticket);
    return status; // would_block already produced silence; never mix an older reply.
}
} // namespace cyaudio::examples
