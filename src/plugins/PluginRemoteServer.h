#pragma once
#include "PluginRpc.h"
namespace cyaudio::detail {
// Owned exclusively by the worker main thread. Foreign audio diagnostics already
// pass through the native adapter's bounded queue before arriving here.
struct RemoteHost {
    wire::Header& header;
    rpc::HostSnapshot snapshot;
    ClapHostServices services;
    PluginHostCallbacks callbacks;
    std::vector<rpc::Notice> notices;
    RemoteHost(wire::Header&, rpc::HostSnapshot);
    void push(rpc::Notice) noexcept;
    bool reverse(std::span<const std::uint8_t>, std::vector<std::uint8_t>&) noexcept;
};
Status dispatchRemote(rpc::Reader&, rpc::Writer&, rpc::Op, PluginInstance&, ClapPresetDiscovery&,
                      PluginProvider&, const std::filesystem::path&, RemoteHost&) noexcept;
} // namespace cyaudio::detail
