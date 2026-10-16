/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../userspace/common/table.h"

int main(void)
{
    static const char valid[] = "A\xc3\xa9\xe4\xb8\xad\xf0\x9f\x99\x82";
    static const char malformed[] = { (char)0xe2, (char)0x80, 'A', '\0' };
    usize bytes = 0;
    usize cells = 0;

    table_text_metrics_capacity(valid, sizeof(valid), &bytes, &cells);
    if (bytes != sizeof(valid) - 1U || cells != 4U) return 1;
    table_text_metrics_capacity(malformed, sizeof(malformed), &bytes, &cells);
    if (bytes != sizeof(malformed) - 1U || cells != 2U) return 2;
    return 0;
}
