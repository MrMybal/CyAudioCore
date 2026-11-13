"""Explicit Windows-only isolated path measurements; never opens an audio device."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import statistics
import subprocess
import time
import shutil
import zipfile
from reporting import source_digest

ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cydsp-source',type=Path,default=ROOT.parent/'CyDSP')
parser.add_argument('--dependency-commit',default='fda6bd30c5b622795cc08e8bd2750188a6c5a0c0')
parser.add_argument('--daw-source',type=Path)
parser.add_argument('--trials',type=int,default=5)
parser.add_argument('--run',action='store_true',help='Explicitly execute measurements after building')
parser.add_argument('--cmake',default='C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe')
a=parser.parse_args()
if a.trials<3: parser.error('at least three fresh-process trials required')
folder=ROOT/'build/core-path-profile'
folder.mkdir(parents=True,exist_ok=True)
environment={key.upper():value for key,value in os.environ.items()}

def git(root,*args):
    return subprocess.check_output(['git','-c','safe.directory='+root.as_posix(),'-C',str(root),*args])

def run(name,args):
    result=subprocess.run([str(x) for x in args],cwd=ROOT,env=environment,capture_output=True,text=True,timeout=300)
    (folder/(name+'.log')).write_text(result.stdout+result.stderr,encoding='utf-8',newline='\n')
    if result.returncode: raise RuntimeError(name+': '+(result.stdout+result.stderr)[-6000:])
    print('PASS '+name,flush=True)
    return result.stdout

original=a.cydsp_source.resolve(strict=True)
commit=git(original,'rev-parse',a.dependency_commit+'^{commit}').decode().strip()
snapshot=folder/('cydsp-'+commit[:12])
if not snapshot.exists():
    archive=git(original,'archive','--format=zip',commit)
    with zipfile.ZipFile(io.BytesIO(archive)) as data:
        for name in data.namelist():
            if not (snapshot/name).resolve().is_relative_to(snapshot.resolve()): raise ValueError('Archive path escape')
        snapshot.mkdir()
        data.extractall(snapshot)
    (snapshot/'.snapshot.json').write_text(json.dumps({'commit':commit,'sourceSha256':source_digest(snapshot)}),encoding='utf-8')
marker=json.loads((snapshot/'.snapshot.json').read_text())
assert marker['commit']==commit and marker['sourceSha256']==source_digest(snapshot),'Frozen dependency changed'
core_hash=source_digest()
probe_headers={}
if a.daw_source:
    daw=a.daw_source.resolve(strict=True)
    for relative in ('include/cydaw/audio/SampleBuffer.h','include/cydaw/base/Geometry.h','include/cydaw/base/Text.h'):
        probe_headers[relative]=hashlib.sha256((daw/relative).read_bytes()).hexdigest()

native=folder/'native'
run('configure',[a.cmake,'-S',ROOT,'-B',native,'-G','Visual Studio 17 2022','-A','x64',
    '-DCYAUDIOCORE_CYDSP_SOURCE_DIR='+str(snapshot),'-DCYAUDIOCORE_BUILD_PATH_MEASUREMENTS=ON',
    '-DCYAUDIOCORE_BUILD_TESTS=ON','-DCYAUDIOCORE_COMPILE_CHECKS=OFF','-DCYAUDIOCORE_WITH_PORTAUDIO=OFF',
    '-DCYDSP_BUILD_TESTS=OFF','-DCYDSP_COMPILE_CHECKS=OFF','-DCYDSP_BUILD_MEASUREMENTS=OFF'])
run('build',[a.cmake,'--build',native,'--config','Release','--target','cyaudio_path_measurements',
    'cyaudio_worker_measurements','cydsp_vst_fixture','cydsp_clap_fixture','cydsp_plugin_worker_fixture','--parallel','4'])
if a.daw_source:
    source=folder/'probe-source'; source.mkdir(exist_ok=True)
    # Separate consumer only: no application dependency is added to Core targets.
    (source/'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.24)
project(ReadOnlyBufferProbe LANGUAGES CXX)
add_executable(daw_buffer_probe "${CORE_SOURCE}/tests/DawBufferMeasurements.cpp")
target_compile_features(daw_buffer_probe PRIVATE cxx_std_20)
target_include_directories(daw_buffer_probe PRIVATE "${DAW_SOURCE}/include" "${DSP_SOURCE}/include")
target_link_libraries(daw_buffer_probe PRIVATE "${DSP_ARCHIVE}")
target_compile_options(daw_buffer_probe PRIVATE /O2 /fp:precise /utf-8)
''',encoding='utf-8',newline='\n')
    run('probe-configure',[a.cmake,'-S',source,'-B',folder/'probe-build','-G','Visual Studio 17 2022','-A','x64',
        '-DCORE_SOURCE:PATH='+ROOT.as_posix(),'-DDAW_SOURCE:PATH='+daw.as_posix(),'-DDSP_SOURCE:PATH='+snapshot.as_posix(),
        '-DDSP_ARCHIVE:FILEPATH='+(native/'cydsp/Release/CyDSP.lib').as_posix()])
    run('probe-build',[a.cmake,'--build',folder/'probe-build','--config','Release'])

def unchanged():
    assert core_hash==source_digest() and marker['sourceSha256']==source_digest(snapshot),'Compiler inputs changed'
    if a.daw_source:
        assert all(hashlib.sha256((daw/name).read_bytes()).hexdigest()==sha for name,sha in probe_headers.items()),'Application probe headers changed'

unchanged()
metadata={'coreCommit':git(ROOT,'rev-parse','HEAD').decode().strip(),'coreCompilerInputs':core_hash,
    'dependencyCommit':commit,'dependencyCompilerInputs':marker['sourceSha256'],'dawProbeHeaders':probe_headers,
    'physicalDevices':False,'platform':'Windows x64','compiler':'MSVC 19.40 Release',
    'coreProbeFp':'strict','applicationHeaderProbeFp':'precise','sampleRate':48000,
    'timingMeaning':'median of 31 batch means, 32 calls/batch; projected CPU is a normalization, not measured whole-DAW CPU',
    'productionSourcesChanged':False,
    'driverScriptSha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    'backgroundAcceptance':'No compiler/linker at boundaries; unchanged CyDAW process set; less than 1 percent of one core observed in CyDAW over trial'}
if a.daw_source:
    metadata['dawCommit']=git(daw,'rev-parse','HEAD').decode().strip()
(folder/'inputs.json').write_text(json.dumps(metadata,indent=2)+'\n',encoding='utf-8',newline='\n')
if not a.run:
    print('Built only; add --run to explicitly collect measurements')
    raise SystemExit(0)

programs=[('core',native/'Release/cyaudio_path_measurements.exe',[native/'Release']),
          ('workers',native/'Release/cyaudio_worker_measurements.exe',[native/'Release'])]
if a.daw_source: programs.append(('daw-buffer',folder/'probe-build/Release/daw_buffer_probe.exe',[]))
trials=[]
def background():
    command="@(Get-Process -Name 'CyDAW 2','cl','link','cyaudio_path_measurements','cyaudio_worker_measurements','daw_buffer_probe' -ErrorAction SilentlyContinue | Select-Object ProcessName,Id,CPU) | ConvertTo-Json -Compress"
    result=subprocess.run(['powershell','-NoProfile','-Command',command],capture_output=True,text=True,timeout=15)
    text=result.stdout.strip()
    value=json.loads(text) if text else []
    return value if isinstance(value,list) else [value]

background_samples=[]
if (folder/'results.json').exists():
    previous=folder/('previous-'+str(time.time_ns())); previous.mkdir()
    for path in [folder/'results.json',folder/'inputs.json',*folder.glob('attempt-*.log')]:
        if path.exists(): shutil.copy2(path,previous/path.name)
attempt=0
while len(trials)<a.trials:
    attempt+=1
    if attempt>a.trials*4: raise RuntimeError('Insufficient trials meeting background criteria; rejected evidence retained in logs')
    trial=len(trials)
    observed=background()
    deadline=time.monotonic()+180
    while any(p['ProcessName'] in ('cl','link') for p in observed):
        if time.monotonic()>deadline: raise RuntimeError('Other compiler activity persists; measurements not started')
        time.sleep(5)
        observed=background()
    background_samples.append({'attempt':attempt,'before':observed})
    trial_begin=time.monotonic()
    unchanged()
    pair={}
    order=programs if trial%2==0 else list(reversed(programs))
    for name,exe,args in order:
        output=run(f'attempt-{attempt}-{name}',[exe,*args,*(['reverse'] if name=='daw-buffer' and trial%2 else [])])
        rows=[json.loads(line) for line in output.splitlines() if line.startswith('{')]
        assert rows and all(row.get('medianBatchMeanNs',row.get('parentCorePercent',0))>=0 for row in rows)
        pair[name]=rows
    unchanged()
    background_samples[-1]['after']=background()
    after=background_samples[-1]['after']
    before_daw={p['Id']:p['CPU'] for p in observed if p['ProcessName']=='CyDAW 2'}
    after_daw={p['Id']:p['CPU'] for p in after if p['ProcessName']=='CyDAW 2'}
    daw_cpu=sum(max(0,after_daw[pid]-value) for pid,value in before_daw.items() if pid in after_daw)*100/(time.monotonic()-trial_begin)
    background_samples[-1]['dawCorePercent']=daw_cpu
    if (any(p['ProcessName'] in ('cl','link') for p in after) or before_daw.keys()!=after_daw.keys() or daw_cpu>=1):
        background_samples[-1]['accepted']=False
        print('REJECT compiler/linker or changing/active application at trial boundary',flush=True)
        continue
    background_samples[-1]['accepted']=True
    trials.append(pair)
rows=[]
for program in trials[0]:
    for index,first in enumerate(trials[0][program]):
        # The application probe alternates backend order, so match by dimensions.
        samples=[next(row for row in trial[program] if all(row.get(k)==first.get(k) for k in ('case','frames','instances','backend'))) for trial in trials]
        row={k:v for k,v in first.items() if k in ('case','frames','instances','backend')}
        for field in ('medianBatchMeanNs','projectedCorePercent48000','parentCorePercent','workersCorePercent','medianCallNs','p95CallNs',
                      'minimumDriverPeriodNs','maximumDriverPeriodNs','maximumDriverLatenessNs'):
            if field in first:
                values=[sample[field] for sample in samples]
                row[field]=statistics.median(values); row[field+'Trials']=values
        if 'signature' in first:
            assert all(sample['signature']==first['signature'] for sample in samples),'Fresh-process output signatures differ'
            row['signature']=first['signature']
        if 'missed' in first: row['missedTrials']=[sample['missed'] for sample in samples]
        rows.append(row)
report={**metadata,'trials':a.trials,'rows':rows,
        'backgroundSamples':background_samples,
        'executables':{name:hashlib.sha256(exe.read_bytes()).hexdigest() for name,exe,_ in programs}}
(folder/'results.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8',newline='\n')
print('PASS',len(rows),'measured cases,',a.trials,'fresh-process trials')
