#!/usr/bin/env python3
"""
cxe2bios.py — Convert a .cxe executable to a .bios package file

.bios format:
  Offset  Size  Field
  0       8     magic = "CXEBIOS\0"
  8       4     version = 0x00010000
  12      4     flags   (0=regular bios)
  16      4     cxe_size  (size of embedded .cxe data)
  20      4     entry_hint (entry point vaddr from .cxe header)
  24      4     load_addr (where to load this bios in RAM, default 0x500)
  28      4     reserved = 0
  32      N     raw .cxe data

Usage:
  python3 cxe2bios.py <input.cxe> [output.bios] [--load-addr 0x500]
"""

import struct
import sys
import os

MAGIC        = b"CXEBIOS\x00"
VERSION      = 0x00010000
CXE_MAGIC    = 0x45585843  # "CXEX"

def read_cxe_entry(data):
    """Parse CxeHeader to get entry point."""
    if len(data) < 20:
        return 0
    magic, ver, flags, entry, nsect = struct.unpack_from("<IHHIH", data, 0)
    if magic != CXE_MAGIC:
        print(f"  warning: file does not start with CXE magic (got 0x{magic:08x})", file=sys.stderr)
    return entry

def cxe_to_bios(cxe_path, bios_path, load_addr=0x500, flags=0):
    with open(cxe_path, "rb") as f:
        cxe_data = f.read()

    entry = read_cxe_entry(cxe_data)
    cxe_size = len(cxe_data)

    header = struct.pack("<8sIIIIII",
        MAGIC,
        VERSION,
        flags,
        cxe_size,
        entry,
        load_addr,
        0,          # reserved
    )

    with open(bios_path, "wb") as f:
        f.write(header)
        f.write(cxe_data)

    print(f"cxe2bios: '{cxe_path}' → '{bios_path}'")
    print(f"  cxe size:   {cxe_size} bytes")
    print(f"  entry hint: 0x{entry:08x}")
    print(f"  load addr:  0x{load_addr:08x}")
    print(f"  bios size:  {len(header) + cxe_size} bytes")

def main():
    import argparse
    p = argparse.ArgumentParser(description="Convert .cxe to .bios package")
    p.add_argument("input",  help="input .cxe file")
    p.add_argument("output", nargs="?", help="output .bios file (default: replace .cxe extension)")
    p.add_argument("--load-addr", default="0x500",
                   help="RAM address to load BIOS at (default: 0x500)")
    p.add_argument("--flags", default="0", help="bios flags (default: 0)")
    args = p.parse_args()

    inp = args.input
    out = args.output or (os.path.splitext(inp)[0] + ".bios")
    load_addr = int(args.load_addr, 0)
    flags     = int(args.flags,     0)

    if not os.path.exists(inp):
        print(f"cxe2bios: error: file not found: '{inp}'", file=sys.stderr)
        sys.exit(1)

    cxe_to_bios(inp, out, load_addr, flags)

if __name__ == "__main__":
    main()
