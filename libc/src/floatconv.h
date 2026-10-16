/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef bool (*mg_float_put_fn)(void *context, char value);

/* kind: 0=float, 1=double, 2=long double. Returns zero on success. */
int __mg_strto_float(const char *text, char **end, int kind, void *output);

/* source_kind: 1=double, 2=long double. flags use printf's -+ #0 bits. */
bool __mg_format_float(char conversion, int source_kind, unsigned int flags,
                       int width, int precision, bool precision_set,
                       const void *value, mg_float_put_fn put, void *context);
