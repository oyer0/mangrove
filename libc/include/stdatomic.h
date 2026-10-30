/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/*
 * C17 atomics for Mangrove's x86-64 GCC toolchain.
 *
 * This target has lock-free scalar atomics through 64 bits, provided objects
 * have their natural alignment. These operations are compiler intrinsics and
 * do not require libatomic. Larger or aggregate atomic objects are outside the
 * supported target contract; using their operations may require a runtime
 * helper that Mangrove does not provide.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum memory_order {
    memory_order_relaxed = __ATOMIC_RELAXED,
    memory_order_consume = __ATOMIC_CONSUME,
    memory_order_acquire = __ATOMIC_ACQUIRE,
    memory_order_release = __ATOMIC_RELEASE,
    memory_order_acq_rel = __ATOMIC_ACQ_REL,
    memory_order_seq_cst = __ATOMIC_SEQ_CST
} memory_order;

typedef _Atomic(_Bool) atomic_bool;
typedef _Atomic(char) atomic_char;
typedef _Atomic(signed char) atomic_schar;
typedef _Atomic(unsigned char) atomic_uchar;
typedef _Atomic(short) atomic_short;
typedef _Atomic(unsigned short) atomic_ushort;
typedef _Atomic(int) atomic_int;
typedef _Atomic(unsigned int) atomic_uint;
typedef _Atomic(long) atomic_long;
typedef _Atomic(unsigned long) atomic_ulong;
typedef _Atomic(long long) atomic_llong;
typedef _Atomic(unsigned long long) atomic_ullong;
typedef _Atomic(__CHAR16_TYPE__) atomic_char16_t;
typedef _Atomic(__CHAR32_TYPE__) atomic_char32_t;
typedef _Atomic(__WCHAR_TYPE__) atomic_wchar_t;

typedef _Atomic(int_least8_t) atomic_int_least8_t;
typedef _Atomic(int_least16_t) atomic_int_least16_t;
typedef _Atomic(int_least32_t) atomic_int_least32_t;
typedef _Atomic(int_least64_t) atomic_int_least64_t;
typedef _Atomic(uint_least8_t) atomic_uint_least8_t;
typedef _Atomic(uint_least16_t) atomic_uint_least16_t;
typedef _Atomic(uint_least32_t) atomic_uint_least32_t;
typedef _Atomic(uint_least64_t) atomic_uint_least64_t;
typedef _Atomic(int_fast8_t) atomic_int_fast8_t;
typedef _Atomic(int_fast16_t) atomic_int_fast16_t;
typedef _Atomic(int_fast32_t) atomic_int_fast32_t;
typedef _Atomic(int_fast64_t) atomic_int_fast64_t;
typedef _Atomic(uint_fast8_t) atomic_uint_fast8_t;
typedef _Atomic(uint_fast16_t) atomic_uint_fast16_t;
typedef _Atomic(uint_fast32_t) atomic_uint_fast32_t;
typedef _Atomic(uint_fast64_t) atomic_uint_fast64_t;
typedef _Atomic(intptr_t) atomic_intptr_t;
typedef _Atomic(uintptr_t) atomic_uintptr_t;
typedef _Atomic(size_t) atomic_size_t;
typedef _Atomic(ptrdiff_t) atomic_ptrdiff_t;
typedef _Atomic(intmax_t) atomic_intmax_t;
typedef _Atomic(uintmax_t) atomic_uintmax_t;

typedef struct {
    atomic_bool __value;
} atomic_flag;

#define ATOMIC_BOOL_LOCK_FREE 2
#define ATOMIC_CHAR_LOCK_FREE 2
#define ATOMIC_CHAR16_T_LOCK_FREE 2
#define ATOMIC_CHAR32_T_LOCK_FREE 2
#define ATOMIC_WCHAR_T_LOCK_FREE 2
#define ATOMIC_SHORT_LOCK_FREE 2
#define ATOMIC_INT_LOCK_FREE 2
#define ATOMIC_LONG_LOCK_FREE 2
#define ATOMIC_LLONG_LOCK_FREE 2
#define ATOMIC_POINTER_LOCK_FREE 2

#define ATOMIC_VAR_INIT(value) (value)
#define ATOMIC_FLAG_INIT { ATOMIC_VAR_INIT(false) }

#define kill_dependency(value) (value)

#define atomic_init(object, desired) \
    __atomic_store_n((object), (desired), __ATOMIC_RELAXED)
#define atomic_is_lock_free(object) \
    __atomic_always_lock_free(sizeof(*(object)), (object))

#define atomic_thread_fence(order) __atomic_thread_fence((int)(order))
#define atomic_signal_fence(order) __atomic_signal_fence((int)(order))

#define atomic_load(object) atomic_load_explicit((object), memory_order_seq_cst)
#define atomic_load_explicit(object, order) \
    __atomic_load_n((object), (int)(order))
#define atomic_store(object, desired) \
    atomic_store_explicit((object), (desired), memory_order_seq_cst)
#define atomic_store_explicit(object, desired, order) \
    __atomic_store_n((object), (desired), (int)(order))
#define atomic_exchange(object, desired) \
    atomic_exchange_explicit((object), (desired), memory_order_seq_cst)
#define atomic_exchange_explicit(object, desired, order) \
    __atomic_exchange_n((object), (desired), (int)(order))

#define atomic_compare_exchange_weak(object, expected, desired) \
    atomic_compare_exchange_weak_explicit((object), (expected), (desired), \
        memory_order_seq_cst, memory_order_seq_cst)
#define atomic_compare_exchange_weak_explicit(object, expected, desired, \
                                               success, failure) \
    __atomic_compare_exchange_n((object), (expected), (desired), true, \
                                (int)(success), (int)(failure))
#define atomic_compare_exchange_strong(object, expected, desired) \
    atomic_compare_exchange_strong_explicit((object), (expected), (desired), \
        memory_order_seq_cst, memory_order_seq_cst)
#define atomic_compare_exchange_strong_explicit(object, expected, desired, \
                                                 success, failure) \
    __atomic_compare_exchange_n((object), (expected), (desired), false, \
                                (int)(success), (int)(failure))

#define atomic_fetch_add(object, operand) \
    atomic_fetch_add_explicit((object), (operand), memory_order_seq_cst)

/* GCC's generic __atomic_fetch_add treats a pointer operand as a byte count,
 * while C17 specifies pointer arithmetic in units of the pointed-to type.
 * Use a compare/exchange loop for pointers and retain GCC's scalar RMW for
 * integer atomics (including its defined narrow-integer behavior).  The
 * statement expressions capture each caller argument once. */
