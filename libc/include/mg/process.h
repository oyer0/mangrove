/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/error.h>
#include <mg/process_status.h>
#include <mg/session.h>

#define MG_PROCESS_MAX_ARGUMENTS 16U

/* A successful spawn returns a process handle owned by the caller. */
mg_result_t process_spawn(const char *path);
/* Spawns an already separated argv vector and inherits the caller's stdout. */
mg_result_t process_spawn_argv(const char *const *argv, usize argc);
/* Spawn with stdout inherited from a writable file handle.  A zero handle
 * keeps the normal console output.  Stdin remains the console. */
mg_result_t process_spawn_with_output(const char *path,
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
/* Terminates the calling process and never returns. */
void process_exit(i32 status) __attribute__((noreturn));
