#!/usr/bin/env python3
"""Fetch the exact, hash-locked Pocket and Kokoro offline release payload.

Usage: python3 prepare_assets.py --output /path/to/tts-assets
The output has common/, linux-x64/ and win32-x64/ trees. Merge common plus
one platform tree into the application's resources/tts-assets directory.
Only this explicit build step uses the network; inference never downloads.
"""
from __future__ import annotations
import argparse
import concurrent.futures
import hashlib
import json
import os
import re
from pathlib import Path, PurePosixPath
import shutil
import stat
import tarfile
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parent

def sha256(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(data)
    return h.hexdigest()

def safe_name(name):
    path = PurePosixPath(name)
    if path.is_absolute() or '..' in path.parts or '\\' in name:
        raise ValueError('Unsafe archive member: ' + name)
    return path

def extract_archive(archive, dest, kind):
    dest.mkdir(parents=True, exist_ok=True)
    if kind == 'wheel':
        with zipfile.ZipFile(archive) as z:
            for member in z.infolist():
                rel = safe_name(member.filename)
                if member.is_dir():
                    continue
                # Wheels may place importable modules in a purelib/platlib subtree.
                parts = rel.parts
                if len(parts) > 2 and parts[0].endswith('.data') and parts[1] in ('purelib','platlib'):
                    rel = PurePosixPath(*parts[2:])
                target = dest / str(rel)
                target.parent.mkdir(parents=True, exist_ok=True)
                with z.open(member) as src, target.open('wb') as out:
                    shutil.copyfileobj(src, out)
                target.chmod(0o755 if (member.external_attr >> 16) & 0o111 else 0o644)
        return
    with tempfile.TemporaryDirectory(prefix='extract-', dir=dest.parent) as temporary:
        temp = Path(temporary)
        if zipfile.is_zipfile(archive):
            with zipfile.ZipFile(archive) as z:
                for member in z.infolist():
                    safe_name(member.filename)
                z.extractall(temp)
        else:
            with tarfile.open(archive) as t:
                for member in t.getmembers():
                    safe_name(member.name)
                    if member.isdev() or member.isfifo():
                        raise ValueError('Special archive member')
                    if member.issym() or member.islnk():
                        base = temp / member.name
                        target = (base.parent / member.linkname).resolve() if member.issym() else (temp / member.linkname).resolve()
                        if not target.is_relative_to(temp.resolve()):
                            raise ValueError('Outside-root archive link')
                t.extractall(temp, filter='data')
        if kind == 'python-archive':
            source = temp / 'python'
            if not source.is_dir():
                raise ValueError('Python archive has no python/ root')
            # Release payloads contain regular files; do not retain symlinks.
            shutil.copytree(source, dest, dirs_exist_ok=True, symlinks=False)
        elif kind == 'pocket-archive':
            candidates = [p for p in temp.rglob('*') if p.is_file() and p.name in ('pocket-tts', 'pocket-tts.exe')]
            if len(candidates) != 1:
                raise ValueError('Pocket archive has no unique executable')
            shutil.copytree(candidates[0].parent, dest, dirs_exist_ok=True, symlinks=False)
            if candidates[0].name == 'pocket-tts':
                (dest / 'pocket-tts').chmod(0o755)

def prepare(entry, output, cache):
    key = entry['sha256']
    download = cache / key
    if not download.is_file() or sha256(download) != key:
        partial = cache / (key + '.part')
        req = urllib.request.Request(entry['url'], headers={'User-Agent':'SpeechDesk-offline-assets-build/2'})
        try:
            with urllib.request.urlopen(req, timeout=90) as response, partial.open('wb') as out:
                shutil.copyfileobj(response, out, 1024 * 1024)
            if sha256(partial) != key or ('size' in entry and partial.stat().st_size != entry['size']):
                raise ValueError('Asset hash/size mismatch: ' + entry['target'])
            partial.replace(download)
        finally:
            partial.unlink(missing_ok=True)
    target = output / entry['target']
    if entry['kind'] == 'file':
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(download, target)
        if sha256(target) != key:
            raise ValueError('Copied asset hash mismatch: ' + entry['target'])
    else:
        extract_archive(download, target, entry['kind'])
    print('Verified ' + entry['target'] + ('/' + entry['filename'] if entry.get('filename') else ''), flush=True)

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--cache', type=Path)
    ap.add_argument('--workers', type=int, default=4)
    ap.add_argument('--platform', choices=('all','linux-x64','win32-x64'), default='all')
    args = ap.parse_args()
    output = args.output.resolve()
    cache = (args.cache or output.parent / '.tts-downloads').resolve()
    output.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((ROOT / 'assets-manifest.json').read_text())
    platforms = ('linux-x64','win32-x64') if args.platform == 'all' else (args.platform,)
    selected = lambda name: name.startswith(('common/',) + tuple(p+'/' for p in platforms))
    support = ROOT / 'support'
    expected_support = json.loads((ROOT / 'support-manifest.json').read_text())['files']
    for entry in expected_support:
        if not selected(entry['path']):
            continue
        item = support / entry['path']
        if not item.is_file() or item.is_symlink() or item.stat().st_size != entry['size'] or sha256(item) != entry['sha256']:
            raise ValueError('Source support-file integrity mismatch: ' + entry['path'])
    # Shared pure-Python wheels occur in both platforms. Download once per hash,
    # then extract sequentially to avoid races in the runtime directories.
    unique = {}
    for entry in manifest['assets']:
        if not selected(entry['target']):
            continue
        unique.setdefault(entry['sha256'], []).append(entry)
    def group(entries):
        for entry in entries:
            prepare(entry, output, cache)
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, min(args.workers, 8))) as pool:
        list(pool.map(group, unique.values()))
    # Copy only the verified support files selected for this platform.
    for entry in expected_support:
        if selected(entry['path']):
            target = output / entry['path']
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(support / entry['path'], target)
    # Temporary managed-filesystem copies are not release inputs.
    for item in sorted(output.rglob('*'), reverse=True):
        if item.is_file() and (item.suffix in ('.pyc','.pyo') or re.match(r'^\..+\.[A-Za-z0-9]{6}$',item.name)):
            item.unlink()
    # Runtime aliases and developer launcher scripts are unnecessary for this
    # explicit Python API. Retain the standalone interpreter's real binary.
    bin_dir = output / 'linux-x64/runtime/python/bin'
    if bin_dir.is_dir():
        for item in bin_dir.iterdir():
            if item.name != 'python3' and item.is_file():
                item.unlink()
    file_manifest = []
    for path in sorted(output.rglob('*')):
        if path.is_file() and path.name not in ('payload-sha256.json','asset-inventory.json'):
            relative = path.relative_to(output).as_posix()
            mode = 0o755 if relative in ('linux-x64/runtime/python/bin/python3','linux-x64/engines/pocket/pocket-tts') else 0o644
            path.chmod(mode)
            file_manifest.append({'path':relative, 'bytes':path.stat().st_size, 'sha256':sha256(path), 'mode':mode})
    (output / 'payload-sha256.json').write_text(json.dumps({'schema':1,'onnxruntime':manifest['onnxruntime'],'files':file_manifest}, indent=2)+'\n')
    for platform in platforms:
        files = [{**item,'path':item['path'].split('/',1)[1]} for item in file_manifest if item['path'].startswith(('common/',platform+'/'))]
        (output/platform/'asset-inventory.json').write_text(json.dumps({'schema':1,'platform':platform,'onnxruntime':manifest['onnxruntime'],'files':files},indent=2)+'\n')
    print('Complete: ' + str(sum(f['bytes'] for f in file_manifest)) + ' bytes, ' + str(len(file_manifest)) + ' files.', flush=True)

if __name__ == '__main__':
    main()
