#!/usr/bin/env python3
"""Add a UTF-8 Windows manifest to the pinned official Needle executable.

This is a resource-only transformation: all existing section bytes, native
instructions, imports and entry point are preserved. No compiler or third-party
Python packages are required. The input must have the exact official SHA256.

Usage: python vendor/tools/patch_needle_manifest.py ORIGINAL_NEEDLE_EXE OUTPUT_NEEDLE_EXE
The source is pinned at the URL in vendor/provenance/asset-provenance.json.
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import struct
import xml.etree.ElementTree as ET

ORIGINAL_SHA256 = "0d4bc1da8c51f5600f5c4f4bb4a92e0abe142fae7b69e695ccab93e6745c883d"
PROJECT = Path(__file__).resolve().parent.parent
MANIFEST = PROJECT / "native" / "needle-utf8.manifest"


def align(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def pe_sections(data: bytes):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("Not a PE image")
    optional = pe + 24
    if struct.unpack_from("<H", data, optional)[0] != 0x20B:
        raise ValueError("Expected a PE32+ x64 executable")
    count = struct.unpack_from("<H", data, pe + 6)[0]
    table = optional + struct.unpack_from("<H", data, pe + 20)[0]
    sections = []
    for index in range(count):
        at = table + index * 40
        name, virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from("<8sIIII", data, at)
        sections.append((name.rstrip(b"\0"), virtual_size, virtual_address, raw_size, raw_pointer))
    return pe, optional, table, sections


def extract_manifest(data: bytes) -> bytes:
    _, optional, _, sections = pe_sections(data)
    resource_rva, resource_size = struct.unpack_from("<II", data, optional + 112 + 16)
    section = next(s for s in sections if s[2] <= resource_rva < s[2] + s[1])
    base = section[4] + resource_rva - section[2]
    offset = 0
    for identifier in (24, 1, 0x409):
        named, ids = struct.unpack_from("<HH", data, base + offset + 12)
        entries = [struct.unpack_from("<II", data, base + offset + 16 + index * 8) for index in range(named + ids)]
        _, child = next(item for item in entries if item[0] == identifier)
        offset = child & 0x7FFFFFFF
    value_rva, size, codepage, reserved = struct.unpack_from("<IIII", data, base + offset)
    start = section[4] + value_rva - section[2]
    value = bytes(data[start:start + size])
    root = ET.fromstring(value)
    active = root.find(".//{http://schemas.microsoft.com/SMI/2019/WindowsSettings}activeCodePage")
    if active is None or active.text != "UTF-8" or codepage != 65001 or resource_size < len(value):
        raise ValueError("UTF-8 manifest verification failed")
    return value


def patch(original: bytes, manifest: bytes) -> bytes:
    if hashlib.sha256(original).hexdigest() != ORIGINAL_SHA256:
        raise ValueError("Input is not the pinned official Needle executable")
    pe, optional, table, sections = pe_sections(original)
    data = bytearray(original)
    if struct.unpack_from("<II", data, optional + 112 + 16) != (0, 0):
        raise ValueError("Refusing to replace existing resources")
    if struct.unpack_from("<II", data, optional + 112 + 32) != (0, 0):
        raise ValueError("Refusing to modify an Authenticode-signed image")
    section_alignment, file_alignment = struct.unpack_from("<II", data, optional + 32)
    header_limit = min(s[4] for s in sections if s[4])
    new_header = table + len(sections) * 40
    if new_header + 40 > header_limit or any(data[new_header:new_header + 40]):
        raise ValueError("No unused section-header slot")
    rva = align(max(s[2] + max(s[1], s[3]) for s in sections), section_alignment)
    raw_at = align(len(data), file_alignment)
    resource = bytearray(88)
    # RT_MANIFEST -> resource ID 1 -> language en-US -> IMAGE_RESOURCE_DATA_ENTRY.
    for directory, identifier, child in ((0, 24, 0x80000018), (24, 1, 0x80000030), (48, 0x409, 72)):
        struct.pack_into("<IIHHHH", resource, directory, 0, 0, 0, 0, 0, 1)
        struct.pack_into("<II", resource, directory + 16, identifier, child)
    struct.pack_into("<IIII", resource, 72, rva + 88, len(manifest), 65001, 0)
    resource.extend(manifest)
    raw_size = align(len(resource), file_alignment)
    data.extend(bytes(raw_at - len(data)))
    data.extend(resource)
    data.extend(bytes(raw_size - len(resource)))
    struct.pack_into("<8sIIIIIIHHI", data, new_header, b".rsrc\0\0\0", len(resource), rva, raw_size, raw_at, 0, 0, 0, 0, 0x40000040)
    struct.pack_into("<H", data, pe + 6, len(sections) + 1)
    initialized = struct.unpack_from("<I", data, optional + 8)[0]
    struct.pack_into("<I", data, optional + 8, initialized + raw_size)
    struct.pack_into("<I", data, optional + 56, align(rva + len(resource), section_alignment))
    struct.pack_into("<II", data, optional + 112 + 16, rva, len(resource))
    checksum_at = optional + 64
    struct.pack_into("<I", data, checksum_at, 0)
    checksum = 0
    for at in range(0, len(data), 2):
        word = data[at] | (data[at + 1] << 8 if at + 1 < len(data) else 0)
        checksum += word
        checksum = (checksum & 0xFFFF) + (checksum >> 16)
    checksum = ((checksum & 0xFFFF) + (checksum >> 16)) + len(data)
    struct.pack_into("<I", data, checksum_at, checksum)
    for name, _, _, size, start in sections:
        if data[start:start + size] != original[start:start + size]:
            raise ValueError(f"Existing {name!r} section changed")
    if extract_manifest(data) != manifest:
        raise ValueError("Extracted resource differs from supplied XML")
    return bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    arguments = parser.parse_args()
    result = patch(arguments.input.read_bytes(), MANIFEST.read_bytes())
    arguments.output.write_bytes(result)
    print("Resource-only manifest transformation verified")
    print("Original SHA256:", ORIGINAL_SHA256)
    print("Patched SHA256:", hashlib.sha256(result).hexdigest())


if __name__ == "__main__":
    main()
