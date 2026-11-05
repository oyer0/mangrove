/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/src/acpi.c"

void kprint_debug(const char *format, ...) { (void)format; }

int main(void)
{
    u8 bytes[276] = {0};
    acpi_sdt_header_t *table = (acpi_sdt_header_t *)bytes;
    table->length = sizeof(bytes);
    table->revision = 3;
    bytes[FADT_IAPC_BOOT_ARCH] = 2;
    acpi_parse_fadt(table);
    assert(fadt_info.i8042_known && fadt_info.i8042_present);
    bytes[FADT_IAPC_BOOT_ARCH] = 1; /* legacy devices alone do not mean 8042 */
    acpi_parse_fadt(table);
    assert(fadt_info.i8042_known && !fadt_info.i8042_present);
    bytes[FADT_IAPC_BOOT_ARCH] = 2;
    table->length = FADT_IAPC_BOOT_ARCH + 1;
    acpi_parse_fadt(table);
    assert(!fadt_info.i8042_known && !fadt_info.i8042_present);
    table->length = FADT_IAPC_BOOT_ARCH + 2;
    acpi_parse_fadt(table);
    assert(fadt_info.i8042_known && fadt_info.i8042_present);
    table->length = sizeof(bytes); table->revision = 1;
    acpi_parse_fadt(table);
    assert(!fadt_info.i8042_known && !fadt_info.i8042_present);
    puts("ACPI i8042: FADT presence bit, unrelated flags, short table and ACPI 1.0 reserved bytes passed");
}
