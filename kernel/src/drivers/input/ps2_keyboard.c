/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <drivers/input/ps2_keyboard.h>
#include <drivers/input/keyboard.h>
#include <console.h>
#include <acpi.h>
#include <io.h>
#include <ioapic.h>
#include <irq.h>
#include <lapic.h>
#include <timer.h>
#include <input_diagnostics.h>

#define KBC_STATUS 0x64U
#define KBC_DATA   0x60U
#define KBC_OBF    0x01U
#define KBC_IBF    0x02U
#define KBC_AUX    0x20U
#define KBC_ERROR  0xC0U
#define KBC_IRQ1   0x01U
#define KBC_IRQ12  0x02U
#define KBC_KBD_DISABLED 0x10U
#define KBC_TRANSLATE    0x40U
#define KBC_READ_CONFIG  0x20U
#define KBC_WRITE_CONFIG 0x60U
#define KBC_DISABLE_KBD  0xADU
#define KBC_ENABLE_KBD   0xAEU
#define KBC_TEST_KBD     0xABU
#define KBD_ACK    0xFAU
#define KBD_RESEND 0xFEU
#define PS2_POLL_LIMIT 1000000U
#define PS2_WAIT_US 100000U
#define PS2_IRQ_BUDGET 32U

/* Both receive IRQs target the BSP and share the one controller output
 * buffer. Only their handler owns decoder state after initialization. There
 * is no timer-driven PS/2 repeat producer or runtime command-response reader.
 * Hardware typematic generates repeated makes; breaks retire held keys. */
static bool active;
static bool extended;
static u8 pause_index;
static u8 modifiers;
static bool held[256];

static void publish_key(u8 key, u8 mods, bool repeat)
{
    INPUT_DIAG_INC(decoded);
    keyboard_input_key(key, mods, repeat);
}

/* IBM translated (Set 1) positions -> Mangrove's canonical HID identities.
 * The keyboard itself is explicitly put in Set 2 before translation is on. */
static const u8 set1_keys[128] = {
    [0x01]=0x29, [0x02]=0x1E, [0x03]=0x1F, [0x04]=0x20,
    [0x05]=0x21, [0x06]=0x22, [0x07]=0x23, [0x08]=0x24,
    [0x09]=0x25, [0x0A]=0x26, [0x0B]=0x27, [0x0C]=0x2D,
    [0x0D]=0x2E, [0x0E]=0x2A, [0x0F]=0x2B, [0x10]=0x14,
    [0x11]=0x1A, [0x12]=0x08, [0x13]=0x15, [0x14]=0x17,
    [0x15]=0x1C, [0x16]=0x18, [0x17]=0x0C, [0x18]=0x12,
    [0x19]=0x13, [0x1A]=0x2F, [0x1B]=0x30, [0x1C]=0x28,
    [0x1D]=0xE0, [0x1E]=0x04, [0x1F]=0x16, [0x20]=0x07,
    [0x21]=0x09, [0x22]=0x0A, [0x23]=0x0B, [0x24]=0x0D,
    [0x25]=0x0E, [0x26]=0x0F, [0x27]=0x33, [0x28]=0x34,
    [0x29]=0x35, [0x2A]=0xE1, [0x2B]=0x31, [0x2C]=0x1D,
    [0x2D]=0x1B, [0x2E]=0x06, [0x2F]=0x19, [0x30]=0x05,
    [0x31]=0x11, [0x32]=0x10, [0x33]=0x36, [0x34]=0x37,
    [0x35]=0x38, [0x36]=0xE5, [0x37]=0x55, [0x38]=0xE2,
    [0x39]=0x2C, [0x3A]=0x39, [0x3B]=0x3A, [0x3C]=0x3B,
    [0x3D]=0x3C, [0x3E]=0x3D, [0x3F]=0x3E, [0x40]=0x3F,
    [0x41]=0x40, [0x42]=0x41, [0x43]=0x42, [0x44]=0x43,
    [0x45]=0x53, [0x46]=0x47, [0x47]=0x5F, [0x48]=0x60,
    [0x49]=0x61, [0x4A]=0x56, [0x4B]=0x5C, [0x4C]=0x5D,
    [0x4D]=0x5E, [0x4E]=0x57, [0x4F]=0x59, [0x50]=0x5A,
    [0x51]=0x5B, [0x52]=0x62, [0x53]=0x63, [0x56]=0x64,
    [0x57]=0x44, [0x58]=0x45,
};

