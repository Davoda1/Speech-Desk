#!/usr/bin/env python3
"""Copy the verified offline CPU payload selected for one platform.

Build-only helper; no downloads or user profile access. The source tree includes
only the downloader/locks in vendor/tts. Set SPEECHDESK_TTS_ASSETS to its prepared
output or pass --assets. Every staged byte is checked against its source hash.
"""
import argparse, hashlib, json, os
from pathlib import Path

def sha256(path):
    digest=hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda:stream.read(1024*1024),b''):digest.update(block)
    return digest.hexdigest()

def inventory(assets,platform):
    entries={}
    for name in ('common',{'linux':'linux-x64','win32':'win32-x64'}[platform]):
        folder=assets/name
        if not folder.is_dir():raise RuntimeError(f'Missing offline TTS assets: {folder}. Run vendor/tts/prepare_assets.py first.')
        for source in sorted(folder.rglob('*')):
            if source.is_dir():continue
            if source.is_symlink() or not source.is_file():raise RuntimeError(f'Only regular TTS asset files may be packaged: {source}')
            relative=source.relative_to(folder).as_posix()
            if relative in entries:raise RuntimeError(f'Duplicate TTS asset: {relative}')
            entries[relative]=source
    if not any(name.startswith('models/') for name in entries) or not any(name.startswith('engines/') for name in entries) or not any(name.startswith('runtime/') for name in entries):
        raise RuntimeError('Incomplete offline TTS assets: models, engines and runtime are required.')
    record=json.loads((assets/{'linux':'linux-x64','win32':'win32-x64'}[platform]/'asset-inventory.json').read_text())
    if record.get('onnxruntime') != '1.31.1':raise RuntimeError('TTS ONNX Runtime must match the application stable 1.31.1 pin.')
    expected={row['path']:row for row in record['files']}
    if set(entries)-{'asset-inventory.json'} != set(expected):raise RuntimeError('TTS payload differs from its complete prepared inventory.')
    for relative,row in expected.items():
        source=entries[relative]
        if source.stat().st_size != row['bytes'] or sha256(source) != row['sha256']:raise RuntimeError(f'TTS inventory mismatch: {relative}')
    return entries

def stage(assets,platform,destination):
    entries=inventory(assets,platform);result={}
    record=json.loads(entries['asset-inventory.json'].read_text());modes={row['path']:row.get('mode',493 if row['path'] in ('runtime/python/bin/python3','engines/pocket/pocket-tts') and platform=='linux' else 420) for row in record['files']}
    if destination.exists() and any(destination.iterdir()):raise RuntimeError('TTS destination must be empty; refusing to merge old runtime files.')
    for relative,source in entries.items():
        target=destination/relative;target.parent.mkdir(parents=True,exist_ok=True)
        expected=sha256(source);mode=modes.get(relative,420)
        with source.open('rb') as inp,target.open('wb') as out:
            for block in iter(lambda:inp.read(1024*1024),b''):out.write(block)
            out.flush();os.fsync(out.fileno())
        target.chmod(mode)
        if sha256(target)!=expected:raise RuntimeError(f'Incomplete copied TTS asset: {relative}')
        result[relative]={'sha256':expected,'size':source.stat().st_size,'mode':mode}
    worker=Path(__file__).resolve().parents[1]/'vendor/tts/voice_light.py'
    target=destination/'voice_light.py';target.write_bytes(worker.read_bytes());target.chmod(0o644)
    result['voice_light.py']={'sha256':sha256(target),'size':target.stat().st_size,'mode':0o644}
    (destination/'PACKAGED-ASSETS.json').write_bytes((json.dumps({'schema':1,'platform':platform,'files':result},indent=2)+'\n').encode('utf-8'))
    print(json.dumps({'platform':platform,'files':len(result),'bytes':sum(row['size'] for row in result.values())}))

def validate_staged(destination, platform):
    record=json.loads((destination/'PACKAGED-ASSETS.json').read_text())
    if record.get('platform') != platform:
        raise RuntimeError('Offline TTS platform does not match the application package.')
    expected=record['files']
    actual={p.relative_to(destination).as_posix() for p in destination.rglob('*') if p.is_file()}
    if actual != set(expected)|{'PACKAGED-ASSETS.json'}:
        raise RuntimeError('Staged offline TTS payload differs from its verified inventory.')
    for relative,row in expected.items():
        source=destination/relative
        if source.is_symlink() or source.stat().st_size != row['size'] or sha256(source) != row['sha256']:
            raise RuntimeError('Staged offline TTS asset integrity mismatch: '+relative)
        if platform=='linux' and relative in ('runtime/python/bin/python3','engines/pocket/pocket-tts') and not source.stat().st_mode & 0o111:
            raise RuntimeError('Offline TTS executable mode is missing: '+relative)
    for name in ('voice_light.py','models/pocket/model.safetensors','models/kokoro/kokoro-v1.0.onnx',
                 'runtime/python/python.exe' if platform=='win32' else 'runtime/python/bin/python3',
                 'engines/pocket/pocket-tts.exe' if platform=='win32' else 'engines/pocket/pocket-tts'):
        if name not in expected:
            raise RuntimeError('Offline TTS required asset is missing: '+name)

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--assets',type=Path,required=True);parser.add_argument('--platform',choices=['linux','win32'],required=True);parser.add_argument('--destination',type=Path,required=True)
    args=parser.parse_args();stage(args.assets.resolve(),args.platform,args.destination.resolve())
if __name__=='__main__':main()
