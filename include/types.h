#pragma once

#ifndef __bool_true_false_are_defined
typedef _Bool bool;
#define true 1
#define false 0
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

#ifndef UINTPTR_MAX
typedef u64 uintptr_t;
#endif