static u8 extended_key(u8 code)
{
    switch (code) {
        case 0x1C: return 0x58; /* keypad Enter */
        case 0x1D: return 0xE4;
        case 0x35: return 0x54; /* keypad slash */
        case 0x37: return 0x46; /* Print Screen (fake shift ignored) */
        case 0x38: return 0xE6;
        case 0x47: return 0x4A;
        case 0x48: return 0x52;
        case 0x49: return 0x4B;
        case 0x4B: return 0x50;
        case 0x4D: return 0x4F;
        case 0x4F: return 0x4D;
        case 0x50: return 0x51;
        case 0x51: return 0x4E;
        case 0x52: return 0x49;
        case 0x53: return 0x4C;
        case 0x5B: return 0xE3;
        case 0x5C: return 0xE7;
        case 0x5D: return 0x65;
        default: return 0;
    }
}

static void decode_byte(u8 byte)
{
    /* Pause has no break, and embeds fake Ctrl/NumLock bytes. Match its whole
     * translated sequence instead of changing either modifier/lock state. */
    static const u8 pause_tail[] = {0x1D, 0x45, 0xE1, 0x9D, 0xC5};
    if (pause_index) {
        if (byte == pause_tail[pause_index - 1U]) {
            if (++pause_index == sizeof(pause_tail) + 1U) {
                pause_index = 0;
                publish_key(0x48, modifiers, false);
            }
            return;
        }
        pause_index = 0; /* resynchronize on a malformed sequence */
    }
    if (byte == 0xE1U) { extended = false; pause_index = 1; return; }
    if (byte == 0xE0U) { extended = true; return; }
    bool release = (byte & 0x80U) != 0;
    u8 code = byte & 0x7FU;
    u8 key = extended ? extended_key(code) : set1_keys[code];
    extended = false;
    if (!key) return;
    bool repeat = held[key];
    held[key] = !release;
    if (key >= 0xE0U && key <= 0xE7U) {
        u8 mask = 1U << (key - 0xE0U);
        if (release) modifiers &= ~mask;
        else modifiers |= mask;
        return;
    }
    if (release) {
        console_discard_key_repeats(key);
        return;
    }
    publish_key(key, modifiers, repeat);
}

static void keyboard_irq(struct cpu_registers *regs)
{
    (void)regs;
#if PS2_INPUT_DIAGNOSTICS
    if (regs && regs->vec_no == IRQ_VECTOR_PS2_AUX) INPUT_DIAG_INC(irq12);
    else INPUT_DIAG_INC(irq1);
#endif
    if (!active) return;
    /* Never wait for another byte in interrupt context. Leaving an unclaimed
     * AUX byte in the shared output buffer can block all later keyboard data.
     * This controller receiver owns both IRQs; AUX has no input consumer yet.
     * Drain it without passing it to the keyboard decoder. */
    for (u32 i = 0; i < PS2_IRQ_BUDGET; i++) {
        u8 status = inb(KBC_STATUS);
        INPUT_DIAG_SET(last_status, status);
        if (status == 0xFFU || !(status & KBC_OBF)) break;
        u8 byte = inb(KBC_DATA);
        if (i + 1U == PS2_IRQ_BUDGET) INPUT_DIAG_INC(budget);
        if (status & KBC_AUX) {
            INPUT_DIAG_INC(aux_bytes); INPUT_DIAG_SET(last_aux, byte);
            continue;
        }
        INPUT_DIAG_INC(kbd_bytes); INPUT_DIAG_SET(last_kbd, byte);
        if (status & KBC_ERROR) {
            INPUT_DIAG_INC(errors);
            extended = false;
            pause_index = modifiers = 0;
            __builtin_memset(held, 0, sizeof(held));
            continue;
        }
        /* No runtime commands are outstanding. Protocol/error bytes must
         * never masquerade as key breaks or enter the console. */
        if (byte == KBD_ACK || byte == KBD_RESEND || byte == 0x00U ||
            byte == 0xFFU) {
            INPUT_DIAG_INC(protocol);
            extended = false;
            pause_index = 0;
            continue;
        }
        decode_byte(byte);
    }
}

