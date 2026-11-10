#!/usr/bin/env python3
"""Check the code-page and key bundle hashes in an ad-hoc signed fat CLAP."""
import hashlib
from pathlib import Path
import plistlib
import struct
import sys

bundle = Path(sys.argv[1])
contents = bundle / "Contents"
info = (contents / "Info.plist").read_bytes()
metadata = plistlib.loads(info)
assert metadata["CFBundleIdentifier"] == "works.reconstruction.monofilterdist"
exe = contents / "MacOS" / metadata["CFBundleExecutable"]
resource = (contents / "_CodeSignature" / "CodeResources").read_bytes()
plistlib.loads(resource)
fat = exe.read_bytes()
magic, count = struct.unpack_from(">II", fat)
assert magic == 0xCAFEBABE and count == 2
seen = set()
for index in range(count):
    cpu, subtype, offset, length, alignment = struct.unpack_from(">IIIII", fat, 8 + 20 * index)
    assert offset % (1 << alignment) == 0
    b = fat[offset:offset + length]
    mhmagic, actual_cpu, actual_subtype, filetype, ncmds, _, _, _ = struct.unpack_from("<8I", b)
    assert mhmagic == 0xFEEDFACF and filetype == 8
    assert (cpu, subtype) == (actual_cpu, actual_subtype)
    seen.add(cpu)
    command_offset = 32
    sig = symbols = None
    for _ in range(ncmds):
        command, size = struct.unpack_from("<II", b, command_offset)
        if command == 0x1D:
            sig = struct.unpack_from("<II", b, command_offset + 8)
        elif command == 2:
            symbols = struct.unpack_from("<IIII", b, command_offset + 8)
        command_offset += size
    assert sig is not None and symbols is not None
    symoff, nsyms, stroff, strsize = symbols
    exported = []
    for i in range(nsyms):
        strx, typ, _, _, _ = struct.unpack_from("<IBBHQ", b, symoff + 16 * i)
        if not (typ & 0xE0) and (typ & 0x0E) == 0x0E and (typ & 1):
            name = b[stroff + strx:stroff + strsize].split(b"\0", 1)[0].decode()
            exported.append(name)
    assert exported == ["_clap_entry"], exported
    start, sigsize = sig
    super_magic, super_size, count_blobs = struct.unpack_from(">III", b, start)
    assert super_magic == 0xFADE0CC0 and super_size <= sigsize
    code_directory = None
    for j in range(count_blobs):
        slot, blob_offset = struct.unpack_from(">II", b, start + 12 + 8 * j)
        if slot == 0:
            code_directory = start + blob_offset
    assert code_directory is not None
    cd = code_directory
    cdmagic, _, _, flags, hashoff, identoff, nspecial, ncode, limit = struct.unpack_from(">9I", b, cd)
    hashsize, hashtype, _, pageshift = struct.unpack_from(">4B", b, cd + 36)
    assert cdmagic == 0xFADE0C02 and flags & 2 and hashsize == 32 and hashtype == 2
    assert nspecial >= 3
    assert b[cd + identoff:cd + identoff + len(metadata["CFBundleIdentifier"])] == metadata["CFBundleIdentifier"].encode()
    def slot_hash(n):
        return b[cd + hashoff - n * hashsize:cd + hashoff - (n - 1) * hashsize]
    assert slot_hash(1) == hashlib.sha256(info).digest()
    assert slot_hash(3) == hashlib.sha256(resource).digest()
    for j in range(ncode):
        expected = b[cd + hashoff + j * hashsize:cd + hashoff + (j + 1) * hashsize]
        actual = hashlib.sha256(b[j * (1 << pageshift):min(limit, (j + 1) * (1 << pageshift))]).digest()
        assert expected == actual, (hex(cpu), j)
    print(f"PASS cpu={hex(cpu)}: signed {ncode} code pages, Info.plist, resource seal, CLAP entry")
assert seen == {0x1000007, 0x100000C}, seen
