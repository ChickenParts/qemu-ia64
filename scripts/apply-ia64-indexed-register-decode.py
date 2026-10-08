#!/usr/bin/env python3
"""Apply the IA-64 M43 indexed-register read decode repair.

The edit is deliberately fail-closed against the reviewed 75f316c source
shape. It may be checked without writing, or applied atomically after every
source and destination precondition has been validated.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one occurrence, found {count}")
    return text.replace(old, new, 1)


def c_block_span(text: str, marker: str, label: str) -> tuple[int, int]:
    count = text.count(marker)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one marker, found {count}")
    start = text.index(marker)
    opening = text.index("{", start)
    depth = 0
    in_string: str | None = None
    escaped = False
    in_line_comment = False
    in_block_comment = False

    i = opening
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""

        if in_line_comment:
            if ch == "\n":
                in_line_comment = False
            i += 1
            continue
        if in_block_comment:
            if ch == "*" and nxt == "/":
                in_block_comment = False
                i += 2
            else:
                i += 1
            continue
        if in_string is not None:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == in_string:
                in_string = None
            i += 1
            continue
        if ch == "/" and nxt == "/":
            in_line_comment = True
            i += 2
            continue
        if ch == "/" and nxt == "*":
            in_block_comment = True
            i += 2
            continue
        if ch in {'"', "'"}:
            in_string = ch
            i += 1
            continue
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return start, i + 1
        i += 1

    raise RuntimeError(f"{label}: unterminated C block")


def replace_c_block(text: str, marker: str, replacement: str, label: str) -> str:
    start, end = c_block_span(text, marker, label)
    return text[:start] + replacement + text[end:]


def remove_c_block(text: str, marker: str, label: str) -> str:
    start, end = c_block_span(text, marker, label)
    if end < len(text) and text[end] == "\n":
        end += 1
    return text[:start] + text[end:]


DECODE_H = r'''/*
 * IA-64 instruction decode helpers
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef TARGET_IA64_DECODE_H
#define TARGET_IA64_DECODE_H

#include <stdint.h>

typedef enum IA64MUnitIndexedRegister {
    IA64_MUNIT_INDEXED_NONE = 0,
    IA64_MUNIT_INDEXED_RR,
    IA64_MUNIT_INDEXED_DBR,
    IA64_MUNIT_INDEXED_IBR,
    IA64_MUNIT_INDEXED_PKR,
    IA64_MUNIT_INDEXED_PMC,
    IA64_MUNIT_INDEXED_PMD,
    IA64_MUNIT_INDEXED_MSR,
    IA64_MUNIT_INDEXED_CPUID,
} IA64MUnitIndexedRegister;

IA64MUnitIndexedRegister ia64_munit_decode_indexed_read(uint64_t insn);

#endif /* TARGET_IA64_DECODE_H */
'''

DECODE_C = r'''/*
 * IA-64 instruction decode helpers
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "decode.h"

IA64MUnitIndexedRegister ia64_munit_decode_indexed_read(uint64_t insn)
{
    uint8_t major = (insn >> 37) & 0xf;
    uint8_t x3 = (insn >> 33) & 0x7;
    uint8_t x6 = (insn >> 27) & 0x3f;

    if (major != 0x1 || x3 != 0) {
        return IA64_MUNIT_INDEXED_NONE;
    }

    switch (x6) {
    case 0x10:
        return IA64_MUNIT_INDEXED_RR;
    case 0x11:
        return IA64_MUNIT_INDEXED_DBR;
    case 0x12:
        return IA64_MUNIT_INDEXED_IBR;
    case 0x13:
        return IA64_MUNIT_INDEXED_PKR;
    case 0x14:
        return IA64_MUNIT_INDEXED_PMC;
    case 0x15:
        return IA64_MUNIT_INDEXED_PMD;
    case 0x16:
        return IA64_MUNIT_INDEXED_MSR;
    case 0x17:
        return IA64_MUNIT_INDEXED_CPUID;
    default:
        return IA64_MUNIT_INDEXED_NONE;
    }
}
'''

TEST_C = r'''/*
 * IA-64 instruction decode tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "target/ia64/decode.h"

static uint64_t munit_indexed_read(uint8_t x6)
{
    return (UINT64_C(0x1) << 37) | ((uint64_t)x6 << 27);
}

static void test_indexed_read_table(void)
{
    static const struct {
        uint8_t x6;
        IA64MUnitIndexedRegister reg;
    } cases[] = {
        { 0x10, IA64_MUNIT_INDEXED_RR },
        { 0x11, IA64_MUNIT_INDEXED_DBR },
        { 0x12, IA64_MUNIT_INDEXED_IBR },
        { 0x13, IA64_MUNIT_INDEXED_PKR },
        { 0x14, IA64_MUNIT_INDEXED_PMC },
        { 0x15, IA64_MUNIT_INDEXED_PMD },
        { 0x16, IA64_MUNIT_INDEXED_MSR },
        { 0x17, IA64_MUNIT_INDEXED_CPUID },
    };

    for (size_t i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_assert_cmpint(ia64_munit_decode_indexed_read(
                            munit_indexed_read(cases[i].x6)),
                        ==, cases[i].reg);
    }
}

static void test_firmware_dbr_read(void)
{
    /* Xen/IPF DXE: mov r5 = dbr[r3]. */
    uint64_t insn = UINT64_C(0x02088300140);

    g_assert_cmpint(ia64_munit_decode_indexed_read(insn), ==,
                    IA64_MUNIT_INDEXED_DBR);
    g_assert_cmpuint((insn >> 27) & 0x3f, ==, 0x11);
    g_assert_cmpuint((insn >> 6) & 0x7f, ==, 5);
    g_assert_cmpuint((insn >> 20) & 0x7f, ==, 3);
}

static void test_non_read_extensions_are_rejected(void)
{
    /* Only 0x10..0x17 are indexed reads in this decode class. */
    for (uint8_t x6 = 0; x6 < 0x40; x6++) {
        if (x6 >= 0x10 && x6 <= 0x17) {
            continue;
        }
        g_assert_cmpint(ia64_munit_decode_indexed_read(
                            munit_indexed_read(x6)),
                        ==, IA64_MUNIT_INDEXED_NONE);
    }
}

static void test_translation_operations_are_not_indexed_reads(void)
{
    /* ptc.l, ptc.g, ptc.ga, ptr.d, ptr.i, itr.d, and itr.i. */
    for (uint8_t x6 = 0x09; x6 <= 0x0f; x6++) {
        g_assert_cmpint(ia64_munit_decode_indexed_read(
                            munit_indexed_read(x6)),
                        ==, IA64_MUNIT_INDEXED_NONE);
    }
}

static void test_wrong_major_or_extension_rejected(void)
{
    uint64_t dbr = munit_indexed_read(0x11);

    g_assert_cmpint(ia64_munit_decode_indexed_read(
                        dbr ^ (UINT64_C(0x3) << 37)),
                    ==, IA64_MUNIT_INDEXED_NONE);
    g_assert_cmpint(ia64_munit_decode_indexed_read(
                        dbr | (UINT64_C(0x1) << 33)),
                    ==, IA64_MUNIT_INDEXED_NONE);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/ia64/decode/indexed-read-table",
                    test_indexed_read_table);
    g_test_add_func("/ia64/decode/firmware-dbr-read",
                    test_firmware_dbr_read);
    g_test_add_func("/ia64/decode/non-read-extensions",
                    test_non_read_extensions_are_rejected);
    g_test_add_func("/ia64/decode/translation-ops-not-indexed-reads",
                    test_translation_operations_are_not_indexed_reads);
    g_test_add_func("/ia64/decode/wrong-major-or-extension",
                    test_wrong_major_or_extension_rejected);

    return g_test_run();
}
'''

INDEXED_BLOCK = r'''            if (indexed_read != IA64_MUNIT_INDEXED_NONE) {
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r1 = extract64(insn, 6, 7);
                uint8_t r3 = extract64(insn, 20, 7);
                TCGv_i64 idx = tcg_temp_new_i64();
                TCGv_i64 val = tcg_temp_new_i64();

                if (r3 == 0) {
                    tcg_gen_movi_i64(idx, 0);
                } else {
                    tcg_gen_mov_i64(idx, cpu_r[r3]);
                }

                switch (indexed_read) {
                case IA64_MUNIT_INDEXED_RR:
                    gen_load_rr_reg(val, idx);
                    break;
                case IA64_MUNIT_INDEXED_DBR:
                    gen_helper_dbr_read(val, tcg_env, idx);
                    break;
                case IA64_MUNIT_INDEXED_IBR:
                    gen_helper_ibr_read(val, tcg_env, idx);
                    break;
                case IA64_MUNIT_INDEXED_PKR:
                    gen_helper_pkr_read(val, tcg_env, idx);
                    break;
                case IA64_MUNIT_INDEXED_PMC:
                    gen_helper_pmc_read(val, tcg_env, idx);
                    break;
                case IA64_MUNIT_INDEXED_PMD:
                    gen_helper_pmd_read(val, tcg_env, idx);
                    break;
                case IA64_MUNIT_INDEXED_MSR:
                    gen_helper_msr_read(val, tcg_env, idx);
                    break;
                case IA64_MUNIT_INDEXED_CPUID:
                    gen_helper_get_cpuid(val, tcg_env, idx);
                    break;
                default:
                    g_assert_not_reached();
                }

                if (r1 != 0) {
                    tcg_gen_mov_i64(cpu_r[r1], val);
                    gen_helper_gr_nat_set(tcg_env,
                                          tcg_constant_i32(r1),
                                          tcg_constant_i64(0));
                }
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }
            if (x3 == 0 && x6 == 0x6) {
                /* M42: mov msr[r3] = r2 */
                uint8_t qp = insn & 0x3f;
                TCGLabel *skip_label = gen_qp_skip(qp);
                uint8_t r2 = extract64(insn, 13, 7);
                uint8_t r3 = extract64(insn, 20, 7);
                TCGv_i64 idx = tcg_temp_new_i64();
                TCGv_i64 val = tcg_temp_new_i64();

                if (r3 == 0) {
                    tcg_gen_movi_i64(idx, 0);
                } else {
                    tcg_gen_mov_i64(idx, cpu_r[r3]);
                }
                if (r2 == 0) {
                    tcg_gen_movi_i64(val, 0);
                } else {
                    tcg_gen_mov_i64(val, cpu_r[r2]);
                }
                gen_helper_msr_write(tcg_env, idx, val);
                if (skip_label) {
                    gen_set_label(skip_label);
                }
                break;
            }'''


def prepare(root: Path) -> dict[Path, str]:
    translate_path = root / "target/ia64/translate.c"
    target_meson_path = root / "target/ia64/meson.build"
    unit_meson_path = root / "tests/unit/meson.build"
    new_paths = {
        root / "target/ia64/decode.c": DECODE_C,
        root / "target/ia64/decode.h": DECODE_H,
        root / "tests/unit/test-ia64-decode.c": TEST_C,
    }

    for path in [translate_path, target_meson_path, unit_meson_path]:
        if not path.is_file():
            raise RuntimeError(f"missing source file: {path.relative_to(root)}")
    for path in new_paths:
        if path.exists():
            raise RuntimeError(f"destination already exists: {path.relative_to(root)}")

    translate = translate_path.read_text()
    translate = replace_once(
        translate,
        '#include "cpu.h"\n',
        '#include "cpu.h"\n#include "decode.h"\n',
        "target/ia64/translate.c include",
    )
    translate = replace_once(
        translate,
        "            uint8_t x2 = (insn >> 31) & 0x3;\n"
        "            uint8_t x4 = (insn >> 27) & 0xf;\n",
        "            IA64MUnitIndexedRegister indexed_read =\n"
        "                ia64_munit_decode_indexed_read(insn);\n",
        "target/ia64/translate.c extension decode",
    )
    translate = replace_c_block(
        translate,
        "            if (x3 == 0 && x4 == 0x6) {",
        INDEXED_BLOCK,
        "target/ia64/translate.c indexed/MSR block",
    )

    obsolete_markers = [
        "            if (x3 == 0 && x6 == 0x17) {\n                /* mov r1 = cpuid[r3] */",
        "            if (x3 == 0 && x6 == 0xd) {\n                /* M43: mov r1 = pkr[r3] */",
        "            if (x3 == 0 && x6 == 0xe) {\n                /* M43: mov r1 = pmc[r3] */",
        "            if (x3 == 0 && x6 == 0xf) {\n                /* M43: mov r1 = pmd[r3] */",
        "            if (x3 == 0 && x6 == 0xb) {\n                /* M43: mov r1 = dbr[r3] */",
        "            if (x3 == 0 && x6 == 0xc) {\n                /* M43: mov r1 = ibr[r3] */",
    ]
    for marker in obsolete_markers:
        translate = remove_c_block(
            translate, marker,
            f"target/ia64/translate.c obsolete {marker.split('/*', 1)[-1].strip()}",
        )

    translate = remove_c_block(
        translate,
        "else if (x3 == 0 && x6 == 0x10) {\n                /* mov r1 = rr[r3] */",
        "target/ia64/translate.c obsolete RR read",
    )
    translate = replace_once(
        translate,
        "            }  else if (x3 == 0 && x6 == 0x1e) {",
        "            } else if (x3 == 0 && x6 == 0x1e) {",
        "target/ia64/translate.c RR chain formatting",
    )

    target_meson = target_meson_path.read_text()
    target_meson = replace_once(
        target_meson,
        "  'cpu.c',\n",
        "  'cpu.c',\n  'decode.c',\n",
        "target/ia64/meson.build",
    )

    unit_meson = unit_meson_path.read_text()
    unit_meson = replace_once(
        unit_meson,
        "  'test-fifo': [],\n",
        "  'test-fifo': [],\n"
        "  'test-ia64-decode': [meson.project_source_root() / 'target/ia64/decode.c'],\n",
        "tests/unit/meson.build",
    )

    forbidden = [
        "x6 == 0xb) {\n                /* M43: mov r1 = dbr",
        "x6 == 0xc) {\n                /* M43: mov r1 = ibr",
        "x6 == 0xd) {\n                /* M43: mov r1 = pkr",
        "x6 == 0xe) {\n                /* M43: mov r1 = pmc",
        "x6 == 0xf) {\n                /* M43: mov r1 = pmd",
    ]
    for needle in forbidden:
        if needle in translate:
            raise RuntimeError(f"translator still contains obsolete decode: {needle}")
    if translate.count("ia64_munit_decode_indexed_read(insn)") != 1:
        raise RuntimeError("translator indexed-read decoder is not present exactly once")

    updates = {
        translate_path: translate,
        target_meson_path: target_meson,
        unit_meson_path: unit_meson,
    }
    updates.update(new_paths)
    return updates


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--apply", action="store_true")
    parser.add_argument("source_root", nargs="?", default=".")
    args = parser.parse_args(argv)

    root = Path(args.source_root).resolve()
    updates = prepare(root)
    if args.apply:
        for path, content in updates.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        verb = "applied"
    else:
        verb = "would update"

    print(f"{verb} IA-64 indexed-register decode repair in {len(updates)} files")
    for path in updates:
        print(f"  {path.relative_to(root)}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except RuntimeError as exc:
        print(f"apply-ia64-indexed-register-decode.py: {exc}", file=sys.stderr)
        raise SystemExit(1)
