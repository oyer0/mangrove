/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/error.h>
#include <mg/process_limits.h>
#include <mg/process_status.h>
#include <mg/session.h>

/* A successful spawn returns a process handle owned by the caller. This
 * legacy command-string ABI accepts 255 input bytes plus NUL and uses its
 * historical quote parser. New callers should use the argv interfaces. */
mg_result_t process_spawn(const char *command_line);
/* Spawns separated arguments and inherits stdout. argc includes a nonempty
 * executable; argv[argc] is not read. Empty subsequent strings are preserved.
 * MG_PROCESS_ARGUMENT_BYTES_MAX bounds pointer slots plus NUL-terminated
 * strings. Input is captured synchronously; callers retain their storage. */
mg_result_t process_spawn_argv(const char *const *argv, usize argc);
/* Spawns separated argv with stdout selected from a writable file or console
 * handle. This does not change the parent's stdout or transfer handle ownership. */
mg_result_t process_spawn_argv_with_output(const char *const *argv,
                                           usize argc,
                                           mg_handle_t output_handle);
/* Legacy command-string form with stdout selected from a writable file or
 * console handle. It has the same 255-byte-plus-NUL bound and quote parser as
 * process_spawn(); new callers should use the argv variant. */
mg_result_t process_spawn_with_output(const char *command_line,
                                      mg_handle_t output_handle);
mg_result_t process_redirect_output(mg_handle_t output_handle,
                                    mg_handle_t *saved_handle);
mg_result_t process_restore_output(mg_handle_t saved_handle);
/* Wait writes the child status and leaves the process handle open. */
mg_result_t process_wait(mg_handle_t handle, i32 *status);
mg_result_t process_poll(mg_handle_t handle, i32 *status);
mg_result_t process_terminate(mg_handle_t handle, i32 status);
u64 process_handle_pid(mg_handle_t handle);
mg_result_t process_get_session_id(mg_session_id_t *session_id);
u64 process_current_pid(void);
mg_result_t process_chdir(const char *path);
mg_result_t process_yield(void);
/* Block the current thread for a relative number of monotonic milliseconds. */
mg_result_t process_sleep_ms(u64 milliseconds);
/* Terminates the calling process and never returns. */
void process_exit(i32 status) __attribute__((noreturn));
