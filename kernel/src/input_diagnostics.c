/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <input_diagnostics.h>
#if PS2_INPUT_DIAGNOSTICS
#include <io.h>
#include <timer.h>

input_diagnostics_t pith_input_diagnostics = {.version = 1};

static void text(char *line, u32 *at, const char *value)
{
    while (*value && *at + 1U < INPUT_DIAGNOSTIC_COLUMNS)
        line[(*at)++] = *value++;
    line[*at] = 0;
}

static void number(char *line, u32 *at, const char *label, u64 value, u32 base)
{
    char digits[21];
    u32 count = 0;
    text(line, at, label);
    do {
        digits[count++] = "0123456789abcdef"[value % base];
        value /= base;
    } while (value && count < sizeof(digits));
    while (count && *at + 1U < INPUT_DIAGNOSTIC_COLUMNS)
        line[(*at)++] = digits[--count];
    line[*at] = 0;
}

void input_diagnostics_lines(char lines[INPUT_DIAGNOSTIC_ROWS][INPUT_DIAGNOSTIC_COLUMNS])
{
    u32 at = 0;
#define GET(field) __atomic_load_n(&pith_input_diagnostics.field, __ATOMIC_RELAXED)
#define DEC(row, label, field) number(lines[row], &at, label, GET(field), 10)
#define HEX(row, label, field) number(lines[row], &at, label, GET(field), 16)
    /* Status and PIC mask reads are nondestructive. Never read port 60 or
     * issue controller commands from this observer. */
    number(lines[0], &at, "INPUT ms=", timer_uptime_ms(), 10);
    DEC(0, " IRQ1=", irq1); DEC(0, "/EOI=", eoi1);
    DEC(0, " IRQ12=", irq12); DEC(0, "/EOI=", eoi12);
    number(lines[0], &at, " ST=", GET(cfg_set) ? inb(0x64) : 0xFFU, 16);
    number(lines[0], &at, " PIC=", inb(0x21), 16);
    number(lines[0], &at, "/", inb(0xA1), 16);
    at = 0;
    DEC(1, "PS2 bytes=", kbd_bytes); DEC(1, " keys=", decoded);
    DEC(1, " AUX=", aux_bytes); DEC(1, " err=", errors);
    DEC(1, " proto=", protocol); DEC(1, " budget=", budget);
    HEX(1, " last=", last_kbd); HEX(1, " aux=", last_aux);
    at = 0;
    DEC(2, "RAW on=", raw_on); DEC(2, " put=", raw_put);
    DEC(2, " drop=", raw_drop); DEC(2, " merge=", raw_merge);
    DEC(2, " cancel=", raw_cancel); DEC(2, " get=", raw_get);
    DEC(2, " depth=", raw_depth); DEC(2, " Shoot=", shoot_get);
    at = 0;
    HEX(3, "i8042 cfg(old/set)=", cfg_before); HEX(3, "/", cfg_set);
    DEC(3, " IRQ1 GSI=", gsi1); HEX(3, " f=", flags1);
    DEC(3, " IRQ12 GSI=", gsi12); HEX(3, " f=", flags12);
    DEC(3, " CPU=", apic_id);
#undef GET
#undef DEC
#undef HEX
}
#endif
