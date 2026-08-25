/*
 * Relocatable QEMU IA-64 PAL gateway
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * This is deliberately tiny and project-owned.  break 0x1000 enters QEMU's
 * PAL helper; the following bundle returns to the caller through b0.
 */

        .text
        .align 16
        .global qemu_ia64_pal_gateway
        .type qemu_ia64_pal_gateway, @function
        .proc qemu_ia64_pal_gateway
qemu_ia64_pal_gateway:
        { .mii
          break.m 0x1000
          nop.i 0
          nop.i 0
        }
        { .mib
          nop.m 0
          nop.i 0
          br.ret.sptk.many b0
        }
        .endp qemu_ia64_pal_gateway
        .size qemu_ia64_pal_gateway, .-qemu_ia64_pal_gateway
