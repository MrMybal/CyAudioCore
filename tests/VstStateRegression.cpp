#include "../src/plugins/PluginInternal.h"
#include <cyaudio/CyAudioCore.h>
#include "plugins/VstSupport.h"
#include <array>
#include <cstring>
#include <iostream>
#include <source_location>
#include <stdexcept>
namespace {
using namespace cyaudio;
void require(bool value, std::source_location at=std::source_location::current()) {
 if(!value)throw std::runtime_error("VST state line "+std::to_string(at.line()));
}
void streams() {
 using namespace cyaudio::detail::vst;
 const std::array<std::uint8_t,3> input{11,22,33};
 MemoryStream stream(input); std::array<std::uint8_t,4096> data{}; int32 actual=-1;
 require(stream.read(data.data(),4096,&actual)==kResultOk && actual==3 && data[2]==33);
 require(stream.read(data.data(),4096,&actual)==kResultFalse && actual==0);
 require(stream.read(nullptr,0,&actual)==kResultOk && actual==0);
 require(stream.read(nullptr,1,&actual)==kInvalidArgument && actual==0);
 require(stream.read(data.data(),-1,&actual)==kInvalidArgument && actual==0);
 require(stream.seek(0,IBStream::kIBSeekSet,nullptr)==kResultOk);
 require(stream.read(data.data(),2,nullptr)==kResultOk && stream.position==2);
 require(stream.read(data.data(),1,&actual)==kResultOk && actual==1 && data[0]==33);
 require(stream.seek(-1,IBStream::kIBSeekSet,nullptr)==kInvalidArgument && stream.position==3);
 require(stream.seek(1,IBStream::kIBSeekEnd,nullptr)==kInvalidArgument && stream.position==3);
 MemoryStream writer; writer.limit=2; std::array<std::uint8_t,3> payload{1,2,3};
 require(writer.write(payload.data(),3,&actual)==kInvalidArgument && writer.failed && actual==0 && writer.bytes.empty());
}
}
void vstStateRegression(const std::filesystem::path& directory) {
 streams();
 using namespace cyaudio;
 std::unique_ptr<PluginProvider> provider;
 require(cyaudio::detail::createInProcessPluginProvider(PluginFormat::vst3,provider)==Status::ok);
 const auto path=std::filesystem::absolute(directory/"cyaudio_state_fixture.vst3");
 std::vector<PluginDescriptor> descriptors; require(provider->scan(path,descriptors)==Status::ok && descriptors.size()==7);
 // Modes 0-4: controller getState variants. Mode 5: controller setComponentState answers
 // kNotImplemented (SDK default). Mode 6: component getState answers kNotImplemented.
 for(unsigned mode=0;mode<7;++mode) {
  const bool good=mode<2 || mode>=5;
  PluginInstance instance;require(provider->create(path,descriptors[mode].id,{},instance)==Status::ok);
  std::vector<std::uint8_t> state{0xa5};
  const auto status=instance.saveState(state,64);
  if(good) {
   require(status==Status::ok && state.size()==((mode==0 || mode==6)?32:40));
   require(instance.loadState(state)==Status::ok);
   const auto saved=state;
   require(instance.saveState(state,mode==6?24:31)==Status::block_too_large && state==saved);
   auto malformed=state;malformed[16]=255;
   require(instance.loadState(malformed)==Status::invalid_argument);
  } else require(status==(mode==4?Status::block_too_large:Status::io_error) && state==std::vector<std::uint8_t>{0xa5});
  require(instance.close()==Status::ok);
#if !defined(__ANDROID__)
  PluginWorker worker; PluginWorkerOptions options;options.processing.maximumFrames=64;options.maximumStateBytes=64;
  const auto executable=std::filesystem::absolute(directory/
#if defined(_WIN32)
    "cydsp_plugin_worker_fixture.exe"
#else
    "cydsp_plugin_worker_fixture"
#endif
  );
  require(worker.launch(executable,PluginFormat::vst3,path,descriptors[mode].id,options)==Status::ok);
  state={0xa5}; const auto remote=worker.saveState(state);
  if(good)require(remote==Status::ok && worker.loadState(state)==Status::ok);
  else require(remote==(mode==4?Status::block_too_large:Status::io_error) && state==std::vector<std::uint8_t>{0xa5});
  worker.close();
#endif
 }
 std::cout<<"PASS VST3 optional controller state/short reads/EOF/errors/capacity/direct/worker\n";
}
