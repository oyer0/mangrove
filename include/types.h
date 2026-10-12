/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

typedef _Bool bool;
#define true 1
#define false 0

#define PACKED __attribute__((packed))

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

typedef signed char        i8;
typedef signed short       i16;
typedef signed int         i32;
typedef signed long long   i64;

typedef u64 usize;
typedef i64 isize;

#if defined(__APPLE__)
/* Native Darwin fixtures share this name with the SDK's C headers. Target
 * cross compilers do not define __APPLE__; retain their existing type. */
typedef __UINTPTR_TYPE__ uintptr_t;
#else
typedef u64 uintptr_t;
#endif