static bool wait_input(void)
{
    timer_monotonic_deadline_t deadline;
    if (!timer_monotonic_deadline_start(&deadline, PS2_WAIT_US)) return false;
    for (u32 i = 0; i < PS2_POLL_LIMIT; i++) {
        u8 status = inb(KBC_STATUS);
        if (status == 0xFFU) return false;
        if (!(status & KBC_IBF)) return true;
        if (timer_monotonic_deadline_expired(&deadline)) break;
        __asm__ volatile("pause");
    }
    return false;
}

static bool read_reply(u8 *reply)
{
    timer_monotonic_deadline_t deadline;
    if (!timer_monotonic_deadline_start(&deadline, PS2_WAIT_US)) return false;
    for (u32 i = 0; i < PS2_POLL_LIMIT; i++) {
        u8 status = inb(KBC_STATUS);
        if (status == 0xFFU) return false;
        if (status & KBC_OBF) {
            *reply = inb(KBC_DATA);
            /* Controller/keyboard replies are not AUX. Retire unrelated
             * AUX data while retaining this transaction's bounded deadline. */
            if (status & KBC_AUX) {
                INPUT_DIAG_INC(aux_bytes); INPUT_DIAG_SET(last_aux, *reply);
                continue;
            }
            return !(status & KBC_ERROR);
        }
        if (timer_monotonic_deadline_expired(&deadline)) break;
        __asm__ volatile("pause");
    }
    return false;
}

static bool write_port(u16 port, u8 byte)
{
    if (!wait_input()) return false;
    outb(port, byte);
    return true;
}

static bool write_config(u8 config)
{
    return write_port(KBC_STATUS, KBC_WRITE_CONFIG) &&
           write_port(KBC_DATA, config);
}

static bool read_config(u8 *config)
{
    return write_port(KBC_STATUS, KBC_READ_CONFIG) && read_reply(config);
}

static bool keyboard_command(u8 command)
{
    for (u32 attempt = 0; attempt < 3U; attempt++) {
        u8 reply;
        if (!write_port(KBC_DATA, command) || !read_reply(&reply)) return false;
        if (reply == KBD_ACK) return true;
        if (reply != KBD_RESEND) return false;
    }
    return false;
}

