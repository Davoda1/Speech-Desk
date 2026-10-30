#!/usr/bin/env python3
"""Package staged runtimes or tracked source; write one final checksum manifest."""
from __future__ import annotations
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = re.search(r'project\(SpeechDesk VERSION ([0-9.]+)', (ROOT/'CMakeLists.txt').read_text()).group(1)
PREFIX = f'SpeechDesk-{VERSION}'

def runtime_files(root):
    return sorted(p for p in root.rglob('*') if p.is_file() and not any(part.startswith('.') for part in p.relative_to(root).parts))

def source_files():
    try:
        names = subprocess.check_output(['git', '-C', str(ROOT), 'ls-files', '-z']).decode().split('\0')
        if any(names):
            return [ROOT/name for name in names if name and (ROOT/name).is_file()]
    except (OSError, subprocess.CalledProcessError):
        pass
    # Extracted bundles have no .git. Retain .github and omit build products.
    files=[]
    for name in ['src', 'tests', 'resources', 'docs', 'packaging', '.github', 'vendor']:
        for file in (ROOT/name).rglob('*'):
            relative = file.relative_to(ROOT)
            if file.is_file() and not set(relative.parts).intersection({'assets','engines','models','__pycache__'}) and file.suffix not in {'.pyc','.log'}:
                files.append(file)
    files += [p for p in ROOT.iterdir() if p.is_file() and (p.suffix in {'.md','.txt'} or p.name in {'.gitignore','.gitattributes'})]
    return sorted(set(files))

