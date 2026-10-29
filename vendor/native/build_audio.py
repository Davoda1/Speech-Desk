#!/usr/bin/env python3
"""Rebuild native SpeechDesk audio helpers; no launch-time Python is required."""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess

NATIVE = Path(__file__).resolve().parent
ROOT = NATIVE.parent
MINIAUDIO_SHA256 = "ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=("linux", "windows", "all"), default="all")
    parser.add_argument("--zig", help="Path to Zig 0.14.1 (required for the Windows cross-build).")
    parser.add_argument("--cc", default="gcc", help="Native Linux C compiler.")
    parser.add_argument("--linux-zig", action="store_true", help="Use Zig to target a glibc 2.31 Linux baseline.")
    args = parser.parse_args()
    actual = hashlib.sha256((NATIVE / "miniaudio.h").read_bytes()).hexdigest()
    if actual != MINIAUDIO_SHA256:
        raise SystemExit("Pinned miniaudio 0.11.25 source checksum does not match.")
    zig = args.zig or os.environ.get("SPEECHDESK_ZIG") or shutil.which("zig")
    common = ["-std=c11", "-O2", "-DNDEBUG", "-s", str(NATIVE / "whistle_audio.c")]
    env = os.environ.copy()
    if args.platform in ("linux", "all"):
        destination = ROOT / "engines/linux/whistle-audio"
        destination.parent.mkdir(parents=True, exist_ok=True)
        if args.linux_zig:
            if not zig:
                raise SystemExit("--linux-zig requires --zig PATH or Zig on PATH.")
            command = [zig, "cc", "-target", "x86_64-linux-gnu.2.31", *common]
        else:
            command = [args.cc, *common]
        subprocess.run([*command, "-o", str(destination), "-lm", "-ldl", "-lpthread"], check=True, env=env)
        print(f"Built {destination}")
    if args.platform in ("windows", "all"):
        if not zig:
            raise SystemExit("The Windows cross-build requires --zig PATH or Zig on PATH.")
        version = subprocess.check_output([zig, "version"], text=True).strip()
        if version != "0.14.1":
            raise SystemExit(f"Expected the pinned Zig 0.14.1 compiler, found {version}.")
        destination = ROOT / "engines/windows/whistle-audio.exe"
        destination.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([zig, "cc", "-target", "x86_64-windows-gnu", *common, "-municode", "-o", str(destination), "-lwinmm", "-lole32", "-luuid"], check=True, env=env)
        print(f"Built {destination}")


if __name__ == "__main__":
    main()
