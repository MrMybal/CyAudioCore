#include "detail/AudioProvider.h"
#include <algorithm>
namespace cyaudio {
Status resolveAudioDevice(std::span<const AudioDeviceInfo> list,const AudioDeviceSelector& select,std::size_t& out) noexcept {
    auto find=[&](auto predicate)->Status {
        std::size_t count=0,index=0;
        for(std::size_t i=0;i<list.size();++i) {
            const auto& d=list[i];
            if(d.provider==select.provider&&d.input==select.input&&(select.backend==AudioBackend::automatic||d.backend==select.backend)&&predicate(d)){++count;index=i;}
        }
        if(count>1)return Status::ambiguous;if(!count)return Status::device_unavailable;
        out=index;return Status::ok;
    };
    if(select.useDefault)return find([](const auto& d){return d.isDefault;});
    if(!select.persistentId.empty()) {
        auto r=find([&](const auto& d){return d.persistentIdentity&&d.persistentId==select.persistentId;});
        if(r!=Status::device_unavailable||!select.allowNameFallback)return r;
    }
    if(select.name.empty())return Status::invalid_argument;
    return find([&](const auto& d){return d.name==select.name;});
}

struct AudioDeviceContext::State {
    std::shared_ptr<detail::AudioProviderContext> implementation;
    AudioProvider provider = AudioProvider::miniaudio;
};
AudioDeviceContext::AudioDeviceContext() = default;
AudioDeviceContext::~AudioDeviceContext() = default;
AudioDeviceContext::AudioDeviceContext(AudioDeviceContext&&) noexcept = default;
AudioDeviceContext& AudioDeviceContext::operator=(AudioDeviceContext&&) noexcept = default;
Status AudioDeviceContext::prepare(bool onlyNull) noexcept {
    return prepare(AudioContextOptions{onlyNull ? AudioBackend::nullDevice : AudioBackend::automatic});
}
Status AudioDeviceContext::prepare(const AudioContextOptions& o) noexcept {
    if (unsigned(o.backend) > unsigned(AudioBackend::custom) || unsigned(o.provider) > unsigned(AudioProvider::portAudio)) return Status::unsupported_backend;
    try {
        auto next = std::make_shared<State>(); next->provider = o.provider;
        const auto r = o.provider == AudioProvider::miniaudio ? detail::createMiniaudioContext(o, next->implementation) : detail::createPortAudioContext(o, next->implementation);
        if (r == Status::ok) state_.swap(next);
        return r;
    } catch (...) { return Status::allocation_failed; }
}
AudioProvider AudioDeviceContext::provider() const noexcept { return state_ ? state_->provider : AudioProvider::miniaudio; }
AudioBackend AudioDeviceContext::backend() const noexcept { return state_ ? state_->implementation->backend() : AudioBackend::automatic; }
std::string AudioDeviceContext::backendName() const { return state_ ? state_->implementation->backendName() : std::string{}; }
Status AudioDeviceContext::enumerate(std::vector<AudioDeviceInfo>& out) noexcept { return state_ ? state_->implementation->enumerate(out) : Status::not_prepared; }
Status AudioDeviceContext::capabilities(std::string_view id, bool input, AudioDeviceCapabilities& out) noexcept { return state_ ? state_->implementation->capabilities(id, input, out) : Status::not_prepared; }
struct AudioDevice::State {
    std::shared_ptr<AudioDeviceContext::State> context;
    std::unique_ptr<detail::AudioProviderStream> stream;
};
AudioDevice::AudioDevice() = default;
AudioDevice::~AudioDevice() = default;
AudioDevice::AudioDevice(AudioDevice&&) noexcept = default;
AudioDevice& AudioDevice::operator=(AudioDevice&&) noexcept = default;
Status AudioDevice::open(AudioDeviceContext& context, const AudioDeviceConfig& c, AudioDeviceCallback cb, void* user) noexcept { return openImpl(context,c,cb,nullptr,user); }
Status AudioDevice::openWithContext(AudioDeviceContext& context, const AudioDeviceConfig& c, AudioDeviceContextCallback cb, void* user) noexcept { return openImpl(context,c,nullptr,cb,user); }
Status AudioDevice::openImpl(AudioDeviceContext& context, const AudioDeviceConfig& c, AudioDeviceCallback cb, AudioDeviceContextCallback ext, void* user) noexcept {
    if (!context.state_) return Status::not_prepared;
    const auto valid = detail::validateAudioConfig(c,cb,ext); if (valid != Status::ok) return valid;
    try {
        auto next = std::make_unique<State>(); next->context = context.state_;
        const auto r = context.state_->implementation->open(c,cb,ext,user,next->stream);
        if (r == Status::ok) state_.swap(next);
        return r;
    } catch (...) { return Status::allocation_failed; }
}
Status AudioDevice::description(AudioStreamDescription& out) const noexcept { return state_ ? state_->stream->description(out) : Status::not_prepared; }
AudioCallbackDiagnostics AudioDevice::diagnostics() const noexcept { return state_ ? state_->stream->diagnostics() : AudioCallbackDiagnostics{}; }
Status AudioDevice::start() noexcept { return state_ ? state_->stream->start() : Status::not_prepared; }
Status AudioDevice::stop() noexcept { return state_ ? state_->stream->stop() : Status::not_prepared; }
void AudioDevice::close() noexcept { state_.reset(); }
bool AudioDevice::running() const noexcept { return state_ && state_->stream->running(); }
std::uint32_t AudioDevice::sampleRate() const noexcept { return state_ ? state_->stream->sampleRate() : 0; }
std::uint32_t AudioDevice::callbackErrors() const noexcept { return diagnostics().callbackErrors; }
std::uint32_t AudioDevice::takeNotifications() noexcept { return state_ ? state_->stream->takeNotifications() : 0; }
namespace detail {
Status validateAudioConfig(const AudioDeviceConfig& c, AudioDeviceCallback cb, AudioDeviceContextCallback ext) noexcept {
    if ((!cb && !ext) || (!c.inputChannels && !c.outputChannels) || c.inputChannels > 256 || c.outputChannels > 256 ||
        !c.sampleRate || c.sampleRate > 768000 || !c.periodFrames || c.periodFrames > 1048576 ||
        !c.maxCallbackFrames || c.maxCallbackFrames > 1048576 || unsigned(c.access) > unsigned(AudioAccess::exclusive)) return Status::invalid_argument;
    auto valid = [](const auto& map, const auto& active, std::size_t count, bool output) {
        if ((!map.empty() && map.size() != count) || (!active.empty() && active.size() != count) || (!map.empty() && !active.empty())) return false;
        for (std::size_t i = 0; i < active.size(); ++i) {
            if (active[i] >= 256) return false;
            if (output) for (std::size_t j = 0; j < i; ++j) if (active[i] == active[j]) return false;
        }
        return true;
    };
    return valid(c.inputChannelMap,c.activeInputChannels,c.inputChannels,false) && valid(c.outputChannelMap,c.activeOutputChannels,c.outputChannels,true) ? Status::ok : Status::invalid_argument;
}
#if !CYAUDIOCORE_WITH_PORTAUDIO
Status createPortAudioContext(const AudioContextOptions&, std::shared_ptr<AudioProviderContext>&) noexcept { return Status::unsupported_backend; }
#endif
}
} // namespace cyaudio
