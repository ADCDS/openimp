#!/usr/bin/env python3
"""Verify recovered ABI anchors in operator-supplied JA-A12 firmware artifacts.

Reads bytes only. Does not run firmware, open camera devices, or import Ghidra.
The exact hashes are required; these offsets are not a generic T23 signature.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def checked_file(path, expected):
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != expected:
        raise ValueError(f"Unexpected artifact SHA256 for {path}: {digest}")
    return data


def text_section(elf):
    if elf[:6] != b"\x7fELF\x01\x01" or struct.unpack_from("<H", elf, 18)[0] != 8:
        raise ValueError("Expected ELF32 little-endian MIPS module")
    shoff = struct.unpack_from("<I", elf, 32)[0]
    size, count, names_index = struct.unpack_from("<HHH", elf, 46)
    sections = [struct.unpack_from("<10I", elf, shoff + i * size) for i in range(count)]
    strings = sections[names_index]
    names = elf[strings[4]:strings[4] + strings[5]]
    for section in sections:
        if names[section[0]:].split(b"\0", 1)[0] == b".text":
            return section[4]
    raise ValueError("Missing .text")


def check_copy_instruction(data, offset, count):
    # addiu a2,zero,count: the actual copy length, independent of _IOC_SIZE.
    word = struct.unpack_from("<I", data, offset)[0]
    if word != 0x24060000 | count:
        raise ValueError(f"Copy-length instruction mismatch at file offset {offset:#x}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--isp", type=Path, required=True)
    args = parser.parse_args()
    ledger = json.loads((Path(__file__).resolve().parents[1] /
                         "docs/re/ja-a12-vpu-ledger.json").read_text())
    kernel = checked_file(args.kernel, ledger["kernel_sha256"])
    isp = checked_file(args.isp, "d82a86c36694f1da8e79b797a25309889433229f3bc42ffdbdfca3b382825907")
    base = int(ledger["load_base"], 16)
    table = int(ledger["dispatcher"]["table_offset"], 16)
    for i, case in enumerate(ledger["cases"]):
        if struct.unpack_from("<I", kernel, table + i * 4)[0] != int(case["entry_va"], 16):
            raise ValueError(f"Dispatch entry {i} mismatch")
        check_copy_instruction(kernel, int(case["copy_in_size_instruction"], 16) - base,
                               case["copy_from_user_bytes"])
        if case["copy_out_size_instruction"]:
            check_copy_instruction(kernel, int(case["copy_out_size_instruction"], 16) - base,
                                   case["copy_to_user_bytes"])
    text = text_section(isp)
    for offset, count in [(0x12638, 128), (0x12bf0, 8), (0x12cb4, 84), (0x12d3c, 84)]:
        check_copy_instruction(isp, text + offset, count)
    print(json.dumps({"passed": True, "kernel_sha256": ledger["kernel_sha256"],
                      "load_base": ledger["load_base"], "vpu_dispatch_entries": 11,
                      "isp_copy_anchors": 4,
                      "scope": "Exact recorded dispatch/copy anchors; not whole-driver equivalence"}, indent=2))


if __name__ == "__main__":
    main()
