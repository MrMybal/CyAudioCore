"""Stable digest of compiler inputs for validation report freshness."""
import hashlib
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]
def source_digest(root=ROOT):
    root=Path(root).resolve(strict=True)
    digest=hashlib.sha256()
    files=[root/'CMakeLists.txt', root/'third_party/manifest.json']
    files.extend(p for p in (root/'third_party').rglob('*') if p.suffix in ('.h', '.hpp', '.cpp', '.c', '.S', '.asm'))
    for directory in ('include','src','tests','examples','cmake'):
        files.extend(p for p in (root/directory).rglob('*') if p.suffix in ('.h','.hpp','.cpp','.cmake','.in','.def','.ttl','.jsfx'))
    for path in sorted(files):
        digest.update(path.relative_to(root).as_posix().encode()+b'\0'+path.read_bytes()+b'\0')
    return digest.hexdigest()

def dependency_fingerprint(root):
    """Hash actual dependency inputs, including uncommitted files; never fetch or edit."""
    root=Path(root).resolve(strict=True)
    result={'sourceSha256':source_digest(root),'revision':None,'trackedChanges':None}
    command=['git','-c','safe.directory='+root.as_posix(),'-C',str(root)]
    try:
        top=Path(subprocess.check_output(command+['rev-parse','--show-toplevel'],stderr=subprocess.DEVNULL,text=True,timeout=10).strip()).resolve()
        if top!=root:
            return result  # Never attribute a nested source archive to its parent's repository.
        result['revision']=subprocess.check_output(command+['rev-parse','HEAD'],stderr=subprocess.DEVNULL,text=True,timeout=10).strip()
        result['trackedChanges']=bool(subprocess.check_output(command+['diff','HEAD','--name-only'],stderr=subprocess.DEVNULL,text=True,timeout=10).strip())
    except (OSError,subprocess.SubprocessError):
        pass  # A source archive need not have Git metadata; its content hash remains available.
    return result
