/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>
#include <address_space.h>
#include <object.h>
#include <identity.h>
#include <mg/process_status.h>
#include <mg/session.h>
#include <mg/inspection.h>
#include <spinlock.h>
#include <mutex.h>

struct kernel_thread;
struct page_table;
struct process_memory_mapping;
struct ipc_request;
struct vfs_location;
struct vfs_node;
typedef struct process process_t;
typedef u32 process_handle_t;

#define PROCESS_NO_SESSION ((mg_session_id_t)0)

/* First installed handle in a fresh process (slot 0, generation 1). */
#define PROCESS_INITIAL_CONSOLE_HANDLE ((process_handle_t)0x00010001U)
/* Keyboard input remains separate when a child redirects stdout. */
#define PROCESS_INITIAL_STDIN_HANDLE ((process_handle_t)0x00010002U)

#define PROCESS_HANDLE_RIGHT_READ  OBJECT_RIGHT_READ
#define PROCESS_HANDLE_RIGHT_WRITE OBJECT_RIGHT_WRITE
#define PROCESS_EXECUTABLE_PATH_CAPACITY 512U

typedef enum {
    PROCESS_STATE_ACTIVE = 0,
    PROCESS_STATE_TERMINATED,
} process_state_t;

/* A process owns userspace execution; scheduling remains thread-based. */
struct process {
    u64 pid;
    process_state_t state;
    process_t *parent;
    process_t *first_child;
    process_t *next_sibling;
    struct kernel_thread *main_thread;
    struct page_table *address_space;
    uintptr_t entry_point;
    uintptr_t user_stack_top;
    phys_addr_t top_stack_frame;
    uintptr_t user_stack_sp;
    uintptr_t user_argc;
    uintptr_t user_argv;
    bool image_loaded;
    i32 exit_status;
    bool wait_collected;
    process_t *waiting_child;
    struct kernel_thread *waiting_thread;
    struct vfs_location *cwd_location;
    spinlock_t cwd_lock;
    kernel_object_t object;
    void *handle_table;
    usize handle_capacity;
    struct process_memory_mapping *memory_mappings;
    char name[32];
    char executable_path[PROCESS_EXECUTABLE_PATH_CAPACITY];
    /* One increment per scheduler timer tick while a process thread runs. */
    volatile u64 cpu_time_ticks;
    process_credentials_t credentials;
    bool credentials_initialized;
    mg_session_id_t session_id;
    bool session_shell;
    bool system_service;
    u32 service_id;
    bool output_redirected;
    process_handle_t redirected_output_saved_handle;
    /* One-shot PASS authorization for the immediately following storage
     * mutation.  It is process-local and never supplied by userspace. */
    u32 storage_authorized_operation;
    /* Set only by the diskutil executable after kernel-authenticated PASS
     * authorization.  This capability dies with that process. */
    bool storage_management_session;
    struct ipc_request *ipc_outstanding;
    bool owner_reference_held;
    process_t *next_all;
    /* Protects this process's handle-table slots.  Lookups return the
     * existing borrowed object reference, so callers retain the current
     * handle/object lifetime contract. */
    spinlock_t handle_lock;
    /* Serializes this address space's mapping interval list and VMM
     * operations that may wait for remote TLB invalidation. */
    mutex_t memory_lock;
};

bool process_init(void);
process_t *process_create(const char *name, process_t *parent,
                          struct kernel_thread *main_thread);
/* Boot diagnostics use this variant to report which prerequisite prevented
 * process construction.  The ordinary API retains its existing contract. */
process_t *process_create_with_failure(const char *name, process_t *parent,
                                       struct kernel_thread *main_thread,
                                       const char **out_stage,
                                       int *out_status);
/* Initializes the parentless session as a kernel/system process. */
bool process_assign_system_credentials(process_t *process);
bool process_assign_system_service(process_t *process, u32 service_id);
bool process_get_credentials(const process_t *process,
                             process_credentials_t *credentials);
bool process_attach_thread(process_t *process, struct kernel_thread *thread);
process_t *process_current(void);
u64 process_current_pid(void);
bool process_exit(process_t *process, i32 status);
/* Terminates the currently executing Ring 3 process after a CPU exception.
 * On a successful scheduler handoff this does not return on the faulting
 * thread's stack. */
bool process_terminate_current_exception(i32 status);
bool process_setup_cmdline(process_t *process, const char *cmdline);
int process_spawn_result(process_t *parent, const char *cmdline,
                           process_handle_t *out_handle);
int process_spawn_argv_result(process_t *parent, const char *const *argv,
                               u32 argc, process_handle_t *out_handle);
int process_spawn_with_output_result(process_t *parent, const char *cmdline,
                                       process_handle_t output_handle,
                                       process_handle_t *out_handle);
bool process_spawn(process_t *parent, const char *cmdline,
                   process_handle_t *out_handle);
