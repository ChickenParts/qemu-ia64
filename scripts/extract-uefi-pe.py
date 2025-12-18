#!/usr/bin/env python3
import argparse
import os
import struct
import sys


def align(value, alignment):
    return (value + (alignment - 1)) & ~(alignment - 1)


def read_u16_le(buf, off):
    return struct.unpack_from("<H", buf, off)[0]


def read_u32_le(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def read_u64_le(buf, off):
    return struct.unpack_from("<Q", buf, off)[0]


def parse_fv(buf, base, outdir):
    fv_len = read_u64_le(buf, base + 0x20)
    sig = read_u32_le(buf, base + 0x28)
    if sig != 0x4856465F:  # "_FVH"
        return 0
    hdr_len = read_u16_le(buf, base + 0x30)
    if hdr_len == 0 or hdr_len > fv_len:
        return 0

    end = base + fv_len
    off = align(base + hdr_len, 8)
    dumped = 0

    while off + 24 <= end:
        name = buf[off:off + 16]
        ftype = buf[off + 0x12]
        fsize = buf[off + 0x14] | (buf[off + 0x15] << 8) | (buf[off + 0x16] << 16)

        if fsize == 0 or fsize == 0xFFFFFF:
            break

        f_end = off + fsize
        if f_end > end:
            break

        # Walk sections to find UI name and PE/TE payloads.
        ui_name = None
        sec_off = off + 24
        while sec_off + 4 <= f_end:
            ssize = buf[sec_off] | (buf[sec_off + 1] << 8) | (buf[sec_off + 2] << 16)
            stype = buf[sec_off + 3]
            if ssize == 0 or sec_off + ssize > f_end:
                break
            payload = buf[sec_off + 4:sec_off + ssize]

            # UI section: UTF-16LE string.
            if stype == 0x15 and ui_name is None:
                try:
                    ui_name = payload.decode("utf-16le", errors="ignore").strip("\x00")
                except Exception:
                    ui_name = None

            # PE32 (0x10) or TE (0x12) payload.
            if stype in (0x10, 0x12):
                tag = ui_name if ui_name else name.hex()
                kind = "pe32" if stype == 0x10 else "te"
                out_name = f"{tag}.{kind}.bin"
                out_path = os.path.join(outdir, out_name)
                with open(out_path, "wb") as fp:
                    fp.write(payload)
                dumped += 1

            sec_off = align(sec_off + ssize, 4)

        off = align(f_end, 8)

    return dumped


def main():
    ap = argparse.ArgumentParser(description="Extract PE/TE sections from a UEFI FV image")
    ap.add_argument("firmware", nargs="?", default="stuff/Flash.fd")
    ap.add_argument("-o", "--outdir", default="scratch/uefi_extract")
    args = ap.parse_args()

    with open(args.firmware, "rb") as fp:
        data = fp.read()

    os.makedirs(args.outdir, exist_ok=True)
    total = 0
    # Scan for "_FVH" signature to find firmware volumes.
    sig = b"_FVH"
    for pos in range(len(data)):
        if data[pos:pos + 4] != sig:
            continue
        base = pos - 0x28
        if base < 0 or base + 0x38 > len(data):
            continue
        total += parse_fv(data, base, args.outdir)

    print(f"Extracted {total} PE/TE sections into {args.outdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