bool ps2_keyboard_init(const char **reason)
{
    const acpi_fadt_info_t *fadt = acpi_fadt_get();
    u8 saved = 0, reply;
    bool have_config = false, registered = false, aux_registered = false;
    *reason = "i8042 absent";
    if (fadt && (fadt->hardware_reduced ||
                 (fadt->i8042_known && !fadt->i8042_present))) return false;
    if (inb(KBC_STATUS) == 0xFFU) return false;
    *reason = "i8042 clock unavailable";
    if (!timer_monotonic_ready()) return false;
    *reason = "i8042 keyboard quiesce failed";
    if (!write_port(KBC_STATUS, KBC_DISABLE_KBD)) return false;
    /* No global controller reset, AUX clock changes or mouse commands. This
     * receiver also owns unclaimed AUX output; flush stale bytes from either
     * source before issuing commands. */
    *reason = "i8042 output busy";
    for (u32 i = 0; ; i++) {
        u8 status = inb(KBC_STATUS);
        if (!(status & KBC_OBF)) break;
        if (status == 0xFFU || i == PS2_IRQ_BUDGET) goto fail;
        u8 byte = inb(KBC_DATA);
        (void)byte;
        if (status & KBC_AUX) {
            INPUT_DIAG_INC(aux_bytes); INPUT_DIAG_SET(last_aux, byte);
        }
    }
    *reason = "i8042 configuration read failed";
    if (!read_config(&saved)) goto fail;
    have_config = true;
    INPUT_DIAG_SET(cfg_before, saved);
    u8 config = saved & ~(KBC_IRQ1 | KBC_IRQ12 | KBC_TRANSLATE);
    *reason = "i8042 keyboard port test failed";
    if (!write_config(config) || !write_port(KBC_STATUS, KBC_TEST_KBD) ||
        !read_reply(&reply) || reply != 0) goto fail;
    *reason = "PS/2 keyboard command/ACK failed";
    if (!write_port(KBC_STATUS, KBC_ENABLE_KBD) ||
        !keyboard_command(0xF5) || /* disable scanning, restore defaults */
        !keyboard_command(0xF0) || !keyboard_command(0x02) ||
        !keyboard_command(0xED) || !keyboard_command(0x00)) goto fail;
    config = (saved | KBC_TRANSLATE) &
             ~(KBC_IRQ1 | KBC_IRQ12 | KBC_KBD_DISABLED);
    *reason = "i8042 translation configuration failed";
    if (!write_config(config) || !read_config(&reply) || reply != config)
        goto fail;
    *reason = "PS/2 keyboard enable/ACK failed";
    if (!keyboard_command(0xF4)) goto fail;
    *reason = "PS/2 IRQ1 registration/routing failed";
    if (!irq_register_vector(IRQ_VECTOR_PS2, keyboard_irq)) goto fail;
    registered = true;
    *reason = "PS/2 shared-buffer IRQ12 registration/routing failed";
    if (!irq_register_vector(IRQ_VECTOR_PS2_AUX, keyboard_irq)) goto fail;
    aux_registered = true;
    *reason = "PS/2 IRQ1 routing failed";
    if (!ioapic_route_gsi(acpi_irq_to_gsi(1), IRQ_VECTOR_PS2,
                          lapic_current_id(), acpi_irq_flags(1))) goto fail;
    *reason = "PS/2 shared-buffer IRQ12 routing failed";
    if (!ioapic_route_gsi(acpi_irq_to_gsi(12), IRQ_VECTOR_PS2_AUX,
                          lapic_current_id(), acpi_irq_flags(12))) goto fail;
    INPUT_DIAG_SET(gsi1, acpi_irq_to_gsi(1));
    INPUT_DIAG_SET(gsi12, acpi_irq_to_gsi(12));
    INPUT_DIAG_SET(flags1, acpi_irq_flags(1));
    INPUT_DIAG_SET(flags12, acpi_irq_flags(12));
    INPUT_DIAG_SET(apic_id, lapic_current_id());
    extended = false;
    pause_index = modifiers = 0;
    __builtin_memset(held, 0, sizeof(held));
    active = true;
    *reason = "i8042 receive IRQ enable failed";
    if (!write_config(config | KBC_IRQ1 | KBC_IRQ12)) goto fail;
    INPUT_DIAG_SET(cfg_set, config | KBC_IRQ1 | KBC_IRQ12);
    *reason = "translated Set 1 on IRQ1";
    return true;
fail:
    active = false;
    (void)write_port(KBC_STATUS, KBC_DISABLE_KBD);
    if (have_config)
        (void)write_config((saved | KBC_KBD_DISABLED) & ~KBC_IRQ1);
    if (registered) irq_unregister_vector(IRQ_VECTOR_PS2);
    if (aux_registered) irq_unregister_vector(IRQ_VECTOR_PS2_AUX);
    return false;
}