/* Spawns an executable from an already separated argv vector.  The child
 * inherits the caller's current stdout object, which also preserves shell
 * redirection for wrapper utilities such as time. */
bool process_spawn_argv(process_t *parent, const char *const *argv,
                        u32 argc, process_handle_t *out_handle);
bool process_spawn_with_output(process_t *parent, const char *cmdline,
                               process_handle_t output_handle,
                               process_handle_t *out_handle);
bool process_redirect_output(process_t *process, process_handle_t output_handle,
                             process_handle_t *saved_handle);
bool process_restore_output(process_t *process, process_handle_t saved_handle);
bool process_spawn_with_context(process_t *parent, const char *cmdline,
                                const process_credentials_t *credentials,
                                mg_session_id_t session_id,
                                bool session_shell, const char *initial_cwd,
                                process_handle_t *out_handle);
bool process_spawn_service(process_t *parent, u32 service_id,
                           process_handle_t *out_handle);
int process_spawn_service_result(process_t *parent, u32 service_id,
                                 process_handle_t *out_handle);
bool process_wait(process_t *parent, process_handle_t handle,
                  i32 *out_status);
int process_poll(process_t *parent, process_handle_t handle, i32 *out_status);
int process_terminate_child(process_t *parent, process_handle_t handle,
                            i32 status);
bool process_terminate_external(process_t *process, i32 status);
bool process_terminate_session_members(mg_session_id_t session_id);
void process_reap_session_members(mg_session_id_t session_id);
u64 process_handle_pid(process_t *process, process_handle_t handle);
mg_session_id_t process_session_id(const process_t *process);
bool process_is_session_shell(const process_t *process);
/* Debug/process-diagnostics path used by scheduler/kmon-style inspection. */
void process_dump(void);
u32 process_snapshot_read(u32 offset, mg_process_info_t *output,
                          u32 capacity, u32 *out_total);
int process_lookup_path(process_t *process, const char *path,
                          struct vfs_location **out);
int process_lookup_path_n(process_t *process, const char *path,
                           usize path_length, struct vfs_location **out);
int process_lookup_path_n_access(process_t *process, const char *path,
                                  usize path_length,
                                  bool (*access)(const struct vfs_node *, void *),
                                  void *context, struct vfs_location **out);
int process_lookup_parent(process_t *process, const char *path,
                            struct vfs_location **out, char *name, usize capacity);
int process_lookup_parent_n(process_t *process, const char *path,
                             usize path_length, struct vfs_location **out,
                             char *name, usize capacity,
                             bool *out_trailing_separator);
int process_lookup_path_access(process_t *process, const char *path,
                                bool (*access)(const struct vfs_node *, void *),
                                void *context, struct vfs_location **out);
int process_lookup_parent_access(process_t *process, const char *path,
                                  bool (*access)(const struct vfs_node *, void *),
                                  void *context, struct vfs_location **out,
                                  char *name, usize capacity);
int process_lookup_parent_n_access(process_t *process, const char *path,
                                    usize path_length,
                                    bool (*access)(const struct vfs_node *, void *),
                                    void *context, struct vfs_location **out,
                                    char *name, usize capacity,
                                    bool *out_trailing_separator);
int process_cwd_path(process_t *process, char *out, usize capacity, usize *required);
int process_chdir_result(process_t *process, const char *input);
int process_chdir_result_n(process_t *process, const char *input,
                            usize input_length);
bool process_chdir(process_t *process, const char *input);
i64 process_memory_map(process_t *process, usize size,
                       uintptr_t *out_address);
i64 process_memory_unmap(process_t *process, uintptr_t address);
kernel_object_t *process_handle_lookup_any(process_t *process,
                                           process_handle_t handle,
                                           kernel_object_type_t type,
                                           u32 required_rights);
bool process_handle_install(process_t *process, kernel_object_t *object,
                            u32 rights, process_handle_t *out_handle);
/* A reservation owns object but is invisible to lookup and close. The caller
 * must activate it after success or cancel it on failure. */
bool process_handle_reserve(process_t *process, kernel_object_t *object,
                            u32 rights, process_handle_t *out_handle);
bool process_handle_activate(process_t *process, process_handle_t handle);
bool process_handle_cancel(process_t *process, process_handle_t handle);
kernel_object_t *process_handle_lookup(process_t *process,
                                       process_handle_t handle,
                                       kernel_object_type_t type,
                                       u32 required_rights);
/* Returns a retained object reference that remains valid across concurrent
 * close; the caller must object_release() it after the operation. */
kernel_object_t *process_handle_lookup_retain(process_t *process,
                                              process_handle_t handle,
                                              kernel_object_type_t type,
                                              u32 required_rights);
bool process_handle_close(process_t *process, process_handle_t handle);
void process_handle_close_all(process_t *process);
const char *process_state_name(process_state_t state);
