#!/usr/bin/env python3
"""Stage pinned recognition dependencies for release builds (Linux build host).

Downloads happen only during this explicit developer command. The application
never invokes it. Models and downloaded engines are checked against pinned hashes;
locally compiled helpers are recorded with their source hashes and output hashes.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
VENDOR = ROOT / 'vendor'
ZIG_URL = 'https://ziglang.org/download/0.14.1/zig-x86_64-linux-0.14.1.tar.xz'
ZIG_SHA = '24aeeec8af16c381934a6cd7d95c807a8cb2cf7df9fa40d359aa884195c4716c'

def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def fetch(url, destination, expected, size=None):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.is_file() and digest(destination) == expected and (size is None or destination.stat().st_size == size):
        return
    temporary = destination.with_name(destination.name + '.partial')
    try:
        with urllib.request.urlopen(url, timeout=120) as response, temporary.open('wb') as output:
            shutil.copyfileobj(response, output, 1024 * 1024)
        if digest(temporary) != expected or (size is not None and temporary.stat().st_size != size):
            raise RuntimeError('Pinned size or SHA-256 mismatch: ' + destination.name)
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)

def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=['linux', 'windows', 'all'], default='all')
    parser.add_argument('--models-only', action='store_true')
    parser.add_argument('--jobs', type=int, default=3)
    args = parser.parse_args()
    assets = VENDOR / 'assets'
    for model in json.loads((VENDOR / 'provenance/model-provenance.json').read_text()):
        fetch(model['url'], assets / 'models' / model['file'], model['sha256'], model['size'])
    if args.models_only:
        return
    if sys.platform != 'linux':
        parser.error('Native reconstruction runs on Linux; use the release workflow or a Linux build host.')
    platforms = ['linux', 'windows'] if args.platform == 'all' else [args.platform]
    with tempfile.TemporaryDirectory(prefix='speechdesk-native-') as work:
        work = Path(work)
        archive = work / 'zig.tar.xz'
        fetch(ZIG_URL, archive, ZIG_SHA)
        with tarfile.open(archive) as bundle:
            bundle.extractall(work, filter='data')
        zig = work / 'zig-x86_64-linux-0.14.1/zig'
        cmake = shutil.which('cmake')
        if not cmake:
            raise RuntimeError('CMake is required')
        if 'linux' in platforms:
            run(sys.executable, VENDOR / 'tools/fetch_whisper.py', '--linux')
        if 'windows' in platforms:
            run(sys.executable, VENDOR / 'tools/fetch_whisper.py', '--windows', '--zig', zig, '--cmake', cmake, '--jobs', max(1, args.jobs))
        run(sys.executable, VENDOR / 'native/build_audio.py', '--platform', args.platform, '--zig', zig, '--linux-zig')
        needles = json.loads((VENDOR / 'provenance/asset-provenance.json').read_text())
        for platform in platforms:
            folder = assets / 'engines' / platform
            shutil.copytree(VENDOR / 'engines' / platform, folder, dirs_exist_ok=True)
            name = 'needle' if platform == 'linux' else 'needle.exe'
            entry = next(item for item in needles if Path(item['file']).name == name)
            if platform == 'windows':
                original = work / 'needle-original.exe'
                fetch(entry['url'], original, entry['upstream_sha256'], entry['upstream_size'])
                run(sys.executable, VENDOR / 'tools/patch_needle_manifest.py', original, folder / name)
            else:
                fetch(entry['url'], folder / name, entry['sha256'], entry['size'])
            # The Windows resource patch is deterministic; reject altered inputs.
            if digest(folder / name) != entry['sha256']:
                raise RuntimeError('Needle transformation hash mismatch')
            if platform == 'linux':
                pinned = json.loads((VENDOR / 'provenance/whisper-provenance.json').read_text())['files']
                for entry in pinned:
                    if '/linux-x64/' in entry['path']:
                        target = folder / Path(entry['path']).name
                        if digest(target) != entry['sha256']:
                            raise RuntimeError('Pinned whisper.cpp hash mismatch: ' + target.name)
                for file in folder.iterdir():
                    file.chmod(0o755)
    receipt = {
        'application': 'SpeechDesk',
        'source_commit': subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip() if (ROOT / '.git').exists() else None,
        'compiler': 'Zig 0.14.1',
        'note': 'Models and downloaded engines use pinned hashes. Compiled output hashes describe this build, not a bit-for-bit reproduction claim.',
        'source_sha256': {str(file.relative_to(ROOT)): digest(file) for file in [VENDOR / 'native/whistle_audio.c', VENDOR / 'native/build_audio.py', VENDOR / 'tools/fetch_whisper.py']},
        'files': [{'file': str(file.relative_to(assets)), 'size_bytes': file.stat().st_size, 'sha256': digest(file)} for file in sorted(assets.rglob('*')) if file.is_file() and file.name != 'build-manifest.json'],
    }
    (assets / 'build-manifest.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print('Verified model downloads and reconstructed native dependencies are ready.')

if __name__ == '__main__':
    main()
