#include <rtc.h>

static int expect_sample(rtc_register_sample_t sample, i32 year, u8 month,
                         u8 day, u8 hour)
{
    rtc_calendar_t output;

    if (!rtc_decode_sample(&sample, &output) || output.year != year ||
        output.month != month || output.day != day || output.hour != hour ||
        output.minute != 58U || output.second != 59U)
        return 1;
    return 0;
}

int main(void)
{
    rtc_register_sample_t bcd_24 = {
        0x59, 0x58, 0x23, 0x31, 0x12, 0x24, 0x20, 0x02, true
    };
    rtc_register_sample_t binary_24 = {
        59, 58, 23, 31, 12, 24, 20, 0x06, true
    };
    rtc_register_sample_t bcd_am = {
        0x59, 0x58, 0x12, 0x01, 0x01, 0x24, 0x20, 0x00, true
    };
    rtc_register_sample_t bcd_pm = bcd_am;
    rtc_register_sample_t invalid_day = bcd_24;

    bcd_pm.hours = 0x92;
    invalid_day.day = 0x31;
    invalid_day.month = 0x02;
    invalid_day.year = 0x23;
    if (expect_sample(bcd_24, 2024, 12, 31, 23) ||
        expect_sample(binary_24, 2024, 12, 31, 23) ||
        expect_sample(bcd_am, 2024, 1, 1, 0) ||
        expect_sample(bcd_pm, 2024, 1, 1, 12) ||
        rtc_decode_sample(&invalid_day, &(rtc_calendar_t){0}))
        return 1;
    return 0;
}
