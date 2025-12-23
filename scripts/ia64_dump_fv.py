#!/usr/bin/env python3
import argparse
import struct
import sys
from pathlib import Path


def align8(val: int) -> int:
    return (val + 7) & ~7


def align4(val: int) -> int:
    return (val + 3) & ~3


def guid_to_str(raw: bytes) -> str:
    if len(raw) != 16:
        return "<invalid>"
    d1, d2, d3 = struct.unpack_from("<IHH", raw, 0)
    d4 = raw[8:]
    return f"{d1:08x}-{d2:04x}-{d3:04x}-" + \
        "".join(f"{b:02x}" for b in d4[:2]) + "-" + \
        "".join(f"{b:02x}" for b in d4[2:])


def guid_from_str(s: str) -> bytes:
    s = s.strip().lower()
    parts = s.split("-")
    if len(parts) != 5:
        raise ValueError(f"invalid GUID: {s}")
    d1 = int(parts[0], 16)
    d2 = int(parts[1], 16)
    d3 = int(parts[2], 16)
    d4 = bytes.fromhex(parts[3] + parts[4])
    return struct.pack("<IHH", d1, d2, d3) + d4


def scan_fvs(data: bytes):
    sig = b"_FVH"
    off = 0
    while True:
        idx = data.find(sig, off)
        if idx == -1:
            break
        if idx < 0x28:
            off = idx + 1
            continue
        base = idx - 0x28
        if base + 0x38 > len(data):
            off = idx + 1
            continue
        zero = data[base:base + 16]
        fs_guid = data[base + 16:base + 32]
        fv_len, = struct.unpack_from("<Q", data, base + 0x20)
        hdr_len, = struct.unpack_from("<H", data, base + 0x30)
        if fv_len == 0 or hdr_len == 0:
            off = idx + 1
            continue
        if base + fv_len > len(data):
            off = idx + 1
            continue
        yield {
            "base": base,
            "len": fv_len,
            "hdr_len": hdr_len,
            "fs_guid": fs_guid,
        }
        off = base + fv_len


def parse_fv_at(data: bytes, base: int, limit: int):
    if base + 0x38 > len(data):
        return None
    if base + 0x28 + 4 > len(data):
        return None
    if data[base + 0x28:base + 0x2c] != b"_FVH":
        return None
    fs_guid = data[base + 16:base + 32]
    fv_len, = struct.unpack_from("<Q", data, base + 0x20)
    hdr_len, = struct.unpack_from("<H", data, base + 0x30)
    if fv_len == 0 or hdr_len == 0:
        return None
    if fv_len > limit:
        return None
    if base + fv_len > len(data):
        return None
    return {
        "base": base,
        "len": fv_len,
        "hdr_len": hdr_len,
        "fs_guid": fs_guid,
    }


def iter_ffs(data: bytes, fv):
    off = align8(fv["base"] + fv["hdr_len"])
    end = fv["base"] + fv["len"]
    while off + 24 <= end:
        hdr = data[off:off + 24]
        name = hdr[0:16]
        ftype = hdr[18]
        attrs = hdr[19]
        size = hdr[20] | (hdr[21] << 8) | (hdr[22] << 16)
        state = hdr[23]
        if size == 0 or size == 0xFFFFFF:
            break
        file_end = off + size
        if file_end > end:
            break
        yield {
            "off": off,
            "size": size,
            "type": ftype,
            "attrs": attrs,
            "state": state,
            "name": name,
        }
        off = align8(file_end)


def iter_sections(data: bytes, ffs):
    off = ffs["off"] + 24
    end = ffs["off"] + ffs["size"]
    while off + 4 <= end:
        size = data[off] | (data[off + 1] << 8) | (data[off + 2] << 16)
        stype = data[off + 3]
        if size < 4:
            break
        sec_end = off + size
        if sec_end > end:
            break
        yield {"off": off, "size": size, "type": stype}
        off = align4(sec_end)


