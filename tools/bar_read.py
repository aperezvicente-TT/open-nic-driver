#!/usr/bin/env python3
"""Read 32-bit registers from a PCI BAR via /sys/bus/pci/devices/<bdf>/resourceN.

Replaces the `ernic-baremetal bar-poke` invocation the shell docs (Ch. 8 §8.2)
reference but which is not part of this repo.  Needs root.

    sudo ./tools/bar_read.py 0000:c2:00.0 0x0            # build stamp, BAR2
    sudo ./tools/bar_read.py 0000:c2:00.0 0x100000 -n 18 # plugin diag counters
    sudo ./tools/bar_read.py 0000:c2:00.0 0x0 --bar 0    # QDMA config BAR

The FPGA shell registers live in BAR2 (the 4 MB one); BAR0 (256 KB) is QDMA
config.  Note lspci lists 64-bit BARs by their first half, so the 4 MB region
shown second is BAR2 -> resource2.
"""

import argparse
import mmap
import os
import struct
import sys

PAGE = mmap.PAGESIZE


def read_words(bdf, bar, offset, count):
    path = f"/sys/bus/pci/devices/{bdf}/resource{bar}"
    if not os.path.exists(path):
        sys.exit(f"error: {path} does not exist (wrong BDF or BAR?)")

    size = os.path.getsize(path)
    span = count * 4
    if offset + span > size:
        sys.exit(f"error: offset 0x{offset:x}+{span} exceeds BAR{bar} size 0x{size:x}")

    # mmap offsets must be page-aligned; map from the aligned base and index in
    page_base = (offset // PAGE) * PAGE
    delta = offset - page_base
    length = ((delta + span + PAGE - 1) // PAGE) * PAGE

    try:
        fd = os.open(path, os.O_RDONLY | os.O_SYNC)
    except PermissionError:
        sys.exit("error: permission denied -- run under sudo")

    try:
        mm = mmap.mmap(fd, length, mmap.MAP_SHARED, mmap.PROT_READ, offset=page_base)
    except OSError as e:
        os.close(fd)
        sys.exit(f"error: mmap failed ({e}); is Memory Space Enable set?  "
                 f"Try: setpci -s {bdf} COMMAND=0x02")
    try:
        return [struct.unpack("<I", mm[delta + 4 * i: delta + 4 * i + 4])[0]
                for i in range(count)]
    finally:
        mm.close()
        os.close(fd)


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("bdf", help="PCI address, e.g. 0000:c2:00.0")
    p.add_argument("offset", help="byte offset within the BAR (hex ok)")
    p.add_argument("-n", "--count", type=int, default=1, help="number of 32-bit words")
    p.add_argument("--bar", type=int, default=2, help="BAR index (default 2 = shell regs)")
    args = p.parse_args()

    offset = int(args.offset, 0)
    if offset % 4:
        sys.exit("error: offset must be 4-byte aligned")

    words = read_words(args.bdf, args.bar, offset, args.count)
    for i, w in enumerate(words):
        print(f"BAR{args.bar} 0x{offset + 4 * i:08x} = 0x{w:08x}")

    if all(w == 0xFFFFFFFF for w in words):
        print("\nwarning: all reads returned 0xFFFFFFFF -- the BAR is likely not "
              "decoding (no driver bound / Memory Space Enable clear), or the FPGA "
              "is unconfigured.", file=sys.stderr)


if __name__ == "__main__":
    main()