static inline int __mangrove_atomic_failure_order(memory_order order)
{
    switch (order) {
    case memory_order_consume: return __ATOMIC_CONSUME;
    case memory_order_acquire: return __ATOMIC_ACQUIRE;
    case memory_order_release: return __ATOMIC_RELAXED;
    case memory_order_acq_rel: return __ATOMIC_ACQUIRE;
    case memory_order_seq_cst: return __ATOMIC_SEQ_CST;
    case memory_order_relaxed:
    default: return __ATOMIC_RELAXED;
    }
}

#define __MANGROVE_ATOMIC_POINTER_FETCH_ADD(object, operand, order) ({ \
    __auto_type __mg_pointer_object = (object); \
    __auto_type __mg_pointer_operand = (operand); \
    memory_order __mg_pointer_order = (order); \
    __auto_type __mg_pointer_old = \
        __atomic_load_n(__mg_pointer_object, __ATOMIC_RELAXED); \
    __typeof__(__mg_pointer_old) __mg_pointer_desired; \
    do { \
        __mg_pointer_desired = __mg_pointer_old + __mg_pointer_operand; \
    } while (!__atomic_compare_exchange_n( \
        __mg_pointer_object, &__mg_pointer_old, __mg_pointer_desired, false, \
        (int)__mg_pointer_order, \
        __mangrove_atomic_failure_order(__mg_pointer_order))); \
    __mg_pointer_old; \
})

