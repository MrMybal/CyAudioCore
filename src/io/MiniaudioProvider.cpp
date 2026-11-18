#include "cyaudio/core/DspTypes.h"
// Private backend: third_party/licenses/miniaudio.txt
#include "detail/AudioProvider.h"
#include "detail/AudioCallbackAdapter.h"
#include "cydsp/core/AudioBuffer.h"
#include "cydsp/core/AudioLayout.h"
#include "cydsp/math/VectorOps.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <chrono>
#include <limits>
#include <miniaudio.h>
namespace cyaudio::detail {

class MiniaudioStream;
class MiniaudioContext final : public AudioProviderContext {
  public:
    MiniaudioContext(); ~MiniaudioContext();
    MiniaudioContext(MiniaudioContext&&) noexcept;
    MiniaudioContext& operator=(MiniaudioContext&&) noexcept;
    Status prepare(bool) noexcept;
    Status prepare(const AudioContextOptions&) noexcept;
    AudioBackend backend() const noexcept override;
    std::string backendName() const override;
    Status enumerate(std::vector<AudioDeviceInfo>&) noexcept override;
    Status capabilities(std::string_view, bool, AudioDeviceCapabilities&) noexcept override;
    Status open(const AudioDeviceConfig&, AudioDeviceCallback, AudioDeviceContextCallback, void*, std::unique_ptr<AudioProviderStream>&) noexcept override;
  private:
    struct State; std::shared_ptr<State> state_; friend class MiniaudioStream;
};
class MiniaudioStream final : public AudioProviderStream {
  public:
    MiniaudioStream(); ~MiniaudioStream();
    MiniaudioStream(MiniaudioStream&&) noexcept;
    MiniaudioStream& operator=(MiniaudioStream&&) noexcept;
    Status open(MiniaudioContext&, const AudioDeviceConfig&, AudioDeviceCallback, void*) noexcept;
    Status openWithContext(MiniaudioContext&, const AudioDeviceConfig&, AudioDeviceContextCallback, void*) noexcept;
    Status openImpl(MiniaudioContext&, const AudioDeviceConfig&, AudioDeviceCallback, AudioDeviceContextCallback, void*) noexcept;
    Status description(AudioStreamDescription&) const noexcept override;
    AudioCallbackDiagnostics diagnostics() const noexcept override;
    Status start() noexcept override; Status stop() noexcept override;
    void close() noexcept;
    bool running() const noexcept override;
    std::uint32_t sampleRate() const noexcept override;
    std::uint32_t callbackErrors() const noexcept;
    std::uint32_t takeNotifications() noexcept override;
  private:
    struct State; std::unique_ptr<State> state_;
};


namespace {
ma_backend nativeBackend(AudioBackend b) {
    switch(b) {
    case AudioBackend::wasapi:return ma_backend_wasapi;
    case AudioBackend::directSound:return ma_backend_dsound;
    case AudioBackend::winmm:return ma_backend_winmm;
    case AudioBackend::coreAudio:return ma_backend_coreaudio;
    case AudioBackend::alsa:return ma_backend_alsa;
    case AudioBackend::pulseAudio:return ma_backend_pulseaudio;
    case AudioBackend::jack:return ma_backend_jack;
    case AudioBackend::aaudio:return ma_backend_aaudio;
    case AudioBackend::openSl:return ma_backend_opensl;
    case AudioBackend::webAudio:return ma_backend_webaudio;
    case AudioBackend::nullDevice:return ma_backend_null;
    default:return ma_backend_custom;
    }
}
AudioBackend publicBackend(ma_backend b) {
    for(auto v:{AudioBackend::wasapi,AudioBackend::directSound,AudioBackend::winmm,AudioBackend::coreAudio,
        AudioBackend::alsa,AudioBackend::pulseAudio,AudioBackend::jack,AudioBackend::aaudio,
        AudioBackend::openSl,AudioBackend::webAudio,AudioBackend::nullDevice})
        if(nativeBackend(v)==b)return v;
    return AudioBackend::custom;
}
std::pair<std::string,bool> identity(ma_backend backend,const ma_device_id& id,bool input) {
    std::string value;bool stable=true;
    auto bytes=[&](const auto& data,bool terminated) {
        constexpr char hex[]="0123456789abcdef";
        for(auto unit:data) {
            if(terminated&&unit==0)break;
            for(unsigned i=0;i<sizeof(unit);++i) {
                auto b=unsigned((std::uint64_t(unit)>>(i*8))&255);
                value+=hex[b>>4];value+=hex[b&15];
            }
        }
    };
    switch(backend) {
    case ma_backend_wasapi:bytes(id.wasapi,true);break;
    case ma_backend_dsound:bytes(id.dsound,false);break;
    case ma_backend_coreaudio:bytes(id.coreaudio,true);break;
    case ma_backend_alsa:bytes(id.alsa,true);stable=std::string_view(id.alsa).find("CARD=")!=std::string_view::npos;break;
    case ma_backend_pulseaudio:bytes(id.pulse,true);break;
    case ma_backend_winmm:stable=false;value=std::to_string(id.winmm);break;
    case ma_backend_aaudio:stable=false;value=std::to_string(id.aaudio);break;
    case ma_backend_opensl:stable=false;value=std::to_string(id.opensl);break;
    case ma_backend_jack:stable=false;value=std::to_string(id.jack);break;
    case ma_backend_null:stable=false;value=std::to_string(id.nullbackend);break;
    default:stable=false;break;
    }
    return {"cydsp.audio/1/"+std::to_string(unsigned(publicBackend(backend)))+
        (input?"/input/":"/output/")+value,stable};
}

}
struct MiniaudioContext::State {
    ma_context context{};
    bool ready = false;
    struct Record {
        ma_device_id id;
        bool input;
        std::string key, nativeKey;
    };
    std::vector<Record> records;
    ~State() {
        if (ready)
            ma_context_uninit(&context);
    }
};
MiniaudioContext::MiniaudioContext() = default;
MiniaudioContext::~MiniaudioContext() = default;
MiniaudioContext::MiniaudioContext(MiniaudioContext&&) noexcept = default;
MiniaudioContext& MiniaudioContext::operator=(MiniaudioContext&&) noexcept = default;
Status MiniaudioContext::prepare(bool onlyNull) noexcept {
    return prepare(AudioContextOptions{onlyNull?AudioBackend::nullDevice:AudioBackend::automatic});
}
Status MiniaudioContext::prepare(const AudioContextOptions& o) noexcept {
    if(o.backend==AudioBackend::asio||o.backend==AudioBackend::custom||
        unsigned(o.backend)>unsigned(AudioBackend::custom))return Status::unsupported_backend;
    const auto backend=nativeBackend(o.backend);
    if(o.backend!=AudioBackend::automatic&&!ma_is_backend_enabled(backend))return Status::unsupported_backend;
    try {
        auto next=std::make_shared<State>();
        if(ma_context_init(o.backend==AudioBackend::automatic?nullptr:&backend,
            o.backend==AudioBackend::automatic?0:1,nullptr,&next->context)!=MA_SUCCESS)return Status::device_unavailable;
        next->ready=true;state_.swap(next);return Status::ok;
    }catch(const std::bad_alloc&){return Status::allocation_failed;}catch(...){return Status::io_error;}
}
AudioBackend MiniaudioContext::backend()const noexcept{return state_?publicBackend(state_->context.backend):AudioBackend::automatic;}
Status MiniaudioContext::capabilities(std::string_view key,bool input,AudioDeviceCapabilities& output) noexcept {
    if(!state_)return Status::not_prepared;
    const ma_device_id* id=nullptr;
    if(!key.empty()) {
        for(auto& r:state_->records)if(r.key==key&&r.input==input){id=&r.id;break;}
        if(!id)return Status::device_unavailable;
    }
    ma_device_info info{};
    if(ma_context_get_device_info(&state_->context,input?ma_device_type_capture:ma_device_type_playback,id,&info)!=MA_SUCCESS)return Status::device_unavailable;
    try {
        AudioDeviceCapabilities next;next.nativeFormatsKnown=info.nativeDataFormatCount!=0;
        for(ma_uint32 i=0;i<info.nativeDataFormatCount&&i<64;++i) {
            const auto& f=info.nativeDataFormats[i];
            auto format=AudioSampleFormat::unknown;
            switch(f.format){case ma_format_u8:format=AudioSampleFormat::unsigned8;break;
                case ma_format_s16:format=AudioSampleFormat::signed16;break;
                case ma_format_s24:format=AudioSampleFormat::signed24;break;
                case ma_format_s32:format=AudioSampleFormat::signed32;break;
                case ma_format_f32:format=AudioSampleFormat::float32;break;default:break;}
            next.nativeFormats.push_back({format,f.channels,f.sampleRate,(f.flags&MA_DATA_FORMAT_FLAG_EXCLUSIVE_MODE)!=0});
        }
        output=std::move(next);return Status::ok;
    }catch(...){return Status::allocation_failed;}
}
Status MiniaudioContext::enumerate(std::vector<AudioDeviceInfo>& result) noexcept {
    if (!state_)
        return Status::not_prepared;
    try {
        ma_device_info *outputs = nullptr, *inputs = nullptr;
        ma_uint32 no = 0, ni = 0;
        if (ma_context_get_devices(&state_->context, &outputs, &no, &inputs, &ni) != MA_SUCCESS)
            return Status::io_error;
        std::vector<AudioDeviceInfo> next;
        for (bool input : {false, true})
            for (ma_uint32 i = 0, n = input ? ni : no; i < n; ++i) {
                const auto& device = (input ? inputs : outputs)[i];
                auto& records = state_->records;
                const auto token=identity(state_->context.backend,device.id,input);
                auto r = std::find_if(records.begin(), records.end(), [&](const State::Record& v) {
                    return v.input == input && v.nativeKey==token.first;
                });
                if (r == records.end()) {
                    records.push_back({device.id,input,token.second?token.first:"context/"+std::to_string(records.size()),token.first});
                    r = records.end() - 1;
                }
                next.push_back({r->key,device.name,input,device.isDefault!=0,token.second?token.first:std::string{},
                    publicBackend(state_->context.backend),token.second});
            }
        result.swap(next);
        return Status::ok;
    } catch (const std::bad_alloc&) {
        return Status::allocation_failed;
    } catch (...) {
        return Status::io_error;
    }
}
std::string MiniaudioContext::backendName() const {
    return state_ ? ma_get_backend_name(state_->context.backend) : std::string{};
}
struct MiniaudioStream::State {
    std::shared_ptr<MiniaudioContext::State> context;
    ma_device device{};
    bool ready = false;
    AudioCallbackAdapter adapter;
    AudioDeviceConfig config;
    std::atomic<std::uint32_t> notifications{0};
    ~State() {
        if (ready)
            ma_device_uninit(&device);
    }
    static void notify(const ma_device_notification* message) noexcept {
        auto& self = *static_cast<State*>(message->pDevice->pUserData);
        std::uint32_t flag = 0;
        switch (message->type) {
        case ma_device_notification_type_started:
            flag = 1;
            break;
        case ma_device_notification_type_stopped:
            flag = 2;
            break;
        case ma_device_notification_type_rerouted:
            flag = 4;
            break;
        case ma_device_notification_type_interruption_began:
            flag = 8;
            break;
        case ma_device_notification_type_interruption_ended:
            flag = 16;
            break;
        case ma_device_notification_type_unlocked:
            flag = 32;
            break;
        }
        self.notifications.fetch_or(flag, std::memory_order_relaxed);
    }
    static void render(ma_device* device, void* out, const void* in, ma_uint32 frames) noexcept {
        auto& s = *static_cast<State*>(device->pUserData);
        s.adapter.render(in, out, frames, false, audioSteadyNow());
    }
};
MiniaudioStream::MiniaudioStream() = default;
MiniaudioStream::~MiniaudioStream() = default;
MiniaudioStream::MiniaudioStream(MiniaudioStream&&) noexcept = default;
MiniaudioStream& MiniaudioStream::operator=(MiniaudioStream&&) noexcept = default;
Status MiniaudioStream::open(MiniaudioContext& context,const AudioDeviceConfig& c,AudioDeviceCallback cb,void* user)noexcept {
    return openImpl(context,c,cb,nullptr,user);
}
Status MiniaudioStream::openWithContext(MiniaudioContext& context,const AudioDeviceConfig& c,AudioDeviceContextCallback cb,void* user)noexcept {
    return openImpl(context,c,nullptr,cb,user);
}
Status validateMiniaudioConfig(const AudioDeviceConfig& c, AudioBackend backend) noexcept {
    if (c.access == AudioAccess::exclusive && backend != AudioBackend::wasapi &&
        backend != AudioBackend::alsa)
        return Status::unsupported_backend;
    const auto validPosition = [](unsigned position) { return position <= MA_CHANNEL_AUX_31; };
    if (!std::all_of(c.inputChannelMap.begin(), c.inputChannelMap.end(), validPosition) ||
        !std::all_of(c.outputChannelMap.begin(), c.outputChannelMap.end(), validPosition))
        return Status::invalid_argument;
    return Status::ok;
}
Status MiniaudioStream::openImpl(MiniaudioContext& context, const AudioDeviceConfig& c,
                         AudioDeviceCallback callback,AudioDeviceContextCallback extended, void* user) noexcept {
    if (!context.state_)
        return Status::not_prepared;
    const auto valid = validateAudioConfig(c, callback, extended);
    if (valid != Status::ok)
        return valid;
    const auto supported = validateMiniaudioConfig(c, context.backend());
    if (supported != Status::ok)
        return supported;
    try {
        auto next = std::make_unique<State>();
        next->context = context.state_;
        next->config = c;
        auto config = ma_device_config_init(
            c.inputChannels ? (c.outputChannels ? ma_device_type_duplex : ma_device_type_capture)
                            : ma_device_type_playback);
        config.capture.shareMode=config.playback.shareMode=c.access==AudioAccess::exclusive?ma_share_mode_exclusive:ma_share_mode_shared;
        std::vector<ma_channel> inMap(c.inputChannelMap.begin(),c.inputChannelMap.end()),outMap(c.outputChannelMap.begin(),c.outputChannelMap.end());
        if(!inMap.empty())config.capture.pChannelMap=inMap.data();
        if(!outMap.empty())config.playback.pChannelMap=outMap.data();
        config.sampleRate = c.sampleRate;
        config.periodSizeInFrames = c.periodFrames;
        config.capture.format = ma_format_f32;
        config.playback.format = ma_format_f32;
        // Active channels are indices into the device's own channels: zero asks miniaudio
        // for the native count, so no channel conversion can remix or drop them. Any
        // subset is then valid, not only one that ends on the device's last channel.
        config.capture.channels = c.activeInputChannels.empty()?static_cast<ma_uint32>(c.inputChannels):0;
        config.playback.channels = c.activeOutputChannels.empty()?static_cast<ma_uint32>(c.outputChannels):0;
        // The adapter writes every native output sample of every callback itself.
        config.noPreSilencedOutputBuffer = MA_TRUE;
        auto select = [&](const std::string& key, bool input, const ma_device_id*& id) {
            if (key.empty())
                return true;
            for (const auto& r : context.state_->records)
                if (r.key == key && r.input == input) {
                    id = &r.id;
                    return true;
                }
            return false;
        };
        if (!select(c.inputId, true, config.capture.pDeviceID) ||
            !select(c.outputId, false, config.playback.pDeviceID))
            return Status::invalid_argument;
        config.dataCallback = &State::render;
        config.notificationCallback = &State::notify;
        config.pUserData = next.get();
        if (ma_device_init(&context.state_->context, &config, &next->device) != MA_SUCCESS)
            return Status::device_unavailable;
        next->ready = true;
        // The device is not started yet: the adapter is prepared with the client channel
        // counts miniaudio settled on (the native ones for an active-channel selection).
        const std::size_t nativeInputs = c.inputChannels ? next->device.capture.channels : 0;
        const std::size_t nativeOutputs = c.outputChannels ? next->device.playback.channels : 0;
        auto reaches=[](const auto& active,std::size_t native,std::size_t internal) {
            return active.empty()||(native==internal&&*std::max_element(active.begin(),active.end())<native);
        };
        if(!reaches(c.activeInputChannels,nativeInputs,next->device.capture.internalChannels)||
           !reaches(c.activeOutputChannels,nativeOutputs,next->device.playback.internalChannels)||
           nativeInputs>256||nativeOutputs>256)return Status::channel_mismatch;
        const auto prepared = next->adapter.prepare(c, callback, extended, user, nativeInputs, nativeOutputs);
        if (prepared != Status::ok) return prepared;
        if(c.inputChannels&&next->device.capture.shareMode!=config.capture.shareMode)return Status::unsupported_backend;
        if(c.outputChannels&&next->device.playback.shareMode!=config.playback.shareMode)return Status::unsupported_backend;
        state_.swap(next);
        return Status::ok;
    } catch (const std::bad_alloc&) {
        return Status::allocation_failed;
    } catch (...) {
        return Status::io_error;
    }
}
Status MiniaudioStream::description(AudioStreamDescription& output)const noexcept {
    if(!state_)return Status::not_prepared;
    try {
        const auto& s=*state_;AudioStreamDescription next;
        next.backend=publicBackend(s.context->context.backend);next.access=s.config.access;
        next.sampleRate=s.device.sampleRate;next.generation=s.config.generation;
        auto fill=[](auto& target,const auto& native,std::size_t channels) {
            target.channels=channels;target.nativeChannels=native.internalChannels;if(!channels)return;
            target.nativeSampleRate=native.internalSampleRate;
            target.periodFrames=native.internalPeriodSizeInFrames;target.periods=native.internalPeriods;
            target.periodKnown=target.periodFrames!=0;
            for(ma_uint32 i=0;i<native.channels;++i)target.channelMap.push_back(unsigned(native.channelMap[i]));
            // A buffer length is not a measured ADC/DAC latency.
        };
        fill(next.input,s.device.capture,s.config.inputChannels);fill(next.output,s.device.playback,s.config.outputChannels);
        next.input.activeChannels=s.config.activeInputChannels;next.output.activeChannels=s.config.activeOutputChannels;
        output=std::move(next);return Status::ok;
    }catch(...){return Status::allocation_failed;}
}
AudioCallbackDiagnostics MiniaudioStream::diagnostics()const noexcept {
    if(!state_)return {};
    return state_->adapter.diagnostics();
}
Status MiniaudioStream::start() noexcept {
    return !state_                                          ? Status::not_prepared
           : ma_device_start(&state_->device) == MA_SUCCESS ? Status::ok
                                                            : Status::io_error;
}
Status MiniaudioStream::stop() noexcept {
    return !state_                                         ? Status::not_prepared
           : ma_device_stop(&state_->device) == MA_SUCCESS ? Status::ok
                                                           : Status::io_error;
}
void MiniaudioStream::close() noexcept {
    state_.reset();
}
bool MiniaudioStream::running() const noexcept {
    return state_ && ma_device_is_started(&state_->device);
}
std::uint32_t MiniaudioStream::sampleRate() const noexcept {
    return state_ ? state_->device.sampleRate : 0;
}
std::uint32_t MiniaudioStream::callbackErrors() const noexcept {
    return state_ ? state_->adapter.diagnostics().callbackErrors : 0;
}
std::uint32_t MiniaudioStream::takeNotifications() noexcept {
    return state_ ? state_->notifications.exchange(0, std::memory_order_relaxed) : 0;
}
Status MiniaudioContext::open(const AudioDeviceConfig& c, AudioDeviceCallback cb, AudioDeviceContextCallback ext,
    void* user, std::unique_ptr<AudioProviderStream>& out) noexcept {
    try {
        auto stream = std::make_unique<MiniaudioStream>();
        auto r = stream->openImpl(*this, c, cb, ext, user);
        if (r == Status::ok) out = std::move(stream);
        return r;
    } catch (...) { return Status::allocation_failed; }
}
Status createMiniaudioContext(const AudioContextOptions& o, std::shared_ptr<AudioProviderContext>& out) noexcept {
    try {
        auto context = std::make_shared<MiniaudioContext>();
        auto r = context->prepare(o);
        if (r == Status::ok) out = std::move(context);
        return r;
    } catch (...) { return Status::allocation_failed; }
}
} // namespace cyaudio::detail
