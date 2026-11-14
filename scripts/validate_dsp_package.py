"""Explicit installed/relocated Core + expanded DSP consumer, without audio devices."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from reporting import dependency_fingerprint, source_digest

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build-dir', type=Path, required=True)
p.add_argument('--cydsp-source', type=Path, default=ROOT.parent/'CyDSP')
p.add_argument('--require-current-dynamics', action='store_true', help='Verify the newer Dynamics::minimumAppliedGain installed symbol')
p.add_argument('--require-prepared-optimizations', action='store_true', help='Verify the b973c1b prepared/mixed DSP APIs and explicit quality defaults')
p.add_argument('--cmake', default=r'C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe')
a = p.parse_args()
build = a.build_dir.resolve(strict=True)
cache = (build/'CMakeCache.txt').read_text(encoding='utf-8')
portaudio = 'CYAUDIOCORE_WITH_PORTAUDIO:BOOL=ON' in cache
tag = 'on' if portaudio else 'off'
(ROOT/'build').mkdir(exist_ok=True)
folder = Path(tempfile.mkdtemp(prefix='dsp-package-'+tag+'-', dir=ROOT/'build'))
source = folder/'consumer'
source.mkdir()
report = {'status': 'running', 'sourceSha256': source_digest(), 'dependency': dependency_fingerprint(a.cydsp_source),
          'currentDynamicsRequired': a.require_current_dynamics or a.require_prepared_optimizations,
          'preparedOptimizationsRequired': a.require_prepared_optimizations,
          'portAudio': portaudio, 'physicalDevices': False, 'folder': str(folder)}
environment = {key.upper(): value for key, value in os.environ.items()}


def run(name, args):
    result = subprocess.run(args, cwd=ROOT, env=environment, capture_output=True, text=True, timeout=240)
    (folder/(name+'.log')).write_text(result.stdout+result.stderr, encoding='utf-8', newline='\n')
    if result.returncode:
        raise RuntimeError(name+' failed: '+(result.stdout+result.stderr)[-4000:])
    print('PASS '+name, flush=True)


(source/'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.24)
project(InstalledDspConsumer LANGUAGES CXX)
find_package(CyAudioCore 0.1 CONFIG REQUIRED COMPONENTS Devices Files Sources Plugins)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE CyAudioCore::CyAudioCore)
''', encoding='utf-8', newline='\n')
if a.require_current_dynamics or a.require_prepared_optimizations:
    with (source/'CMakeLists.txt').open('a', encoding='utf-8', newline='\n') as generated:
        generated.write('target_compile_definitions(consumer PRIVATE EXPECT_CURRENT_DYNAMICS=1)\n')
if a.require_prepared_optimizations:
    with (source/'CMakeLists.txt').open('a', encoding='utf-8', newline='\n') as generated:
        generated.write('target_compile_definitions(consumer PRIVATE EXPECT_PREPARED_OPTIMIZATIONS=1)\n')
(source/'main.cpp').write_text('''#include <cyaudio/CyAudioCore.h>
#include <cydsp/control/Quality.h>
#include <cydsp/synthesis/Granulator.h>
#include <cydsp/synthesis/SourceBatch.h>
#include <cydsp/analysis/LevelMeter.h>
#include <cydsp/processors/Dynamics.h>
#if defined(EXPECT_PREPARED_OPTIMIZATIONS)
#include <cydsp/processors/AdvancedDynamics.h>
#include <cydsp/processors/Biquad.h>
#include <cydsp/spatial/SpatialMath.h>
#endif
#include <cmath>
#include <type_traits>
static_assert(std::is_same_v<cyaudio::AudioBlock<float>,cydsp::AudioBlock<float>>);
static_assert(std::is_same_v<cyaudio::Status,cydsp::Status>);
// Core requires the CyDSP 0.3 package; an older installed header fails here.
static_assert(CYDSP_VERSION_MAJOR > 0 || CYDSP_VERSION_MINOR >= 3);
int main() {
    using cydsp::Status;
    cyaudio::AudioBuffer<float> input, output;
    if (input.resize(1,8)!=Status::ok || output.resize(1,8)!=Status::ok) return 1;
    for (auto& value : input.block().channel(0)) value=.25f;
    cyaudio::MemoryAudioSource<float> source;
    if (source.prepare(input.block(),48000)!=Status::ok) return 2;
    std::size_t read=0;
    if (source.readAt(0,output.block(),read)!=Status::ok || read!=8) return 3;
    cyaudio::AudioTransport<float> transport;
    if (transport.prepare({48000,8,1},source)!=Status::ok ||
        transport.enqueue({cyaudio::TransportAction::play})!=Status::ok ||
        transport.process(output.block())!=Status::ok) return 15;
    for (auto value : output.block().channel(0)) if (value!=.25f) return 16;
    if (transport.enqueue({cyaudio::TransportAction::pause})!=Status::ok ||
        transport.process(output.block())!=Status::ok) return 17;
    for (auto value : output.block().channel(0)) if (value!=0) return 18;
    if (transport.enqueue({cyaudio::TransportAction::seek,6})!=Status::ok ||
        transport.enqueue({cyaudio::TransportAction::play})!=Status::ok ||
        transport.process(output.block())!=Status::ok) return 19;
    for (std::size_t i=0;i<8;++i)
        if (output.block().channel(0)[i]!=(i<2?.25f:0.f)) return 20;
#if defined(EXPECT_CURRENT_DYNAMICS)
    cydsp::Dynamics<float> dynamics;
    cydsp::DynamicsParameters parameters;
    parameters.thresholdDb=0; parameters.kneeDb=0;
    parameters.attackMs=0; parameters.releaseMs=0;
    if (dynamics.prepare({48000,8,1})!=Status::ok ||
        dynamics.setParameters(parameters)!=Status::ok ||
        dynamics.process(input.block(),output.block())!=Status::ok ||
        dynamics.minimumAppliedGain()!=1) return 21;
    for (auto value : output.block().channel(0)) if (value!=.25f) return 22;
#endif
#if defined(EXPECT_PREPARED_OPTIMIZATIONS)
    // Exercise the installed symbols through Core's public block aliases.
    // With unity wet dynamics, dry=.5 and wet=.25 must produce .1875.
    if (dynamics.processMixed(input.block(),output.block(),.5,.25)!=Status::ok ||
        dynamics.minimumAppliedGain()!=1) return 23;
    for (auto value : output.block().channel(0)) if (value!=.1875f) return 24;
    cydsp::QualitySettings policy;
    cydsp::SaturationParameters saturation;
    cydsp::TransientParameters transient;
    if (!saturation.antialias || saturation.math!=cydsp::MathAccuracy::precise ||
        transient.math!=cydsp::MathAccuracy::precise) return 25;
    for (auto profile : {cydsp::QualityProfile::realtimeHigh,cydsp::QualityProfile::offline}) {
        if (cydsp::qualitySettings(profile,48000,policy)!=Status::ok ||
            !policy.saturationAntialias || policy.saturationMath!=cydsp::MathAccuracy::precise ||
            policy.transientMath!=cydsp::MathAccuracy::precise) return 26;
    }
    if (cydsp::qualitySettings(cydsp::QualityProfile::economy,48000,policy)!=Status::ok ||
        policy.saturationAntialias || policy.saturationMath!=cydsp::MathAccuracy::fast ||
        policy.transientMath!=cydsp::MathAccuracy::fast) return 27;
    cydsp::TransientShaper<float> detector;
    if (detector.prepare({48000,8,1})!=Status::ok ||
        detector.processGain(input.block(),output.block())!=Status::ok) return 28;
    for (auto value : output.block().channel(0)) if (value!=1.f) return 29;
    cydsp::AdaaSaturator<float> mixed, reference;
    cyaudio::AudioBuffer<float> wet;
    if (wet.resize(1,8)!=Status::ok || mixed.prepare({48000,8,1})!=Status::ok ||
        reference.prepare({48000,8,1})!=Status::ok ||
        reference.process(input.block(),wet.block())!=Status::ok ||
        mixed.processMixed(input.block(),output.block(),{},.5,.25)!=Status::ok) return 30;
    for (std::size_t i=0;i<8;++i)
        if (!std::isfinite(output.block().channel(0)[i]) ||
            std::abs(output.block().channel(0)[i]-(.125f+.25f*wet.block().channel(0)[i]))>1e-6f) return 31;
    cydsp::PeakingBiquadDesigner bell;
    cydsp::BiquadCoefficients coefficients;
    if (bell.prepare(48000,1000,.707)!=Status::ok ||
        bell.designFor<float>(0,coefficients)!=Status::ok || !coefficients.isStable() ||
        coefficients.b0!=1 || coefficients.b1!=coefficients.a1 ||
        coefficients.b2!=coefficients.a2) return 32;
    cydsp::DopplerPlan doppler;
    double ratio=0;
    if (doppler.prepare()!=Status::ok || doppler.ratio(0,0,ratio)!=Status::ok || ratio!=1) return 33;
#endif
    cydsp::DspBus bus{1,1};
    cydsp::DspConnection route{cydsp::graphInputNode,1,cydsp::graphOutputNode,1,1};
    cydsp::MultiBusGraph<float> graph;
    if (graph.prepare({48000,8,1},{&bus,1},{&bus,1},{},{&route,1})!=Status::ok) return 4;
    cydsp::QualityGraph<float> quality;
    if (quality.prepare({48000,8,1},{&bus,1},{&bus,1},0)!=Status::ok ||
        quality.setInitial(cydsp::QualityProfile::economy,std::move(graph))!=Status::ok) return 5;
    cydsp::AudioBlock<const float> in=input.block(); auto out=output.block();
    if (quality.process({&in,1},{&out,1})!=Status::ok || out.channel(0)[0]!=.25f) return 6;
    cydsp::LevelMeter<float> meter;
    if (meter.prepare({48000,8,1},8)!=Status::ok || meter.process(out)!=Status::ok || meter.levels(0).peak!=.25) return 7;
    cydsp::SampleData<float> sample;
    cydsp::Granulator<float> grains;
    if (sample.prepare(input.block(),48000)!=Status::ok || grains.prepare({48000,8,1},sample,2)!=Status::ok ||
        grains.trigger({})!=Status::ok || grains.process(out)!=Status::ok) return 8;
    // CyDSP 0.3 block forms used by the Core synthesizer, linked from the installed archive.
    cydsp::Adsr envelope; float curve[8];
    if (envelope.prepare(48000)!=Status::ok || envelope.setParameters({0,0,1,0})!=Status::ok) return 34;
    envelope.noteOn(); envelope.process(std::span<float>(curve));
    for (auto value : curve) if (value!=1.f) return 35;
    double position=0; std::size_t inside=0;
    if (sample.render(position,1,cydsp::SampleInterpolation::linear,out,inside)!=Status::ok ||
        inside!=8 || position!=8 || out.channel(0)[7]!=.25f) return 36;
    float cycle[16]; for (auto& value : cycle) value=.25f;
    cydsp::Wavetable<float> table;
    if (table.prepare(cycle)!=Status::ok) return 9;
    const cydsp::Wavetable<float>* tables[]={&table};
    cydsp::SourceBatch<float> batch;
    if (batch.prepare({48000,8,1},1,tables)!=Status::ok) return 10;
    cydsp::BatchSource voice; voice.kind=cydsp::BatchSourceKind::wavetable;
    if (batch.start(0,voice)!=Status::ok || batch.process(out)!=Status::ok || out.channel(0)[0]!=.25f) return 11;
    const float impulse[]={1,0};
    cydsp::HrirPoint<float> point{{1,0,0},impulse,impulse};
    cydsp::HrtfSet<float> bank;
    cydsp::HrtfPreparation settings; settings.detectOnsets=false; settings.maximumTaps=16;
    if (bank.prepare({&point,1},settings)!=Status::ok) return 12;
    cydsp::HrirBinaural<float> binaural;
    if (binaural.prepare({48000,8,1},bank)!=Status::ok) return 13;
    cyaudio::AudioBuffer<float> stereo;
    if (stereo.resize(2,8)!=Status::ok || binaural.process(input.block(),stereo.block())!=Status::ok) return 14;
    // The CLAP facade/discovery symbols and public headers must survive install/relocation.
    cyaudio::ClapPresetDiscovery discovery;
    if (discovery.catalogue()!=nullptr) return 37;
    std::unique_ptr<cyaudio::PluginProvider> clap;
    if (cyaudio::createPluginProvider(clap)!=Status::invalid_argument || clap) return 38;
    cyaudio::PluginIsolationOptions isolated;
    isolated.workerExecutable=std::filesystem::absolute("cyaudio-worker");
    if (cyaudio::createPluginProvider(clap,isolated)!=Status::ok || !clap) return 39;
    cyaudio::PluginInstance emptyPlugin;
    cyaudio::PluginProcess<float> emptyProcess;
    std::uint64_t ticket=0;
    if (emptyPlugin.isolated() || emptyPlugin.submit(emptyProcess,ticket)!=Status::not_prepared ||
        emptyPlugin.collect(emptyProcess,ticket)!=Status::not_prepared || emptyPlugin.discard(ticket)!=Status::not_prepared) return 40;
    if (emptyPlugin.clapExtensions()!=nullptr) return 41;
    return 0;
}
''', encoding='utf-8', newline='\n')
try:
    prefix, relocated = folder/'installed', folder/'relocated'
    run('install', [a.cmake, '--install', str(build), '--config', 'Release', '--prefix', str(prefix)])
    shutil.copytree(prefix, relocated)
    # Ignore the original install prefix and the dependency checkout. Verify the
    # resolved package directories below, rather than trusting the search order.
    run('configure', [a.cmake, '-S', str(source), '-B', str(folder/'consumer-build'), '-G', 'Visual Studio 17 2022', '-A', 'x64',
        '-DCMAKE_PREFIX_PATH='+str(relocated), '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF',
        '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF', '-DCMAKE_IGNORE_PREFIX_PATH='+str(prefix)+';'+str(ROOT.parent/'CyDSP')])
    consumer_cache = (folder/'consumer-build/CMakeCache.txt').read_text(encoding='utf-8')
    for package in ('CyAudioCore', 'CyDSP'):
        entries = [line.split('=',1)[1] for line in consumer_cache.splitlines() if line.startswith(package+'_DIR:')]
        assert len(entries)==1 and Path(entries[0]).resolve().is_relative_to(relocated.resolve()), 'Non-relocated package: '+package
    run('build', [a.cmake, '--build', str(folder/'consumer-build'), '--config', 'Release'])
    run('run', [str(folder/'consumer-build/Release/consumer.exe')])
    assert not list((relocated/'include').rglob('portaudio.h')), 'Provider headers leaked'
    assert not (relocated/'include/clap').exists(), 'CLAP SDK leaked into public include tree'
    report['inputsUnchanged'] = report['sourceSha256']==source_digest() and report['dependency']==dependency_fingerprint(a.cydsp_source)
    assert report['inputsUnchanged'], 'Compiler inputs changed during package validation'
    report['status'] = 'passed'
except Exception as error:
    report['status'], report['error'] = 'failed', str(error)
finally:
    (ROOT/'build'/('dsp-package-'+tag+'.json')).write_text(json.dumps(report,indent=2)+'\n', encoding='utf-8', newline='\n')
if report['status']!='passed':
    raise SystemExit(report['error'])
print('PASS installed/relocated Core + expanded DSP consumer, PortAudio '+tag)
