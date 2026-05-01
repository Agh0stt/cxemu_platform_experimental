#!/usr/bin/env python3
"""
bios2cxe.py — Extract the .cxe executable from a .bios package file

Usage:
  python3 bios2cxe.py <input.bios> [output.cxe]
  python3 bios2cxe.py <input.bios> --info
"""

import struct
import sys
import os

MAGIC     = b"CXEBIOS\x00"
HDR_SIZE  = 32

def parse_bios_header(data):
    if len(data) < HDR_SIZE:
        return None
    magic, version, flags, cxe_size, entry, load_addr, reserved = \
        struct.unpack_from("<8sIIIIII", data, 0)
    return {
        "magic":     magic,
        "version":   version,
        "flags":     flags,
        "cxe_size":  cxe_size,
        "entry":     entry,
        "load_addr": load_addr,
    }

def bios_to_cxe(bios_path, cxe_path=None, info_only=False):
    with open(bios_path, "rb") as f:
        data = f.read()

    hdr = parse_bios_header(data)
    if hdr is None:
        print(f"bios2cxe: error: file too small", file=sys.stderr)
        sys.exit(1)

    if hdr["magic"] != MAGIC:
        print(f"bios2cxe: error: bad magic (got {hdr['magic']})", file=sys.stderr)
        sys.exit(1)

    ver_maj = (hdr["version"] >> 16) & 0xFFFF
    ver_min =  hdr["version"]        & 0xFFFF

    print(f"bios2cxe: '{bios_path}'")
    print(f"  magic:      {hdr['magic'].rstrip(b'\\x00').decode()}")
    print(f"  version:    {ver_maj}.{ver_min}")
    print(f"  flags:      0x{hdr['flags']:08x}")
    print(f"  cxe_size:   {hdr['cxe_size']} bytes")
    print(f"  entry hint: 0x{hdr['entry']:08x}")
    print(f"  load addr:  0x{hdr['load_addr']:08x}")

    if info_only:
        return

    cxe_data = data[HDR_SIZE : HDR_SIZE + hdr["cxe_size"]]
    if len(cxe_data) != hdr["cxe_size"]:
        print(f"bios2cxe: error: truncated file (expected {hdr['cxe_size']} bytes, got {len(cxe_data)})",
              file=sys.stderr)
        sys.exit(1)

    out = cxe_path or (os.path.splitext(bios_path)[0] + ".cxe")
    with open(out, "wb") as f:
        f.write(cxe_data)
    print(f"  extracted → '{out}'")

def main():
    import argparse
    p = argparse.ArgumentParser(description="Extract .cxe from .bios package")
    p.add_argument("input",  help="input .bios file")
    p.add_argument("output", nargs="?", help="output .cxe file (default: replace .bios extension)")
    p.add_argument("--info", action="store_true", help="print header info only, don't extract")
    args = p.parse_args()

    if not os.path.exists(args.input):
        print(f"bios2cxe: error: file not found: '{args.input}'", file=sys.stderr)
        sys.exit(1)

    bios_to_cxe(args.input, args.output, info_only=args.info)

if __name__ == "__main__":
    main()
