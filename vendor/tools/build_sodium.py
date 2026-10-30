#!/usr/bin/env python3
"""Build the pinned libsodium source with the application's native C compiler.

Linux requires make and a C compiler. Windows requires MSYS2 bash/make plus
the same MinGW compiler installed with Qt; no MSYS2 compiler is installed.
Only the static library is used, so packages need no additional runtime DLL.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
PROVENANCE = ROOT / 'vendor/provenance/libsodium.json'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('build/deps/sodium'))
    parser.add_argument('--cache', type=Path, default=ROOT / '.cache')
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--shell', default='sh')
    parser.add_argument('--make', default='make')
    parser.add_argument('--jobs', type=int, default=3)
    args = parser.parse_args()
    pin = json.loads(PROVENANCE.read_text(encoding='utf-8'))
    output = args.output.resolve()
    cache = args.cache.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / f"libsodium-{pin['version']}.tar.gz"
    if not archive.is_file():
        temporary = archive.with_suffix('.partial')
        try:
            with urllib.request.urlopen(pin['source_url'], timeout=60) as response, temporary.open('wb') as target:
                shutil.copyfileobj(response, target)
            temporary.replace(archive)
        finally:
            temporary.unlink(missing_ok=True)
    with archive.open('rb') as stream:
        if hashlib.file_digest(stream, 'sha256').hexdigest() != pin['source_sha256']:
            raise RuntimeError('libsodium source SHA-256 mismatch; refusing to build')

    shell = shutil.which(args.shell)
    compiler = shutil.which(args.cc)
    make = shutil.which(args.make)
    if not all((shell, compiler, make)):
        raise RuntimeError('A C compiler, POSIX shell and make are required; see docs/Build.md')
    compiler = str(Path(compiler).resolve())
    compiler_version = subprocess.check_output([compiler, '--version'], text=True).splitlines()[0]
    env = os.environ.copy()
    # Keep Qt's compiler ahead of shell utilities and any other installed GCC.
    env['PATH'] = os.pathsep.join([str(Path(compiler).parent), str(Path(shell).parent), env['PATH']])

    def shell_path(path):
        path = str(Path(path).resolve())
        if os.name == 'nt':
            return subprocess.check_output([shell, '-c', 'cygpath -u "$1"', 'speechdesk-path', path], env=env, text=True).strip()
        return path

    work = output.parent / 'sodium-build'
    source = work / f"libsodium-{pin['version']}"
    work.mkdir(parents=True, exist_ok=True)
    if not source.is_dir():
        with tarfile.open(archive) as payload:
            payload.extractall(work, filter='data')
    build = work / 'build'
    build.mkdir(exist_ok=True)
    quote = shlex.quote
    configure = [shell_path(source / 'configure'), '--disable-shared', '--enable-static',
                 '--with-pic', '--disable-dependency-tracking', '--prefix=' + shell_path(output)]
    if os.name == 'nt':
        configure.append('--host=x86_64-w64-mingw32')
    # Avoid local build paths in the statically linked dependency, and do not
    # tune for the build host's CPU. Upstream selects optimized code at runtime.
    cflags = '-O2 -g0 -ffile-prefix-map=' + shell_path(work) + '=libsodium'
    command = 'set -eu\n' + ' '.join([
        'CC=' + quote(shell_path(compiler)), 'CFLAGS=' + quote(cflags),
        *[quote(item) for item in configure],
    ]) + '\n'
    command += quote(shell_path(make)) + ' -j' + str(max(1, args.jobs)) + '\n'
    command += quote(shell_path(make)) + ' install\n'
    subprocess.run([shell, '-c', command], cwd=build, env=env, check=True)
    library = output / 'lib/libsodium.a'
    if not library.is_file() or not (output / 'include/sodium.h').is_file():
        raise RuntimeError('libsodium did not install the required static library and headers')
    with library.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    receipt = {'component': 'libsodium', 'version': pin['version'],
               'source_url': pin['source_url'], 'source_sha256': pin['source_sha256'],
               'linkage': 'static', 'compiler': compiler_version,
               'library_sha256': digest}
    (output / 'build-manifest.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print('Pinned libsodium static library ready:', output)


if __name__ == '__main__':
    main()
