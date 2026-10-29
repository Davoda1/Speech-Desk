#!/usr/bin/env python3
"""Fetch pinned whisper.cpp assets and reproduce the Windows CPU binaries.

Python 3.12+ is required for safe tar extraction. Linux assets are the upstream
CPU release; Windows builds use Zig 0.14.1 and static C++ linkage so installation
of a Visual C++ redistributable is unnecessary. The Windows system UCRT is used.

Examples (from the source root):
  python vendor/tools/fetch_whisper.py --linux
  python vendor/tools/fetch_whisper.py --windows --zig /path/to/zig --cmake /path/to/cmake

This command reconstructs the pinned native dependencies under vendor/engines.
Bundled application packages already include these engines.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
COMMIT = "d1be6fde11ac6e0407606b4e42fe72d34add8037"
RELEASE = "b5454"
VERSION = "1.9.5"
ASSETS = {
    "linux": (
        f"https://github.com/ggml-org/whisper.cpp/releases/download/{RELEASE}/whisper-bin-ubuntu-x64.tar.gz",
        "a72becf15d7917f990f6313867a52638b82b7f9ef237fb0c980dac56a135781c",
    ),
    "source": (
        f"https://api.github.com/repos/ggml-org/whisper.cpp/tarball/{COMMIT}",
        "be49b097b23e24283c1070e6df34337134757e9be05a6525e3c526d943051adb",
    ),
}


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def download(kind: str, directory: Path) -> Path:
    url, expected = ASSETS[kind]
    path = directory / f"{kind}.tar.gz"
    with urllib.request.urlopen(url, timeout=120) as response, path.open("wb") as out:
        shutil.copyfileobj(response, out, length=1024 * 1024)
    if digest(path) != expected:
        raise RuntimeError(f"SHA256 mismatch for {kind}; refusing unverified bytes")
    extracted = directory / kind
    extracted.mkdir()
    with tarfile.open(path, "r:gz") as archive:
        archive.extractall(extracted, filter="data")
    return next(extracted.iterdir())


def copy_linux(source: Path) -> None:
    out = ROOT / "engines" / "linux"
    out.mkdir(parents=True, exist_ok=True)
    names = ["whisper-cli", "libwhisper.so.1", "libggml.so.0", "libggml-base.so.0"]
    names += [path.name for path in source.glob("libggml-cpu*.so")]
    for name in names:
        # Dereference upstream symlinks. Runtime files are regular files and
        # use their ELF SONAMEs; upstream RPATH is $ORIGIN.
        shutil.copy2(source / name, out / name)
    licenses = ROOT / "licenses"
    licenses.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source / "LICENSE", licenses / "whisper.cpp-MIT.txt")


def zig_toolchain(directory: Path, zig: Path) -> Path:
    # CMake archiver arguments cannot be supplied as a semicolon command list.
    # These temporary build helpers are never part of application packages.
    for command in ("ar", "ranlib"):
        wrapper = directory / f"zig-{command}"
        wrapper.write_text(
            f"#!{sys.executable}\nimport os, sys\n"
            f"os.execv({str(zig)!r}, [{str(zig)!r}, {command!r}, *sys.argv[1:]])\n",
            encoding="utf-8",
        )
        wrapper.chmod(0o755)
    toolchain = directory / "zig-windows.cmake"
    toolchain.write_text(
        "set(CMAKE_SYSTEM_NAME Windows)\n"
        "set(CMAKE_SYSTEM_PROCESSOR AMD64)\n"
        f'set(CMAKE_C_COMPILER "{zig}" cc -target x86_64-windows-gnu)\n'
        f'set(CMAKE_CXX_COMPILER "{zig}" c++ -target x86_64-windows-gnu)\n'
        f'set(CMAKE_AR "{directory / "zig-ar"}")\n'
        f'set(CMAKE_RANLIB "{directory / "zig-ranlib"}")\n'
        'set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")\n'
        "set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)\n",
        encoding="utf-8",
    )
    return toolchain


def build_windows(source: Path, directory: Path, zig: Path, cmake: Path, jobs: int) -> None:
    if subprocess.check_output([str(zig), "version"], text=True).strip() != "0.14.1":
        raise RuntimeError("This build is pinned to Zig 0.14.1")
    toolchain = zig_toolchain(directory, zig)
    env = os.environ.copy()
    env["ZIG_GLOBAL_CACHE_DIR"] = str(directory / "zig-global-cache")
    env["ZIG_LOCAL_CACHE_DIR"] = str(directory / "zig-local-cache")
    # The upstream CLI uses narrow main()/fstream paths. Request UTF-8 as its
    # Windows active code page so Hungarian and other Unicode paths survive
    # command-line conversion and file creation (Windows 10 version 1903+).
    manifest = directory / "utf8.manifest"
    manifest.write_text(
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
        '<assembly manifestVersion="1.0" xmlns="urn:schemas-microsoft-com:asm.v1">\n'
        '<assemblyIdentity type="win32" name="SpeechDesk.WhisperEngine" '
        'version="1.9.5.0" processorArchitecture="amd64"/>\n'
        '<application xmlns="urn:schemas-microsoft-com:asm.v3"><windowsSettings>\n'
        '<activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">'
        'UTF-8</activeCodePage>\n'
        '</windowsSettings></application>\n</assembly>\n', encoding="utf-8",
    )
    resource_script = directory / "utf8.rc"
    resource_script.write_text(f'1 24 "{manifest}"\n', encoding="utf-8")
    resource = directory / "utf8.res"
    subprocess.run([str(zig), "rc", "/fo", str(resource), str(resource_script)], env=env, check=True)
    out = ROOT / "engines" / "windows"
    out.mkdir(parents=True, exist_ok=True)
    for optimized in (False, True):
        build = directory / ("windows-avx2" if optimized else "windows-baseline")
        feature = "ON" if optimized else "OFF"
        options = [
            str(cmake), "-S", str(source), "-B", str(build),
            f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", "-DCMAKE_BUILD_TYPE=Release",
            "-DBUILD_SHARED_LIBS=OFF", "-DGGML_NATIVE=OFF", "-DGGML_OPENMP=OFF",
            "-DWHISPER_BUILD_IS_DEV=OFF", "-DWHISPER_BUILD_TESTS=OFF",
            "-DWHISPER_BUILD_SERVER=OFF",
            f"-DCMAKE_EXE_LINKER_FLAGS=-static {resource}",
        ]
        options += [f"-DGGML_{name}={feature}" for name in ("AVX", "AVX2", "FMA", "F16C", "BMI2")]
        subprocess.run(options, env=env, check=True)
        subprocess.run([str(cmake), "--build", str(build), "--target", "whisper-cli", "--parallel", str(jobs)], env=env, check=True)
        name = "whisper-cli-avx2.exe" if optimized else "whisper-cli.exe"
        shutil.copy2(build / "bin" / "whisper-cli.exe", out / name)
    licenses = ROOT / "licenses"
    licenses.mkdir(parents=True, exist_ok=True)
    for relative, name in [
        ("lib/libcxx/LICENSE.TXT", "whisper-windows-libcxx.txt"),
        ("lib/libcxxabi/LICENSE.TXT", "whisper-windows-libcxxabi.txt"),
        ("lib/libc/mingw/COPYING", "whisper-windows-mingw.txt"),
        ("LICENSE", "whisper-windows-zig.txt"),
    ]:
        shutil.copy2(zig.parent / relative, licenses / name)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--linux", action="store_true")
    parser.add_argument("--windows", action="store_true")
    parser.add_argument("--zig", type=Path)
    parser.add_argument("--cmake", type=Path)
    parser.add_argument("--jobs", type=int, default=3)
    args = parser.parse_args()
    if not args.linux and not args.windows:
        parser.error("Choose --linux and/or --windows")
    if args.windows and (not args.zig or not args.cmake):
        parser.error("--windows requires paths to --zig and --cmake")
    with tempfile.TemporaryDirectory(prefix="speechdesk-whisper-") as work:
        directory = Path(work)
        if args.linux:
            copy_linux(download("linux", directory))
        if args.windows:
            if os.name == "nt":
                parser.error("This reproducible cross-build runs on Linux; use shipped Windows binaries on Windows")
            build_windows(download("source", directory), directory, args.zig.resolve(), args.cmake.resolve(), max(1, args.jobs))
    print(json.dumps({"version": VERSION, "commit": COMMIT, "status": "ready"}))


if __name__ == "__main__":
    main()