def decode_ui_string(data: bytes) -> str:
    if not data:
        return ""
    try:
        text = data.decode("utf-16le", errors="ignore")
        text = text.split("\x00", 1)[0]
        return text
    except Exception:
        return ""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--flash", default="stuff/Flash.fd")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--show-sections", action="store_true")
    ap.add_argument("--dump-depex", action="store_true")
    ap.add_argument("--scan-nested", action="store_true")
    ap.add_argument("--find-guid", default="")
    ap.add_argument("--find-offset", default="")
    ap.add_argument("--base", default="0xff600000")
    args = ap.parse_args()

    data = Path(args.flash).read_bytes()
    base_addr = int(args.base, 0)

    target_guid = None
    if args.find_guid:
        target_guid = guid_from_str(args.find_guid)

    target_off = None
    if args.find_offset:
        target_off = int(args.find_offset, 0)
        if target_off >= base_addr:
            target_off -= base_addr

    for fv_idx, fv in enumerate(scan_fvs(data)):
        fv_addr = base_addr + fv["base"]
        if args.list:
            print(f"FV {fv_idx}: off=0x{fv['base']:x} addr=0x{fv_addr:x} "
                  f"len=0x{fv['len']:x} hdr=0x{fv['hdr_len']:x} "
                  f"fs={guid_to_str(fv['fs_guid'])}")
        for ffs in iter_ffs(data, fv):
            faddr = base_addr + ffs["off"]
            if args.list:
                print(f"  FFS off=0x{ffs['off']:x} addr=0x{faddr:x} "
                      f"size=0x{ffs['size']:x} type=0x{ffs['type']:02x} "
                      f"state=0x{ffs['state']:02x} "
                      f"name={guid_to_str(ffs['name'])}")
                if args.show_sections:
                    for sec in iter_sections(data, ffs):
                        print(f"    SEC off=0x{sec['off']:x} "
                              f"size=0x{sec['size']:x} type=0x{sec['type']:02x}")
                        if sec["type"] == 0x15:
                            ui = decode_ui_string(
                                data[sec["off"] + 4:sec["off"] + sec["size"]])
                            if ui:
                                print(f"      UI: {ui}")
                        if args.scan_nested and sec["type"] == 0x17:
                            nested = parse_fv_at(
                                data, sec["off"] + 4, sec["size"] - 4)
                            if nested:
                                naddr = base_addr + nested["base"]
                                print(f"      NESTED FV off=0x{nested['base']:x} "
                                      f"addr=0x{naddr:x} len=0x{nested['len']:x} "
                                      f"hdr=0x{nested['hdr_len']:x} "
                                      f"fs={guid_to_str(nested['fs_guid'])}")
                                for nffs in iter_ffs(data, nested):
                                    naddr = base_addr + nffs["off"]
                                    print(f"        FFS off=0x{nffs['off']:x} "
                                          f"addr=0x{naddr:x} "
                                          f"size=0x{nffs['size']:x} "
                                          f"type=0x{nffs['type']:02x} "
                                          f"name={guid_to_str(nffs['name'])}")
            if target_guid and target_guid in data[ffs["off"]:ffs["off"] + ffs["size"]]:
                print("GUID match:",
                      f"fv_off=0x{fv['base']:x} ffs_off=0x{ffs['off']:x} "
                      f"ffs_guid={guid_to_str(ffs['name'])} "
                      f"type=0x{ffs['type']:02x}")
                if args.show_sections:
                    for sec in iter_sections(data, ffs):
                        print(f"  SEC off=0x{sec['off']:x} "
                              f"size=0x{sec['size']:x} type=0x{sec['type']:02x}")
                        if sec["type"] == 0x15:
                            ui = decode_ui_string(
                                data[sec["off"] + 4:sec["off"] + sec["size"]])
                            if ui:
                                print(f"    UI: {ui}")
                        if args.dump_depex and sec["type"] == 0x1B:
                            sec_data = data[sec["off"] + 4:sec["off"] + sec["size"]]
                            hexline = " ".join(f"{b:02x}" for b in sec_data[:64])
                            print(f"    DEPEX {len(sec_data)} bytes: {hexline}")
            if target_off is not None:
                if ffs["off"] <= target_off < ffs["off"] + ffs["size"]:
                    print("Offset owner:",
                          f"fv_off=0x{fv['base']:x} ffs_off=0x{ffs['off']:x} "
                          f"ffs_guid={guid_to_str(ffs['name'])} "
                          f"type=0x{ffs['type']:02x}")
                    if args.show_sections:
                        for sec in iter_sections(data, ffs):
                            print(f"  SEC off=0x{sec['off']:x} "
                                  f"size=0x{sec['size']:x} type=0x{sec['type']:02x}")
                            if sec["type"] == 0x15:
                                ui = decode_ui_string(
                                    data[sec["off"] + 4:sec["off"] + sec["size"]])
                                if ui:
                                    print(f"    UI: {ui}")
                            if args.dump_depex and sec["type"] == 0x1B:
                                sec_data = data[sec["off"] + 4:sec["off"] + sec["size"]]
                                hexline = " ".join(f"{b:02x}" for b in sec_data[:64])
                                print(f"    DEPEX {len(sec_data)} bytes: {hexline}")
                    target_off = None
        if target_off is None and target_guid:
            pass

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
