/*
 * Minimal IA-64 EFI entry smoke test for Rooster.
 *
 * SPDX-License-Identifier: NCSA
 */

#include <efi.h>

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table)
{
    static CHAR16 banner[] = L"Rooster IA-64 EFI entry reached\r\n";

    if (image_handle == NULL || system_table == NULL ||
        system_table->ConOut == NULL ||
        system_table->ConOut->OutputString == NULL) {
        return EFI_LOAD_ERROR;
    }

    return system_table->ConOut->OutputString(system_table->ConOut, banner);
}
