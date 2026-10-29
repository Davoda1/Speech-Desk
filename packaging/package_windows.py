#!/usr/bin/env python3
"""Stage SpeechDesk's MinGW Windows portable package, with no launch scripts.

Requires a Qt 6 MinGW SDK, MinGW objdump/compiler tools, and the model/native
asset tree. The same helper works on Windows or a Linux cross-build host.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import shutil
import subprocess

SOURCE = Path(__file__).resolve().parents[1]
MODELS = ("whistle.cact", "ggml-tiny-q5_1.bin", "ggml-base-q5_1.bin", "ggml-small-q5_1.bin")
ENGINES = ("needle.exe", "whisper-cli.exe", "whisper-cli-avx2.exe", "whistle-audio.exe")
PLUGINS = {
    "platforms": ["qwindows.dll", "qoffscreen.dll"],
    "multimedia": ["ffmpegmediaplugin.dll", "windowsmediaplugin.dll"],
    "tls": ["qschannelbackend.dll", "qcertonlybackend.dll"],
    "imageformats": ["qgif.dll", "qico.dll", "qjpeg.dll", "qsvg.dll"],
    "iconengines": ["qsvgicon.dll"],
    "styles": ["qmodernwindowsstyle.dll"],
}

def copy(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)

def sha256(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()

def stage(args):
    out = args.output.resolve()
    qt = args.qt.resolve()
    out.mkdir(parents=True, exist_ok=True)
    exe = out / "SpeechDesk.exe"
    copy(args.binary, exe)
    lookup = {f.name.lower(): f for f in (qt / "bin").glob("*.dll")}
    for name in ("libstdc++-6.dll", "libgcc_s_seh-1.dll", "libwinpthread-1.dll"):
        found = Path(subprocess.check_output([args.compiler, "-print-file-name=" + name], text=True).strip())
        if not found.is_file():
            raise RuntimeError(f"Missing compiler runtime: {name}")
        lookup[name.lower()] = found
    queue = [exe]
    for directory, names in PLUGINS.items():
        for name in names:
            source = qt / "plugins" / directory / name
            if not source.is_file():
                raise RuntimeError(f"Missing Qt plugin: {source}")
            target = out / "plugins" / directory / name
            copy(source, target)
            queue.append(target)
    seen = set()
    copied = set()
    system = set()
    while queue:
        binary = queue.pop()
        if binary.name.lower() in seen:
            continue
        seen.add(binary.name.lower())
        imports = subprocess.check_output([args.objdump, "-p", str(binary)], text=True)
        for dep in re.findall(r"DLL Name: (\S+)", imports):
            key = dep.lower()
            if key in lookup:
                target = out / lookup[key].name
                if key not in copied:
                    copy(lookup[key], target)
                    copied.add(key)
                queue.append(target)
            elif key.startswith(("qt6", "libgcc", "libstdc", "libwinpthread", "avcodec", "avformat", "avutil", "swscale", "swresample")):
                raise RuntimeError(f"Unresolved application DLL {dep} needed by {binary.name}")
            else:
                system.add(dep)
    for name in MODELS:
        copy(args.assets / "models" / name, out / "resources/models" / name)
    for name in ENGINES:
        copy(args.assets / "engines/windows" / name, out / "resources/bin" / name)
    for source in (SOURCE / "vendor/licenses").iterdir():
        if source.is_file() and not source.name.startswith("."):
            copy(source, out / "licenses" / source.name)
    if (args.assets / 'build-manifest.json').is_file():
        copy(args.assets / 'build-manifest.json', out / 'native-build-manifest.json')
    (out / "qt.conf").write_text("[Paths]\nPrefix=.\nPlugins=plugins\n", encoding="utf-8")
    source_files = [SOURCE / "CMakeLists.txt", *sorted((SOURCE / "src").glob("*")),
                    *sorted((SOURCE / "resources").glob("*"))]
    source_hashes = {str(f.relative_to(SOURCE)).replace("\\", "/"): sha256(f)
                     for f in source_files if f.is_file() and not f.name.startswith(".")}
    qt_version = (qt / "mkspecs/qconfig.pri").read_text(errors="replace")
    match = re.search(r"^QT_VERSION\s*=\s*(\S+)", qt_version, re.MULTILINE)
    manifest = {
        "application": "SpeechDesk", "version": "1.0.0", "platform": "windows-x64",
        "qt_version": match.group(1) if match else "unknown",
        "compiler": subprocess.check_output([args.compiler, "--version"], text=True).splitlines()[0],
        "source_provenance": "Source archives and build-manifest.json",
        "source_files_sha256": source_hashes,
        "copied_dlls": sorted(copied), "system_dlls": sorted(system), "plugins": PLUGINS,
        "models": {name: sha256(out / "resources/models" / name) for name in MODELS},
    }
    (out / "runtime-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    files = [f for f in out.rglob("*") if f.is_file() and not any(p.startswith(".") for p in f.relative_to(out).parts)]
    print(json.dumps({"staged": str(out), "files": len(files),
                      "bytes": sum(f.stat().st_size for f in files),
                      "qt_version": manifest["qt_version"]}, indent=2))

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--qt", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", default=shutil.which("x86_64-w64-mingw32-g++-posix") or "g++")
    parser.add_argument("--objdump", default=shutil.which("x86_64-w64-mingw32-objdump") or "objdump")
    stage(parser.parse_args())
