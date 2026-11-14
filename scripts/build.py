"""Configure/build static CyAudioCore. No native test or example is launched."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--preset', default='windows-x64' if sys.platform == 'win32' else 'portable')
p.add_argument('--cmake')
p.add_argument('--cydsp-source', type=Path, help='Explicit local CyDSP source directory; overrides the preset')
p.add_argument('--compile-checks', action='store_true', help='Also compile contract/regression/example objects; no test executable')
a = p.parse_args()
cmake = a.cmake or shutil.which('cmake')
if not cmake and sys.platform == 'win32':
    candidate = Path(r'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe')
    if candidate.is_file(): cmake = str(candidate)
if not cmake:
    p.error('CMake 3.24+ is required; install/configure it separately or pass --cmake PATH')
options = []
if a.cydsp_source:
    dependency = a.cydsp_source.resolve()
    if not (dependency/'CMakeLists.txt').is_file():
        p.error('--cydsp-source must contain the CyDSP CMakeLists.txt')
    options.append('-DCYAUDIOCORE_CYDSP_SOURCE_DIR=' + str(dependency))
environment = {key.upper(): value for key, value in os.environ.items()}
subprocess.run([cmake, '--preset', a.preset, '-DCYAUDIOCORE_BUILD_TESTS=OFF', '-DCYAUDIOCORE_BUILD_MEASUREMENTS=OFF', '-DCYAUDIOCORE_BUILD_PATH_MEASUREMENTS=OFF', '-DCYAUDIOCORE_SANITIZE_ADDRESS=OFF','-DCYDSP_BUILD_TESTS=OFF','-DCYDSP_BUILD_MEASUREMENTS=OFF','-DCYDSP_SANITIZE_ADDRESS=OFF',
    '-DCYAUDIOCORE_COMPILE_CHECKS=' + ('ON' if a.compile_checks else 'OFF')] + options, cwd=ROOT, env=environment, check=True)
subprocess.run([cmake, '--build', '--preset', a.preset, '--parallel'], cwd=ROOT, env=environment, check=True)
