/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
/* Darwin ld has no GNU --wrap. Intercept every malloc reference in the Reed
 * fixture and its production translation units, retaining failure injection.
 * Include libc declarations before substituting the function name. */
#include <stdlib.h>
static inline void *reed_test_real_malloc(size_t size) { return malloc(size); }
void *__wrap_malloc(size_t size);
#define __real_malloc reed_test_real_malloc
#define malloc __wrap_malloc
