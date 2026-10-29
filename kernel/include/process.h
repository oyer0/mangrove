/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>
#include <address_space.h>
#include <process_arguments.h>
#include <object.h>
#include <identity.h>
#include <terminal_output.h>
#include <mg/process_status.h>
#include <mg/session.h>
#include <mg/inspection.h>
#include <spinlock.h>
#include <mutex.h>

struct kernel_thread;
struct page_table;
struct process_memory_mapping;
struct process_user_region;
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
    PROCESS_STATE_EXITING,
    PROCESS_STATE_TERMINATED,
} process_state_t;

typedef enum {
    PROCESS_USER_REGION_INITIAL_STACK = 1,
    PROCESS_USER_REGION_WORKER_STACK,
    PROCESS_USER_REGION_THREAD_TLS,
} process_user_region_kind_t;

/* Process-owned reservation metadata. The complete reservation includes the
 * unmapped guard pages; only [mapping_base, mapping_base + mapping_pages)
 * has user leaves. release_pending records deferred disposal; revoked records
 * that new syscall leases are denied. Reservations and frames remain owned
 * until unmap and its TLB handoff complete. */
typedef struct process_user_region {
    uintptr_t reservation_base;
    usize reservation_pages;
    uintptr_t mapping_base;
    usize mapping_pages;
    process_user_region_kind_t kind;
    bool worker_owned;
    bool release_pending;
    bool revoked;
    struct process_user_region *next;
} process_user_region_t;

/* Lightweight worker completion state. The process list owns one object
 * reference; the typed thread handle owns another. Scheduler and user
 * execution resources are reclaimed independently after final handoff. */
typedef struct process_thread_record {
    kernel_object_t object;
    process_t *process;
    struct kernel_thread *execution;
    struct kernel_thread *join_waiter;
    process_user_region_t *stack_region;
    process_user_region_t *tls_region;
    struct process_thread_record *next;
    u64 id;
    u64 result;
    bool completed;
    bool kernel_stack_released;
    bool join_claimed;
    bool joined;
    bool detached;
    bool listed;
    bool reclaiming;
} process_thread_record_t;

