#pragma once

#include <mg/error.h>

/* Returned file handles are owned by the caller and must be closed. */
mg_result_t file_open(const char *path, u32 flags);
/* Reads and writes may transfer fewer bytes than requested. */
mg_result_t object_read(mg_handle_t handle, void *buffer, usize length);
mg_result_t object_write(mg_handle_t handle, const void *buffer, usize length);
mg_result_t handle_close(mg_handle_t handle);

/* Allocation-free helpers for complete writes and the process console. */
mg_result_t object_write_all(mg_handle_t handle, const void *buffer, usize length);
mg_result_t console_write(const void *buffer, usize length);
mg_result_t console_write_string(const char *string);
/* Console update transaction API for atomic shell presentation. */
mg_result_t console_begin_transaction(void);
mg_result_t console_end_transaction(void);
mg_result_t console_set_secure_input(bool secure);
