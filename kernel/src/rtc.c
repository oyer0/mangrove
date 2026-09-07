/* SPDX-License-Identifier: GPL-3.0-only */
#include <rtc.h>

#include <acpi.h>
#include <io.h>
#if defined(RTC_DEBUG)
#include <kprint.h>
#endif

#define CMOS_INDEX_PORT 0x70U
#define CMOS_DATA_PORT  0x71U
#define CMOS_STATUS_A   0x0aU
#define CMOS_SECONDS    0x00U
#define CMOS_MINUTES    0x02U
#define CMOS_HOURS      0x04U
#define CMOS_DAY        0x07U
#define CMOS_MONTH      0x08U
#define CMOS_YEAR       0x09U
#define CMOS_STATUS_B   0x0bU
#define CMOS_UIP        0x80U

#define RTC_UIP_RETRIES       100000U
#define RTC_STABLE_RETRIES         8U

static u8 cmos_read(u8 saved_index, u8 reg)
{
    u8 value;

    /* Bit 7 is the NMI mask.  Preserve the caller's selection/mask while
     * selecting a register and restore the original selection afterwards. */
    outb(CMOS_INDEX_PORT, (u8)((saved_index & 0x80U) | reg));
    value = inb(CMOS_DATA_PORT);
    outb(CMOS_INDEX_PORT, saved_index);
    return value;
}

static bool rtc_wait_for_update_clear(u8 saved_index)
{
    for (u32 attempt = 0; attempt < RTC_UIP_RETRIES; attempt++) {
        if (!(cmos_read(saved_index, CMOS_STATUS_A) & CMOS_UIP))
            return true;
        __asm__ volatile("pause");
    }
    return false;
}

static bool rtc_read_sample(u8 saved_index, u8 century_register,
                            bool century_valid, rtc_register_sample_t *out)
{
    if (!out || !rtc_wait_for_update_clear(saved_index))
        return false;

    out->status_b = cmos_read(saved_index, CMOS_STATUS_B);
    out->seconds = cmos_read(saved_index, CMOS_SECONDS);
    out->minutes = cmos_read(saved_index, CMOS_MINUTES);
    out->hours = cmos_read(saved_index, CMOS_HOURS);
    out->day = cmos_read(saved_index, CMOS_DAY);
    out->month = cmos_read(saved_index, CMOS_MONTH);
    out->year = cmos_read(saved_index, CMOS_YEAR);
    out->century = century_valid ? cmos_read(saved_index, century_register) : 0;
    out->century_valid = century_valid;

    /* Do not accept a sample that crossed the update boundary. */
    return rtc_wait_for_update_clear(saved_index);
}

static bool rtc_samples_equal(const rtc_register_sample_t *left,
                              const rtc_register_sample_t *right)
{
    return left && right &&
        left->seconds == right->seconds &&
        left->minutes == right->minutes &&
        left->hours == right->hours &&
        left->day == right->day &&
        left->month == right->month &&
        left->year == right->year &&
        left->century == right->century &&
        left->status_b == right->status_b &&
        left->century_valid == right->century_valid;
}

bool rtc_read_calendar(rtc_calendar_t *out)
{
    u8 saved_index;
    u8 century_register = 0;
    bool century_valid;

    if (!out)
        return false;

    saved_index = inb(CMOS_INDEX_PORT);
    century_valid = acpi_fadt_has_rtc_century(&century_register);
    for (u32 attempt = 0; attempt < RTC_STABLE_RETRIES; attempt++) {
        rtc_register_sample_t first;
        rtc_register_sample_t second;

        if (!rtc_read_sample(saved_index, century_register, century_valid,
                             &first) ||
            !rtc_read_sample(saved_index, century_register, century_valid,
                             &second))
            break;
        if (rtc_samples_equal(&first, &second)) {
            bool valid = rtc_decode_sample(&first, out);
#if defined(RTC_DEBUG)
            kprint("[RTC] B=%02x raw=%02x:%02x:%02x %02x/%02x/%02x C=%02x/%u decoded=%u\n",
                   first.status_b, first.hours, first.minutes, first.seconds,
                   first.month, first.day, first.year, first.century,
                   first.century_valid, valid);
#endif
            outb(CMOS_INDEX_PORT, saved_index);
            return valid;
        }
    }
#if defined(RTC_DEBUG)
    kprint("[RTC] no stable validated sample\n");
#endif
    outb(CMOS_INDEX_PORT, saved_index);
    return false;
}