def archive_zip(root, files, target, prefix=''):
    with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for file in files:
            name = (Path(prefix)/file.relative_to(root)).as_posix()
            info = zipfile.ZipInfo(name, (2026,1,1,0,0,0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = (file.stat().st_mode & 0xffff) << 16
            with file.open('rb') as src, archive.open(info,'w') as dst:
                shutil.copyfileobj(src,dst,1024*1024)

def archive_tar(root, files, target, prefix=''):
    with target.open('wb') as raw, gzip.GzipFile(filename='',mode='wb',fileobj=raw,mtime=0) as compressed, tarfile.open(fileobj=compressed, mode='w|') as archive:
        for file in files:
            info=archive.gettarinfo(str(file),arcname=(Path(prefix)/file.relative_to(root)).as_posix())
            info.uid=info.gid=0
            info.uname=info.gname='root'
            info.mtime=0
            with file.open('rb') as stream:
                archive.addfile(info,stream)

def copy_documents(runtime, output):
    for file in [ROOT/'LICENSE.txt', ROOT/'SECURITY.md', ROOT/'docs/Release-Notes.md', ROOT/'docs/Server-Profiles.md', ROOT/'docs/Verification.md', ROOT/'docs/Optional-Models.md', ROOT/'docs/User-Guide.md']:
        shutil.copy2(file, runtime/file.name)
    guide = runtime/'User-Guide.md'
    guide.write_text(guide.read_text(encoding='utf-8').replace('](../SECURITY.md)', '](SECURITY.md)'), encoding='utf-8')
    shutil.copy2(output/f'{PREFIX}-README.html', runtime/'README.html')
    shutil.copy2(ROOT/'vendor/optional-models.json',runtime/'optional-models.json')
    inventory=json.loads((ROOT/'vendor/native-assets.json').read_text())
    inventory['provenance']='Exact native payload in this package; upstream sources are recorded per component.'
    actual=[]
    for entry in inventory['files']:
        target=runtime/entry['deployment_path']
        if target.is_file():
            entry=dict(entry)
            with target.open('rb') as stream:entry['sha256']=hashlib.file_digest(stream,'sha256').hexdigest()
            entry['size_bytes']=target.stat().st_size
            entry.pop('pinned_hash_matches',None)
            actual.append(entry)
    inventory['files']=actual
    (runtime/'native-assets.json').write_text(json.dumps(inventory,indent=2)+'\n')

def validate_runtime(root, platform):
    binary=root/('SpeechDesk.exe' if platform=='windows' else 'speechdesk')
    if not binary.is_file() or binary.is_symlink():
        raise RuntimeError('A real executable must be at the archive root')
    for file in runtime_files(root):
        if file.suffix.lower() in {'.sh','.cmd','.bat','.ps1','.sha256'}:
            raise RuntimeError('Unexpected launcher or per-file checksum: '+file.name)
        if file.name.startswith('ggml-large-'):
            raise RuntimeError('Optional model weights must not enter the runtime')
    for model in ['whistle.cact','ggml-tiny-q5_1.bin','ggml-base-q5_1.bin','ggml-small-q5_1.bin']:
        if not (root/'resources/models'/model).is_file():
            raise RuntimeError('Missing included model: '+model)
    if not list(root.glob('Qt6Core.dll')) and not list((root/'lib').glob('libQt6Core.so*')):
        raise RuntimeError('Qt Core runtime is missing')
    if not list(root.glob('Qt6WebSockets.dll')) and not list((root/'lib').glob('libQt6WebSockets.so*')):
        raise RuntimeError('Qt WebSockets runtime is missing')
    if not (root/'licenses/libsodium-ISC.txt').is_file():
        raise RuntimeError('libsodium license is missing')
    sodium=json.loads((root/'runtime-manifest.json').read_text(encoding='utf-8')).get('static_dependencies',{}).get('libsodium',{})
    if sodium.get('version') != '1.0.22' or sodium.get('linkage') != 'static':
        raise RuntimeError('Pinned libsodium build provenance is missing')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=['windows','linux','source','checksums'], required=True)
    parser.add_argument('--runtime', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args=parser.parse_args()
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    if args.platform=='source':
        files=source_files()
        archive_zip(ROOT,files,out/f'{PREFIX}-Source.zip',f'{PREFIX}-Source')
        archive_tar(ROOT,files,out/f'{PREFIX}-Source.tar.gz',f'{PREFIX}-Source')
    elif args.platform=='checksums':
        checksum=out/'SHA256SUM.txt'
        lines=[]
        for file in sorted(out.iterdir()):
            if file.is_file() and file!=checksum:
                with file.open('rb') as stream:
                    lines.append(f"{hashlib.file_digest(stream,'sha256').hexdigest()}  {file.name}")
        checksum.write_text('\n'.join(lines)+'\n')
    else:
        if not args.runtime:
            parser.error('--runtime is required for platform packaging')
        runtime=args.runtime.resolve()
        copy_documents(runtime,out)
        validate_runtime(runtime,args.platform)
        if args.platform=='windows':
            archive_zip(runtime,runtime_files(runtime),out/f'{PREFIX}-Windows-x64.zip')
        else:
            archive_tar(runtime,runtime_files(runtime),out/f'{PREFIX}-Linux-amd64.tar.gz')
            deb_root=runtime.parent/'deb-root'
            if deb_root.exists():shutil.rmtree(deb_root)
            subprocess.run(['python3',str(ROOT/'packaging/stage_deb.py'),'--portable',str(runtime),'--output',str(deb_root)],check=True)
            doc=deb_root/'usr/share/doc/speechdesk';doc.mkdir(parents=True,exist_ok=True)
            for name in ['README.html','LICENSE.txt','SECURITY.md','Release-Notes.md','Server-Profiles.md','Verification.md','Optional-Models.md','User-Guide.md']:
                shutil.copy2(runtime/name,doc/name)
            control=deb_root/'DEBIAN/control'
            control.write_text(re.sub(r'Installed-Size: \d+', f"Installed-Size: {sum(f.stat().st_size for f in runtime_files(deb_root/'usr'))//1024}",control.read_text()))
            subprocess.run(['dpkg-deb','--root-owner-group','-Zxz','-z3','--build',str(deb_root),str(out/f'{PREFIX}-Linux-amd64.deb')],check=True)
    print('Release packaging complete:',args.platform)

if __name__=='__main__':
    main()

