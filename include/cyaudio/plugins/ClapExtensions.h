#pragma once
#include "cyaudio/plugins/Plugin.h"
#include <array>
#include <optional>
#include <utility>

namespace cyaudio {
// Portable control API for the stable CLAP extensions. No SDK types escape the archive.
enum ClapParameterRescanFlag : std::uint32_t {
    clapParameterValues = 1,
    clapParameterText = 2,
    clapParameterInfo = 4,
    clapParameterAll = 8
};
enum ClapParameterClearFlag : std::uint32_t {
    clapClearAll = 1,
    clapClearAutomations = 2,
    clapClearModulations = 4
};
enum ClapNoteDialect : std::uint32_t {
    clapNotes = 1,
    clapMidi1 = 2,
    clapMidiMpe = 4,
    clapMidi2 = 8
};
enum class ClapNoteExpression : std::uint32_t {
    volume,
    pan,
    tuning,
    vibrato,
    expression,
    brightness,
    pressure
};
enum class ClapSpeaker : std::uint8_t {
    frontLeft,
    frontRight,
    frontCenter,
    lfe,
    backLeft,
    backRight,
    frontLeftCenter,
    frontRightCenter,
    backCenter,
    sideLeft,
    sideRight,
    topCenter,
    topFrontLeft,
    topFrontCenter,
    topFrontRight,
    topBackLeft,
    topBackCenter,
    topBackRight,
    topSideLeft,
    topSideRight
};
enum class ClapStateContext : std::uint32_t { preset = 1, duplicate = 2, project = 3 };
enum class ClapProcessStatus : std::uint32_t {
    error,
    continueProcessing,
    continueIfNotQuiet,
    tail,
    sleep
};
enum class ClapPresetLocationKind : std::uint32_t { file, plugin };
struct ClapPresetLocation {
    ClapPresetLocationKind kind = ClapPresetLocationKind::plugin;
    std::string location, loadKey; // UTF-8 absolute path for file; empty location for plugin.
};
struct ClapColor {
    std::uint8_t alpha = 255, red = 0, green = 0, blue = 0;
};
struct ClapTrackInfo {
    std::optional<std::string> name;
    std::optional<ClapColor> color;
    std::uint32_t channels = 0; // Zero means unknown.
    std::string portType;
    bool returnTrack = false, bus = false, master = false;
};
struct ClapVoiceInfo {
    std::uint32_t count = 0, capacity = 0;
    bool overlappingNotes = false;
};
struct ClapNoteName {
    std::string name;
    std::int16_t port = -1, key = -1, channel = -1;
};
struct ClapRemoteControlsPage {
    std::uint32_t id = 0;
    std::string section, name;
    std::array<std::uint32_t, 8> parameters{}; // 0xffffffff means unused.
    bool presetSpecific = false;
};
struct ClapAudioConfiguration {
    std::uint32_t id = 0, inputCount = 0, outputCount = 0;
    std::string name;
    std::uint32_t mainInputChannels = 0, mainOutputChannels = 0;
    std::string mainInputType, mainOutputType;
    bool current = false, detailedPortsKnown = false;
    std::vector<PluginAudioPort> inputs, outputs;
};
struct ClapAudioPortRequest {
    bool input = true;
    std::uint32_t index = 0, channels = 0;
    std::string type;
    std::vector<std::uint8_t> surroundMap; // Native CLAP speaker IDs, when type == "surround".
    std::uint32_t ambisonicOrdering = 1, ambisonicNormalization = 1;
};
enum class ClapAutomationIndication : std::uint32_t {
    none,
    present,
    playing,
    recording,
    overriding
};
struct ClapMenuTarget {
    bool parameter = false;
    std::uint32_t id = 0xffffffffu;
};
enum class ClapMenuItemKind : std::uint32_t {
    entry,
    checkEntry,
    separator,
    beginSubmenu,
    endSubmenu,
    title
};
struct ClapMenuItem {
    ClapMenuItemKind kind = ClapMenuItemKind::entry;
    std::string label;
    std::uint32_t action = 0xffffffffu;
    bool enabled = true, checked = false;
};
struct ClapGuiHints {
    bool horizontal = false, vertical = false, preserveAspect = false;
    std::uint32_t aspectWidth = 0, aspectHeight = 0;
};
struct ClapCapabilities {
    bool audioConfigurations = false, configurationDetails = false, configurablePorts = false,
         portActivation = false, livePortActivation = false, surround = false, ambisonic = false,
         stateContext = false, presetLoad = false, remoteControls = false, noteNames = false,
         voiceInfo = false, parameterIndication = false, contextMenu = false, hardRealtime = false;
};
// Copied by create(), including the optional initial track snapshot. Callback context
// must outlive the instance. Main-thread callbacks must not destroy/reenter it.
struct ClapHostServices {
    void* context = nullptr;
    std::string name = "CyAudioCore", vendor = "MrMybal CyberAlien",
                url = "https://github.com/MrMybal/CyAudioCore", version = "0.1.0";
    std::optional<ClapTrackInfo> track;
    // Logs are copied into bounded storage and delivered only by pumpMainThread.
    void (*log)(void*, std::int32_t severity, std::string_view message) noexcept = nullptr;
    // Exact native rescan/clear flags; main thread. Cookie invalidation is internal.
    void (*parameterRescan)(void*, std::uint32_t flags) noexcept = nullptr;
    void (*parameterClear)(void*, std::uint32_t id, std::uint32_t flags) noexcept = nullptr;
    void (*suggestRemotePage)(void*, std::uint32_t id) noexcept = nullptr;
    void (*presetLoaded)(void*, const ClapPresetLocation&) noexcept = nullptr;
    void (*presetError)(void*, const ClapPresetLocation&, std::int32_t,
                        std::string_view) noexcept = nullptr;
    bool (*populateMenu)(void*, ClapMenuTarget, std::vector<ClapMenuItem>&) noexcept = nullptr;
    bool (*performMenu)(void*, ClapMenuTarget, std::uint32_t action) noexcept = nullptr;
    bool (*popupMenu)(void*, ClapMenuTarget, std::int32_t screen, std::int32_t x,
                      std::int32_t y) noexcept = nullptr;
    // Optional application executor. Called from process; must finish every task before
    // returning true, or execute none and return false. No overlapping/reentrant exec.
    // Host supplies no waiting/allocating pool. The executor owns its realtime policy.
    bool (*executeTasks)(void*, std::uint32_t count, void (*run)(void*, std::uint32_t) noexcept,
                         void*) noexcept = nullptr;
};
// Borrowed from PluginInstance::clapExtensions(), invalid after close/replacement.
// Methods are main-thread only except lastProcessStatus() and setAudioPortActive()
// on an active instance (serialized audio owner, if livePortActivation is supported).
// State/preset/port-layout changes require an inactive instance. Never call controls
// concurrently with destruction. Unsupported plugin features return unsupported_format.
class ClapExtensions {
  public:
    virtual ~ClapExtensions() = default;
    virtual ClapCapabilities capabilities() const noexcept = 0;
    virtual ClapProcessStatus lastProcessStatus() const noexcept = 0;
    virtual Status audioConfigurations(std::vector<ClapAudioConfiguration>&) noexcept = 0;
    virtual Status selectAudioConfiguration(std::uint32_t) noexcept = 0;
    virtual Status canConfigureAudioPorts(std::span<const ClapAudioPortRequest>,
                                          bool&) noexcept = 0;
    virtual Status configureAudioPorts(std::span<const ClapAudioPortRequest>) noexcept = 0;
    virtual Status setAudioPortActive(bool input, std::uint32_t index, bool active,
                                      std::uint32_t sampleBits = 0) noexcept = 0;
    virtual Status supportsSurround(std::uint64_t channelMask, bool&) noexcept = 0;
    virtual Status supportsAmbisonic(std::uint32_t ordering, std::uint32_t normalization,
                                     bool&) noexcept = 0;
    virtual Status voiceInfo(ClapVoiceInfo&) noexcept = 0;
    virtual Status noteNames(std::vector<ClapNoteName>&) noexcept = 0;
    virtual Status remoteControls(std::vector<ClapRemoteControlsPage>&) noexcept = 0;
    virtual Status setTrackInfo(const ClapTrackInfo&) noexcept = 0;
    virtual Status parameterFromText(std::uint32_t, std::string_view, double&) noexcept = 0;
    virtual Status indicateMapping(std::uint32_t, bool, const ClapColor*, std::string_view label,
                                   std::string_view description) noexcept = 0;
    virtual Status indicateAutomation(std::uint32_t, ClapAutomationIndication,
                                      const ClapColor* = nullptr) noexcept = 0;
    virtual Status saveState(ClapStateContext, std::vector<std::uint8_t>&,
                             std::size_t maximumBytes = 64 * 1024 * 1024) noexcept = 0;
    virtual Status loadState(ClapStateContext, std::span<const std::uint8_t>) noexcept = 0;
    virtual Status loadPreset(const ClapPresetLocation&) noexcept = 0;
    virtual Status contextMenu(ClapMenuTarget, std::vector<ClapMenuItem>&) noexcept = 0;
    virtual Status performContextMenu(ClapMenuTarget, std::uint32_t action) noexcept = 0;
    virtual Status editorHints(ClapGuiHints&) noexcept = 0;
    virtual Status preferredEditorApi(PluginWindowApi&, bool& floating) noexcept = 0;
    virtual Status suggestEditorTitle(std::string_view) noexcept = 0;
};

struct ClapPresetProviderInfo {
    std::string id, name, vendor;
};
struct ClapPresetFileType {
    std::string name, description, extension;
};
struct ClapPresetSearchLocation {
    std::uint32_t flags = 0;
    std::string name;
    ClapPresetLocationKind kind = ClapPresetLocationKind::plugin;
    std::string location;
};
struct ClapSoundpack {
    std::uint32_t flags = 0;
    std::string id, name, description, homepage, vendor, imagePath;
    std::uint64_t releaseTime = 0; // Zero is the CLAP unknown timestamp.
};
struct ClapPresetCatalogue {
    std::vector<ClapPresetFileType> fileTypes;
    std::vector<ClapPresetSearchLocation> locations;
    std::vector<ClapSoundpack> soundpacks;
};
struct ClapPresetMetadata {
    std::string name, loadKey, soundpackId, description;
    std::uint32_t flags = 0;
    bool flagsKnown = false;
    std::uint64_t creationTime = 0, modificationTime = 0;
    std::vector<std::pair<std::string, std::string>> pluginIds, extra;
    std::vector<std::string> creators, features;
};
// Explicit discovery of ONE selected module/provider/location; never crawls disks.
// All operations may execute foreign code and perform I/O: use a control/background
// thread, serialize each instance. A provider retains its module until close().
class ClapPresetDiscovery {
  public:
    ClapPresetDiscovery();
    ~ClapPresetDiscovery();
    ClapPresetDiscovery(ClapPresetDiscovery&&) noexcept;
    ClapPresetDiscovery& operator=(ClapPresetDiscovery&&) noexcept;
    static Status providers(const std::filesystem::path&, std::vector<ClapPresetProviderInfo>&,
                            const PluginIsolationOptions& = {}) noexcept;
    Status open(const std::filesystem::path&, std::string_view providerId,
                const PluginIsolationOptions& = {}) noexcept;
    const ClapPresetCatalogue* catalogue() const noexcept;
    Status metadata(ClapPresetLocationKind, std::string_view location,
                    std::vector<ClapPresetMetadata>&) noexcept;
    void close() noexcept;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace cyaudio
