#pragma once

#include <mg/error.h>
#include <mg/types.h>

/* Reads one console line.  The terminating newline is consumed and a newline
 * is emitted for prompt presentation. */
mg_result_t read_console_line(char *buffer, usize capacity, bool echo);
mg_result_t read_hidden_line(char *buffer, usize capacity);
void clear_secret(void *buffer, usize size);
