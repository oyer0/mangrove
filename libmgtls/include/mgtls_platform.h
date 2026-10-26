/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/* Private wolfCrypt custom random block callback. */
int mgtls_wolfssl_random_block(unsigned char *output, unsigned int length);
