"""Compare two explicitly built synthesis measurement executables, without audio devices."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--before', type=Path, required=True)
p.add_argument('--after', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--trials', type=int, default=6)
a = p.parse_args()
if a.trials < 2:
    p.error('at least two paired trials are required')
before = a.before.resolve(strict=True)
after = a.after.resolve(strict=True)


def execute(executable):
    result = subprocess.run([str(executable)], check=True, capture_output=True, text=True, timeout=120)
    cases = {}
    for line in result.stdout.splitlines():
        if not line.startswith('polyphonicSynth '):
            continue
        fields = dict(part.split('=', 1) for part in line.split()[1:])
        key = (fields['source'], fields['sampleType'], fields['backend'], int(fields['voices']), int(fields['frames']))
        for field in ('medianBatchMeanNs', 'p95BatchMeanNs', 'checksum'):
            fields[field] = float(fields[field])
            if not math.isfinite(fields[field]):
                raise ValueError('nonfinite measurement')
        if fields['medianBatchMeanNs'] <= 0 or key in cases:
            raise ValueError('invalid or duplicate measurement')
        cases[key] = fields
    if len(cases) != 24:
        raise ValueError('expected 24 synthesis cases')
    return cases


trials = []
for index in range(a.trials):
    order = [('before', before), ('after', after)] if index % 2 == 0 else [('after', after), ('before', before)]
    pair = {name: execute(path) for name, path in order}
    if pair['before'].keys() != pair['after'].keys():
        raise ValueError('case sets differ')
    if any(pair['before'][key]['checksum'] != pair['after'][key]['checksum'] for key in pair['before']):
        raise ValueError('before/after final left-channel samples differ')
    trials.append(pair)
    print(f'PASS paired trial {index + 1}/{a.trials}: matching final-sample sentinels', flush=True)

rows = []
for key in sorted(trials[0]['before']):
    old = [pair['before'][key]['medianBatchMeanNs'] for pair in trials]
    new = [pair['after'][key]['medianBatchMeanNs'] for pair in trials]
    old_median, new_median = statistics.median(old), statistics.median(new)
    rows.append(dict(zip(('source', 'sampleType', 'backend', 'voices', 'frames'), key),
        beforeMedianBatchMeanNs=old_median, afterMedianBatchMeanNs=new_median,
        speedup=old_median / new_median, beforeTrialsNs=old, afterTrialsNs=new))
report = {
    'trials': a.trials, 'alternatingExecutableOrder': True, 'matchingChecksums': True,
    'checksumMeaning': 'final sample of the last left-channel block; not a full-output hash',
    'beforeExeSha256': hashlib.sha256(before.read_bytes()).hexdigest(),
    'afterExeSha256': hashlib.sha256(after.read_bytes()).hexdigest(),
    'timingMeaning': 'median of trial medians of 31 batch means; 16 public calls per batch; not individual callback percentiles',
    'rows': rows,
}
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8', newline='\n')
for row in rows:
    if row['source'] == 'wavetable' and row['frames'] == 128:
        print(f"{row['sampleType']} {row['voices']} voices: {row['beforeMedianBatchMeanNs'] / 1000:.3f} -> "
              f"{row['afterMedianBatchMeanNs'] / 1000:.3f} us/block ({row['speedup']:.2f}x)")