#define atomic_fetch_add_explicit(object, operand, order) ({ \
    __auto_type __mg_atomic_object = (object); \
    __auto_type __mg_atomic_operand = (operand); \
    memory_order __mg_atomic_order = (order); \
    __builtin_choose_expr( \
        __builtin_classify_type(*__mg_atomic_object) == 5, \
        __MANGROVE_ATOMIC_POINTER_FETCH_ADD( \
            __mg_atomic_object, __mg_atomic_operand, __mg_atomic_order), \
        __atomic_fetch_add(__mg_atomic_object, __mg_atomic_operand, \
                           (int)__mg_atomic_order)); \
})
#define atomic_fetch_sub(object, operand) \
    atomic_fetch_sub_explicit((object), (operand), memory_order_seq_cst)

#define __MANGROVE_ATOMIC_POINTER_FETCH_SUB(object, operand, order) ({ \
    __auto_type __mg_pointer_object = (object); \
    __auto_type __mg_pointer_operand = (operand); \
    memory_order __mg_pointer_order = (order); \
    __auto_type __mg_pointer_old = \
        __atomic_load_n(__mg_pointer_object, __ATOMIC_RELAXED); \
    __typeof__(__mg_pointer_old) __mg_pointer_desired; \
    do { \
        __mg_pointer_desired = __mg_pointer_old - __mg_pointer_operand; \
    } while (!__atomic_compare_exchange_n( \
        __mg_pointer_object, &__mg_pointer_old, __mg_pointer_desired, false, \
        (int)__mg_pointer_order, \
        __mangrove_atomic_failure_order(__mg_pointer_order))); \
    __mg_pointer_old; \
})

#define atomic_fetch_sub_explicit(object, operand, order) ({ \
    __auto_type __mg_atomic_object = (object); \
    __auto_type __mg_atomic_operand = (operand); \
    memory_order __mg_atomic_order = (order); \
    __builtin_choose_expr( \
        __builtin_classify_type(*__mg_atomic_object) == 5, \
        __MANGROVE_ATOMIC_POINTER_FETCH_SUB( \
            __mg_atomic_object, __mg_atomic_operand, __mg_atomic_order), \
        __atomic_fetch_sub(__mg_atomic_object, __mg_atomic_operand, \
                           (int)__mg_atomic_order)); \
})
#define atomic_fetch_or(object, operand) \
    atomic_fetch_or_explicit((object), (operand), memory_order_seq_cst)
#define atomic_fetch_or_explicit(object, operand, order) \
    __atomic_fetch_or((object), (operand), (int)(order))
#define atomic_fetch_xor(object, operand) \
    atomic_fetch_xor_explicit((object), (operand), memory_order_seq_cst)
#define atomic_fetch_xor_explicit(object, operand, order) \
    __atomic_fetch_xor((object), (operand), (int)(order))
#define atomic_fetch_and(object, operand) \
    atomic_fetch_and_explicit((object), (operand), memory_order_seq_cst)
#define atomic_fetch_and_explicit(object, operand, order) \
    __atomic_fetch_and((object), (operand), (int)(order))
#define atomic_fetch_nand(object, operand) \
    atomic_fetch_nand_explicit((object), (operand), memory_order_seq_cst)
#define atomic_fetch_nand_explicit(object, operand, order) \
    __atomic_fetch_nand((object), (operand), (int)(order))

static inline bool atomic_flag_test_and_set_explicit(atomic_flag *object,
                                                       memory_order order)
{
    return __atomic_test_and_set(&object->__value, (int)order);
}

static inline bool atomic_flag_test_and_set(atomic_flag *object)
{
    return atomic_flag_test_and_set_explicit(object, memory_order_seq_cst);
}

static inline void atomic_flag_clear_explicit(atomic_flag *object,
                                                memory_order order)
{
    __atomic_clear(&object->__value, (int)order);
}

static inline void atomic_flag_clear(atomic_flag *object)
{
    atomic_flag_clear_explicit(object, memory_order_seq_cst);
}