/* A process owns userspace execution; scheduling remains thread-based. */
struct process {
    u64 pid;
    /* State is atomically observable by admitted sibling operations; all
     * transitions and related lifecycle fields remain registry-locked. */
    _Atomic process_state_t state;
    /* Protected by the process registry lock. A syscall admission retains
     * the process object until its syscall-stack leases have been released. */
    u32 active_syscalls;
    bool termination_pending;
    bool external_termination_in_progress;
    /* False while a newly created process is still being populated. Registry
     * scans and external termination treat it as in-flight construction. */
    bool launch_ready;
    process_t *parent;
    process_t *first_child;
    process_t *next_sibling;
    struct kernel_thread *main_thread;
    /* Protected by process_registry_lock. The list owns every attached
     * scheduler record and retains this process object once per thread. */
    struct kernel_thread *threads;
    u32 thread_count;
    u32 live_thread_count;
    /* Protected by process_registry_lock. Each retained scheduler handoff
     * reference is counted before its thread's handoff barrier is released,
     * then remains counted until the cleanup worker releases it safely. */
    u32 scheduler_handoff_references;
    /* Atomic subset published for the cleanup worker to release. Each such
     * reference keeps this process discoverable in all_processes. */
    _Atomic u32 scheduler_handoff_releases_pending;
    process_thread_record_t *thread_records;
    u32 worker_reapers;
    /* Atomic fast-path hint. The intrusive list remains registry-locked;
     * syscall epilogues only enter the potentially linear reaper while some
     * completed worker still owns execution resources or a consumed record. */
    bool worker_reap_pending;
    /* Protected by memory_lock and sampled atomically by syscall epilogues.
     * Provisional stack/TLS regions can outlive failed setup while a sibling
     * still holds a kernel user-memory lease. */
    _Atomic bool user_region_reap_pending;
    /* Pins unlocked walks that wake thread records. Final reap closes reader
     * admission and waits for these short walks before detaching records. */
    u32 thread_list_readers;
    bool exit_requested;
    bool exit_cleanup_in_progress;
    bool exit_cleanup_complete;
    bool thread_reap_in_progress;
    bool exit_notification_sent;
    bool wait_claimed;
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
    process_user_region_t *user_regions;
    u8 *tls_image;
    usize tls_file_size;
    usize tls_memory_size;
    usize tls_alignment;
    bool tls_present;
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
    bool shutdown_requested;
    bool shutdown_wake_sent;
    /* One-shot cooperative terminal interrupt, delivered by an interruptible
     * operation in this process. */
    volatile bool interrupt_requested;
    bool output_redirected;
    process_handle_t redirected_output_saved_handle;
    /* ESC formatting state belongs to the process that emits console bytes,
     * not to a shared inherited stdout object. */
    terminal_output_state_t terminal_output;
    /* Storage authorization state is protected by process_registry_lock.
     * A one-shot mutation grant is bound to the thread that requested it so
     * a sibling cannot spend or cancel another thread's grant. */
    u32 storage_authorized_operation;
    u64 storage_authorized_thread_id;
    /* PASS session state is process-wide, but its grant-in-progress and
     * lifetime are registry-locked and end when the process begins exiting. */
    bool storage_management_session;
    bool storage_management_authorizing;
    struct ipc_request *ipc_outstanding;
    bool owner_reference_held;
    process_t *next_all;
    /* Protects handle-table publication and slot snapshots. */
    spinlock_t handle_lock;
    /* Serializes slot mutation against table growth. Lookups retain objects
     * under handle_lock and never keep a table pointer after unlocking. */
    mutex_t handle_growth_mutex;
    u64 handle_revision;
    usize next_handle_slot;
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
/* Storage-session and one-shot mutation authority is serialized with process
 * exit under process_registry_lock. Mutation grants are single-use and bound
 * to a kernel-supplied thread identity. No helper holds the lock over I/O. */
bool process_storage_management_begin(process_t *process);
bool process_storage_management_finish(process_t *process,
                                       bool pass_authorized);
int process_storage_authorization_grant(process_t *process,
                                        u64 thread_id, u32 operation);
bool process_storage_authorization_consume(process_t *process,
                                           u64 thread_id, u32 operation);
int process_storage_authorization_cancel(process_t *process, u64 thread_id);
bool process_attach_thread(process_t *process, struct kernel_thread *thread);
/* Transfers a completed non-current thread out of its process owner list.
 * The caller must hold a process reference and may destroy the thread only
 * after this succeeds. */
bool process_detach_thread(process_t *process, struct kernel_thread *thread);
/* Complete a stopped process-owned thread that cannot be selected again. */
void process_scheduler_handoff_complete(struct kernel_thread *thread);
/* Scheduler-only variant: the outgoing record remains protected by its
 * context-switch barrier while this callback accounts completion. */
bool process_scheduler_context_handoff_complete(struct kernel_thread *thread);
/* Transfers one retained scheduler handoff reference to the process cleanup
 * worker and wakes it. This hook runs before the incoming mutex continuation
 * is consumed and must not release the reference or perform teardown. */
void process_scheduler_post_handoff(process_t *process);
/* Start the single process teardown worker after scheduler initialization. */
bool process_start_cleanup_worker(void);
/* Called when a process-owned kernel entry returns, before scheduler exit. */
void process_thread_entry_completed(struct kernel_thread *thread,
                                    i32 status);
void process_worker_exit(struct kernel_thread *thread, u64 result);
int process_thread_create(process_t *process, uintptr_t entry,
                          uintptr_t argument,
                          process_handle_t *out_handle,
                          u64 *out_thread_id);
int process_thread_join(process_t *process,
                        process_handle_t handle, u64 *out_result);
int process_thread_detach(process_t *process,
                          process_handle_t handle);
bool process_thread_handle_close_allowed(kernel_object_t *object);
u64 process_current_thread_id(void);
void process_worker_reclaim_pending(process_t *process);
/* Internal Ring 3 entry preparation used by the initial loader path and the
 * future kernel-only worker constructor. This is not a userspace syscall. */
bool process_user_context_prepare_initial(process_t *process);
void process_user_context_enter(void) __attribute__((noreturn));
/* True when this thread must leave userspace after its current syscall. */
bool process_thread_exit_requested(const struct kernel_thread *thread);
process_t *process_current(void);
u64 process_current_pid(void);
bool process_exit(process_t *process, i32 status);
/* Publishes a fully prepared process to session/shutdown scans and external
 * termination. Creators call after setup and scheduler publication. */
bool process_publish_launch(process_t *process);
bool process_syscall_enter(process_t *process);
void process_syscall_leave(process_t *process);
/* Terminates the currently executing Ring 3 process after a CPU exception.
 * On a successful scheduler handoff this does not return on the faulting
 * thread's stack. */
bool process_terminate_current_exception(i32 status);
bool process_setup_argv(process_t *process, const char *const *argv,
                        u32 argc);
bool process_user_register_initial_stack(process_t *process,
                                         uintptr_t stack_top,
                                         usize stack_pages);
/* Takes ownership of image on success; caller retains it on failure. */
bool process_user_tls_template_adopt(process_t *process, u8 *image,
                                     usize file_size, usize memory_size,
                                     usize alignment);
/* Internal preparation only: the caller must keep process syscall admission
 * through attachment and scheduler publication. It is not a userspace API. */
bool process_user_context_prepare_worker(process_t *process,
                                         struct kernel_thread *thread,
                                         uintptr_t entry, uintptr_t argument);
bool process_user_context_discard_worker(process_t *process,
                                         struct kernel_thread *thread);
int process_spawn_result(process_t *parent, const char *cmdline,
                           process_handle_t *out_handle);
/* Consumes no ownership: the caller retains and destroys the snapshot after
 * this synchronous construction has either completed or rolled back. */
int process_spawn_arguments_result(process_t *parent,
    const process_arguments_t *args, bool inherit_output,
    process_handle_t output_handle, process_handle_t *out_handle);
int process_spawn_argv_result(process_t *parent, const char *const *argv,
                               u32 argc, process_handle_t *out_handle);
int process_spawn_argv_with_output_result(
    process_t *parent, const char *const *argv, u32 argc,
    process_handle_t output_handle, process_handle_t *out_handle);
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
bool process_spawn_argv_with_context(process_t *parent,
                                const char *const *argv, u32 argc,
                                const process_credentials_t *credentials,
                                mg_session_id_t session_id,
                                bool session_shell, const char *initial_cwd,
                                process_handle_t *out_handle);
bool process_spawn_service(process_t *parent, u32 service_id,
                           process_handle_t *out_handle);
int process_spawn_service_result(process_t *parent, u32 service_id,
                                 process_handle_t *out_handle);
int process_wait(process_t *parent, process_handle_t handle,
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
bool process_shutdown_requested(const process_t *process);
bool process_take_interrupt(process_t *process);
bool process_interrupt_foreground_session(void);
bool process_shutdown_pending(void);
int process_shutdown_quiesce(process_t *requester);
void process_shutdown_abort(void);
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
/* Both successful lookup forms return a retained object reference that
 * remains valid across concurrent close. The caller must object_release()
 * after its operation. lookup_any permits querying a terminated process. */
bool process_handle_close(process_t *process, process_handle_t handle);
void process_handle_close_all(process_t *process);
const char *process_state_name(process_state_t state);
