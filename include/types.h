/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#ifndef bool
typedef _Bool bool;
#endif
#ifndef true
#define true 1
#endif
#ifndef false
#define false 0
#endif
#ifndef __bool_true_false_are_defined
#define __bool_true_false_are_defined 1
#endif

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

/* Pointer-width arithmetic is compiler-defined; protocol fields use explicit
 * u64/i64 types instead. */
typedef __INTPTR_TYPE__ intptr_t;
typedef __UINTPTR_TYPE__ uintptr_t;
