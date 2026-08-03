#pragma once

#include <mg/error.h>

/* A successful spawn returns a process handle owned by the caller. */
mg_result_t process_spawn(const char *path);
/* Wait writes the child status and leaves the process handle open. */
mg_result_t process_wait(mg_handle_t handle, i32 *status);
mg_result_t process_chdir(const char *path);
mg_result_t process_yield(void);
/* Terminates the calling process and never returns. */
void process_exit(i32 status) __attribute__((noreturn));
