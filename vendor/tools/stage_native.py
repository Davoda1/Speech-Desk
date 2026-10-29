#!/usr/bin/env python3
"""Verify and copy the pinned offline speech assets from an extracted package.

This is a development/packaging tool, never an application launcher.
Python 3.11+; no network access.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile

VENDOR = Path(__file__).resolve().parents[1]


def matches(path: Path, item: dict) -> bool:
    if not path.is_file() or path.stat().st_size != item["size_bytes"]:
        return False
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest() == item["sha256"]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--from-resources", type=Path, required=True,
                        help="Existing runtime resources directory containing bin/ and models/.")
    parser.add_argument("--platform", choices=("linux", "windows"), required=True)
    parser.add_argument("--output-resources", type=Path,
                        help="Copy validated assets here. Omit for verification only.")
    parser.add_argument("--manifest", type=Path, default=VENDOR / "native-assets.json",
                        help="Trusted native-assets.json inventory; use the verified runtime inventory for rebuilt assets.")
    args = parser.parse_args()
    source = args.from_resources.resolve()
    entries = json.loads(args.manifest.read_text(encoding="utf-8"))["files"]
    entries = [e for e in entries if e["platform"] in ("all", args.platform)
               and not e["deployment_path"].endswith("whisper-cli-avx2.exe")]
    for entry in entries:
        relative = Path(entry["deployment_path"]).relative_to("resources")
        if ".." in relative.parts or relative.is_absolute():
            raise SystemExit("Unsafe asset path in manifest")
        if not matches(source / relative, entry):
            raise SystemExit(f"Missing or changed native asset: {relative}")
    if args.output_resources:
        destination = args.output_resources.resolve()
        destination.mkdir(parents=True, exist_ok=True)
        for entry in entries:
            relative = Path(entry["deployment_path"]).relative_to("resources")
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            if matches(target, entry):
                continue
            with tempfile.TemporaryDirectory(prefix=".speechdesk-stage-", dir=target.parent) as directory:
                staged = Path(directory) / target.name
                shutil.copy2(source / relative, staged)
                if not matches(staged, entry):
                    raise SystemExit(f"Copy verification failed: {relative}")
                staged.replace(target)
    print(json.dumps({"status": "verified", "files": len(entries),
                      "platform": args.platform, "copied": bool(args.output_resources)}))


if __name__ == "__main__":
    main()
