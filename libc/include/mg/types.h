#pragma once

#include <types.h>

typedef u32 mg_handle_t;
typedef i64 mg_result_t;

#define MG_OPEN_READ  0x0001U
#define MG_OPEN_WRITE 0x0002U
#define MG_OPEN_RDWR  (MG_OPEN_READ | MG_OPEN_WRITE)
#define MG_PAGE_SIZE  4096U
#define MG_CONSOLE_HANDLE ((mg_handle_t)0x00010001U)
