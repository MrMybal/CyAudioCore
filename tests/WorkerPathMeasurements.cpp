#include <cyaudio/CyAudioCore.h>
#include "PathMeasurement.h"
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
namespace {
using namespace cyaudio;
using namespace path_measurement;
double cpu(HANDLE process) {
    FILETIME creation{},exit{},kernel{},user{};
    require(GetProcessTimes(process,&creation,&exit,&kernel,&user)!=0);
    return double((std::uint64_t(kernel.dwHighDateTime)<<32)|kernel.dwLowDateTime)*1e-7+
           double((std::uint64_t(user.dwHighDateTime)<<32)|user.dwLowDateTime)*1e-7;
}
struct Children {
    std::vector<HANDLE> handles;
    Children() {
        const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
        require(snapshot!=INVALID_HANDLE_VALUE);
        PROCESSENTRY32W entry{}; entry.dwSize=sizeof(entry);
        for (bool more=Process32FirstW(snapshot,&entry)!=0; more; more=Process32NextW(snapshot,&entry)!=0)
            if (entry.th32ParentProcessID==GetCurrentProcessId()) {
                auto handle=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ProcessID);
                if (handle) handles.push_back(handle);
            }
        CloseHandle(snapshot);
    }
    ~Children() { for (auto handle : handles) CloseHandle(handle); }
    double seconds() const { double total=0; for (auto handle : handles) total+=cpu(handle); return total; }
};
void run(const std::filesystem::path& directory) {
    std::array<PluginWorker,8> workers;
    PluginWorkerOptions options; options.processing.maximumFrames=1024;
    for (auto& worker : workers)
        require(worker.launch(directory/"cydsp_plugin_worker_fixture.exe",PluginFormat::clap,
                             directory/"cydsp_fixture.clap","org.cydsp.fixture",options)==Status::ok);
    Children children; require(children.handles.size()==8);
    AudioBuffer<float> input,side,output; require(input.resize(2,1024)==Status::ok && side.resize(1,1024)==Status::ok && output.resize(2,1024)==Status::ok);
    for (std::size_t ch=0; ch<2; ++ch) std::fill(input.block().channel(ch).begin(),input.block().channel(ch).end(),.125f);
    const std::array<AudioBlock<const float>,2> ins{input.block(),side.block()};
    const std::array<AudioBlock<float>,1> outs{output.block()};
    PluginProcess<float> process{ins,outs,1024};
    for (unsigned mode=0; mode<3; ++mode) {
        std::array<double,256> costs{}; unsigned calls=0,misses=0,received=0,submitted=0;
        double minimumPeriod=1e30,maximumPeriod=0,maximumLateness=0;
        const auto parentBefore=cpu(GetCurrentProcess()),childrenBefore=children.seconds();
        const auto begin=std::chrono::steady_clock::now();
        const auto end=begin+std::chrono::seconds(2);
        if (mode==0) std::this_thread::sleep_until(end);
        else {
            auto next=begin,previous=begin;
            const auto period=std::chrono::nanoseconds(mode==1 ? 16666667 : 21333333);
            while (std::chrono::steady_clock::now()<end) {
                const auto start=std::chrono::steady_clock::now();
                if (calls) {
                    const auto interval=std::chrono::duration<double,std::nano>(start-previous).count();
                    minimumPeriod=std::min(minimumPeriod,interval); maximumPeriod=std::max(maximumPeriod,interval);
                }
                maximumLateness=std::max(maximumLateness,std::chrono::duration<double,std::nano>(start-next).count());
                previous=start;
                for (auto& worker : workers) {
                    if (mode==1) require(worker.poll()==Status::ok);
                    else {
                        PluginWorkerBlockResult result;
                        const auto status=worker.process(process,result);
                        require(status==Status::ok || status==Status::would_block);
                        misses+=result.missedDeadline; received+=result.received; submitted+=result.submitted;
                    }
                }
                require(calls<costs.size());
                costs[calls++]=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count();
                next+=period; std::this_thread::sleep_until(next); // Driver only; never inside process().
            }
        }
        const auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
        const auto parent=(cpu(GetCurrentProcess())-parentBefore)*100/seconds;
        const auto child=(children.seconds()-childrenBefore)*100/seconds;
        std::sort(costs.begin(),costs.begin()+calls);
        std::cout << std::setprecision(15) << "{\"case\":\"core_worker8_" << (mode==0 ? "idle" : mode==1 ? "poll60Hz" : "paced46Hz")
                  << "\",\"instances\":8,\"frames\":1024,\"seconds\":" << seconds << ",\"parentCorePercent\":" << parent
                  << ",\"workersCorePercent\":" << child << ",\"calls\":" << calls
                  << ",\"medianCallNs\":" << (calls ? costs[calls/2] : 0) << ",\"p95CallNs\":" << (calls ? costs[std::size_t(.95*(calls-1))] : 0)
                  << ",\"minimumDriverPeriodNs\":" << (calls>1 ? minimumPeriod : 0) << ",\"maximumDriverPeriodNs\":" << maximumPeriod
                  << ",\"maximumDriverLatenessNs\":" << maximumLateness
                  << ",\"missed\":" << misses << ",\"received\":" << received << ",\"submitted\":" << submitted << "}\n" << std::flush;
        if (mode==2) require(received>0);
    }
    for (auto& worker : workers) worker.close();
}
}
#endif
int main(int argc,char** argv) {
    try {
#if defined(_WIN32)
        path_measurement::require(argc==2); run(std::filesystem::absolute(argv[1])); return 0;
#else
        (void)argc; (void)argv; throw std::runtime_error("Worker CPU accounting probe requires Windows");
#endif
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
