// Optional standalone consumer probe. Only compiled by the explicit profiling
// script, with an application checkout supplied in read-only include paths.
// Never linked into CyAudioCore or installed with the library.
#include <cydaw/audio/SampleBuffer.h>
#include "PathMeasurement.h"

namespace {
using namespace path_measurement;
using cydaw::audio::SampleBuffer;
void run(std::size_t frames, bool automaticFirst) {
    SampleBuffer<float> input(2,int(frames)),output(2,int(frames));
    for (std::size_t ch=0; ch<2; ++ch) for (std::size_t i=0; i<frames; ++i)
        input.setSample(int(ch),int(i),float(.125*std::sin(double(i+7*ch)*.075)));
    cydsp::VectorOps ops; require(cydsp::VectorOps::create(cydsp::CpuBackend::automatic,ops)==cydsp::Status::ok);
    const auto source=std::span<const float>(input.getReadPointer(0),frames);
    float scalarDot=0,autoDot=0; cydsp::VectorOps scalar;
    require(scalar.backend()==cydsp::CpuBackend::scalar);
    require(scalar.dot(source,source,scalarDot)==cydsp::Status::ok && ops.dot(source,source,autoDot)==cydsp::Status::ok);
    require(std::abs(double(scalarDot)-autoDot)<1e-5*std::max(1.f,std::abs(scalarDot)));
    const auto signature=[&] { double result=0; for (int ch=0; ch<2; ++ch) for (std::size_t i=0; i<frames; ++i) { const auto v=output.getSample(ch,int(i)); require(std::isfinite(v)); result+=v; } return result; };
    for (bool automatic : automaticFirst ? std::array{true,false} : std::array{false,true}) {
        const auto backend=automatic ? cydsp::backendName(ops.backend()) : "application_default";
        bench("daw_header_mix32",frames,32,backend,[&] {
            output.clear();
            for (unsigned track=0; track<32; ++track) for (int ch=0; ch<2; ++ch) {
                if (automatic) require(ops.addScaled({input.getReadPointer(ch),frames},{output.getWritePointer(ch),frames},.03125f)==cydsp::Status::ok);
                else output.addFrom(ch,0,input,ch,0,int(frames),.03125f);
            }
        },signature);
        for (int ch=0; ch<2; ++ch) for (std::size_t i=0; i<frames; ++i)
            require(std::abs(output.getSample(ch,int(i))-input.getSample(ch,int(i)))<5e-7);
        bench("daw_header_rms32",frames,32,backend,[&] {
            double sum=0;
            for (unsigned track=0; track<32; ++track) for (int ch=0; ch<2; ++ch) {
                if (automatic) { float value=0; const auto data=std::span<const float>(input.getReadPointer(ch),frames);
                    require(ops.dot(data,data,value)==cydsp::Status::ok); sum+=std::sqrt(value/frames);
                } else sum+=input.getRMSLevel(ch,0,int(frames));
            }
            sink=sum;
        },[&] { return double(sink); });
        bench("daw_header_constant_gain32",frames,32,backend,[&] {
            for (unsigned track=0; track<32; ++track) for (int ch=0; ch<2; ++ch) {
                if (automatic) {
                    require(ops.copy({input.getReadPointer(ch),frames},{output.getWritePointer(ch),frames})==cydsp::Status::ok);
                    const auto data=std::span<float>(output.getWritePointer(ch),frames);
                    require(ops.scale(data,data,.75f)==cydsp::Status::ok);
                } else {
                    output.copyFrom(ch,0,input,ch,0,int(frames));
                    output.applyGainRamp(ch,0,int(frames),.75f,.75f);
                }
            }
        },signature);
        for (int ch=0; ch<2; ++ch) for (std::size_t i=0; i<frames; ++i)
            require(output.getSample(ch,int(i))==input.getSample(ch,int(i))*.75f);
    }
}
}
int main(int argc,char**) {
    try { for (const auto frames : {128u,1024u}) run(frames,argc>1); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
