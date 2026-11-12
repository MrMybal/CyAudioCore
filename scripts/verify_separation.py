"""Read-only public-boundary and split provenance checks."""
import argparse,hashlib,json,re
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cydsp-source',type=Path,default=ROOT.parent/'CyDSP');a=p.parse_args()
dsp=a.cydsp_source.resolve(strict=True);errors=[]
for path in [*ROOT.joinpath('include').rglob('*.h'),*ROOT.joinpath('src').rglob('*.h'),*ROOT.joinpath('src').rglob('*.cpp'),*ROOT.joinpath('examples').rglob('*.cpp')]:
 for relative in re.findall(r'^#include ["<](cydsp/[^">]+)[">]',path.read_text(encoding='utf-8'),re.M):
  if not (dsp/'include'/relative).is_file():errors.append('Missing public DSP include '+relative+' in '+str(path))
for folder in ['include','src']:
 for path in (dsp/folder).rglob('*'):
  if path.suffix in ['.h','.cpp'] and re.search(r'cyaudio|CyAudioCore|cydsp/(io|midi|plugins|sources)/',path.read_text(encoding='utf-8')):errors.append('Reverse dependency/service in DSP '+str(path))
for area in ['io','midi','plugins','sources']:
 if list((dsp/'include/cydsp'/area).glob('*.h')) or list((dsp/'src'/area).glob('*.cpp')):errors.append('Service implementation retained in DSP: '+area)
left=json.loads((dsp/'third_party/manifest.json').read_text(encoding='utf-8'))['components'];right=json.loads((ROOT/'third_party/manifest.json').read_text(encoding='utf-8'))['components']
if set(c['name'] for c in left)&set(c['name'] for c in right):errors.append('Duplicated vendor ownership')
if len(left)!=5 or len(right)!=14:errors.append('Unexpected dependency partition')
if errors:raise SystemExit('\n'.join(errors))
print('PASS public-only CyAudioCore -> CyDSP; no reverse service dependency; 5/14 vendor partition')
