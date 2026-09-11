/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <process.h>
#include <scheduler.h>
#include <heap.h>
#include <string.h>
#include <vmm.h>
#include <pmm.h>
#include <elf_loader.h>
#include <vfs.h>
#include <mangrove_errors.h>
#include <kprint.h>
#include <console.h>
#include <terminal.h>
#include <service.h>
#include <session.h>
#include <ipc.h>
#include <mg/service.h>
#include <spinlock.h>
#include <panic.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

#define PROCESS_MAX_ARGUMENTS 16U
#define PROCESS_COMMAND_LINE_CAPACITY 256U

static u64 next_pid;
static process_t *all_processes;
static spinlock_t process_registry_lock;

extern void ring3_enter(uintptr_t entry, uintptr_t stack_pointer, uintptr_t argc, uintptr_t argv);

typedef struct {
    kernel_object_t *object;
    u32 rights;
    u16 generation;
    bool active;
} process_handle_slot_t;

typedef struct process_memory_mapping {
    uintptr_t address;
    usize page_count;
    struct process_memory_mapping *next;
} process_memory_mapping_t;

#define PROCESS_HANDLE_SLOTS 16U
#define HANDLE_INDEX_MASK 0xffffU

static void process_memory_release_all(process_t *process);
static void process_unlink(process_t *process);
static void process_unlink_all(process_t *process);
static void process_unlink_locked(process_t *process);
static void process_unlink_all_locked(process_t *process);
static kernel_object_t *process_handle_lookup_reference(
    process_t *process, process_handle_t handle,
    kernel_object_type_t type, u32 required_rights);

/* Waiting has the same check-to-sleep handoff as IPC.  The registry lock is
 * released before scheduler_block(), but local interrupts remain masked until
 * the waiter has been registered or the failed handoff has been cleaned up. */
static u64 process_cli_save(void)
{
    u64 flags;

    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void process_cli_restore(u64 flags)
{
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory");
}

static process_handle_slot_t *handle_slots(process_t *process)
{
    return (process_handle_slot_t *)process->handle_table;
}

static process_handle_t encode_handle(u32 index, u16 generation)
{
    return ((process_handle_t)generation << 16) | (index + 1U);
}

static bool decode_handle(process_handle_t handle, u32 *index, u16 *generation)
{
    u32 slot = handle & HANDLE_INDEX_MASK;
    if (!slot || !index || !generation) return false;
    *index = slot - 1U;
    *generation = (u16)(handle >> 16);
    return *generation != 0;
}

static process_t *object_process(kernel_object_t *object)
{
    return (process_t *)((u8 *)object - __builtin_offsetof(process_t, object));
}

static void process_object_destroy(kernel_object_t *object)
{
    process_t *process = object_process(object);
    process_t *child;
    process_t *next;

    process_unlink_all(process);
    /* A parent may be reaped before a detached/session child.  Never leave
     * children pointing into the soon-to-be-freed process object. */
    {
        u64 flags = spin_lock_irqsave(&process_registry_lock);
        child = process->first_child;
        while (child) {
            next = child->next_sibling;
            child->parent = NULL;
            child->next_sibling = NULL;
            child = next;
        }
        process->first_child = NULL;
        spin_unlock_irqrestore(&process_registry_lock, flags);
    }
    if (process->main_thread && process->main_thread != thread_current()) {
        (void)thread_destroy(process->main_thread);
    }
    process_memory_release_all(process);
    vmm_destroy_address_space(process->address_space);
    kfree(process->handle_table);
    kfree(process);
}

bool process_init(void)
{
    spinlock_init(&process_registry_lock);
    next_pid = 1;
    all_processes = NULL;
    return true;
}

process_t *process_create(const char *name, process_t *parent,
                          struct kernel_thread *main_thread)
{
    process_t *process;

    if (!name || !main_thread) {
        return NULL;
    }

    process = (process_t *)kmalloc(sizeof(*process));
    if (!process) {
        return NULL;
    }
    memset(process, 0, sizeof(*process));
    spinlock_init(&process->handle_lock);
    mutex_init(&process->memory_lock);
    object_init(&process->object, OBJECT_TYPE_PROCESS,
                process_object_destroy);
    process->handle_capacity = PROCESS_HANDLE_SLOTS;
    process->handle_table = kmalloc(sizeof(process_handle_slot_t) *
                                    process->handle_capacity);
    if (!process->handle_table) {
        kfree(process);
        return NULL;
    }
    memset(process->handle_table, 0,
           sizeof(process_handle_slot_t) * process->handle_capacity);
    process->address_space = vmm_create_address_space();
    if (!process->address_space) {
        kfree(process->handle_table);
        kfree(process);
        return NULL;
    }
    process->pid = 0;
    process->state = PROCESS_STATE_ACTIVE;
    process->parent = parent;
    process->credentials = identity_system_credentials();
    process->credentials_initialized = false;
    process->session_id = parent ? parent->session_id : PROCESS_NO_SESSION;
    process->session_shell = false;
    process->owner_reference_held = true;
    if (parent && parent->credentials_initialized &&
        identity_credentials_effective(&parent->credentials,
                                       &process->credentials)) {
        process->credentials_initialized = true;
    }
    process->main_thread = main_thread;
    strncpy(process->name, name, sizeof(process->name) - 1);
    process->name[sizeof(process->name) - 1] = '\0';
    if (parent) {
        strncpy(process->cwd, parent->cwd, sizeof(process->cwd) - 1);
    } else {
        strncpy(process->cwd, "/", sizeof(process->cwd) - 1);
    }
    process->cwd[sizeof(process->cwd) - 1] = '\0';

    {
        u64 flags = spin_lock_irqsave(&process_registry_lock);
        if (next_pid == 0 || main_thread->process ||
            (parent && parent->state != PROCESS_STATE_ACTIVE)) {
            spin_unlock_irqrestore(&process_registry_lock, flags);
            vmm_destroy_address_space(process->address_space);
            kfree(process->handle_table);
            kfree(process);
            return NULL;
        }
        process->pid = next_pid++;
        if (parent) {
            process->next_sibling = parent->first_child;
            parent->first_child = process;
        }
        process->next_all = all_processes;
        all_processes = process;
        main_thread->process = process;
        spin_unlock_irqrestore(&process_registry_lock, flags);
    }
    return process;
}

bool process_assign_system_credentials(process_t *process)
{
    if (!process || process->state != PROCESS_STATE_ACTIVE || process->parent ||
        process->credentials_initialized) return false;
    process->credentials = identity_system_credentials();
    process->credentials_initialized = true;
    return true;
}

bool process_assign_system_service(process_t *process, u32 service_id)
{
    const kernel_service_definition_t *definition;
    process_credentials_t credentials;

    if (!process || process->state != PROCESS_STATE_ACTIVE || process->parent ||
        process->credentials_initialized ||
        !service_definition_lookup(service_id, &definition) || !definition)
        return false;
    credentials = identity_system_credentials();
    credentials.service_privileges = definition->privileges;
    if (!identity_credentials_valid(&credentials)) return false;
    process->credentials = credentials;
    process->credentials_initialized = true;
    process->system_service = true;
    process->service_id = service_id;
    return true;
}

bool process_get_credentials(const process_t *process,
                             process_credentials_t *credentials)
{
    if (!process || !credentials || process->state != PROCESS_STATE_ACTIVE ||
        !process->credentials_initialized ||
        !identity_credentials_valid(&process->credentials)) {
        return false;
    }
    return identity_credentials_effective(&process->credentials, credentials);
}

bool process_attach_thread(process_t *process, struct kernel_thread *thread)
{
    if (!process || !thread || process->state != PROCESS_STATE_ACTIVE ||
        (thread->process && thread->process != process)) {
        return false;
    }
    thread->process = process;
    if (!process->main_thread) {
        process->main_thread = thread;
    }
    return true;
}

process_t *process_current(void)
{
    kernel_thread_t *thread = thread_current();
    return thread ? thread->process : NULL;
}

u64 process_current_pid(void)
{
    process_t *process = process_current();

    return process ? process->pid : 0;
}

bool process_resolve_path(process_t *process, const char *input,
                          char *output, usize output_size)
{
    if (!process || process->state != PROCESS_STATE_ACTIVE || !input ||
        !output || output_size < 2 || strlen(input) >= 256) {
        return false;
    }
    if (vfs_resolve_path(process->cwd, input, output, output_size) != VFS_OK ||
        output[0] != '/') {
        return false;
    }
    return strlen(output) < output_size;
}

int process_chdir_result(process_t *process, const char *input)
{
    char path[512];
    vfs_node_t *node = NULL;

    if (!process_resolve_path(process, input, path, sizeof(path))) {
        return VFS_ERR_INVALID_PARAM;
    }
    int res = vfs_lookup(path, &node);
    if (res != VFS_OK || !node) {
        return res;
    }
    if (node->type != VFS_TYPE_DIRECTORY) {
        return VFS_ERR_NOT_FOUND;
    }
    if (!vfs_check_access(node, VFS_ACCESS_READ)) {
        return VFS_ERR_ACCESS_DENIED;
    }
    strncpy(process->cwd, path, sizeof(process->cwd) - 1);
    process->cwd[sizeof(process->cwd) - 1] = '\0';
    return VFS_OK;
}

bool process_chdir(process_t *process, const char *input)
{
    return process_chdir_result(process, input) == VFS_OK;
}

bool process_any_cwd_under_path(const char *path)
{
    process_t *process;
    usize length;

    if (!path || path[0] != '/') return false;
    length = strlen(path);
    if (!length || (length > 1U && path[length - 1U] == '/')) return false;

    u64 flags = spin_lock_irqsave(&process_registry_lock);
    for (process = all_processes; process; process = process->next_all) {
        if (process->state != PROCESS_STATE_ACTIVE) continue;
        if (strcmp(process->cwd, path) == 0) {
            spin_unlock_irqrestore(&process_registry_lock, flags);
            return true;
        }
        if (length < sizeof(process->cwd) - 1U &&
            strncmp(process->cwd, path, length) == 0 &&
            process->cwd[length] == '/') {
            spin_unlock_irqrestore(&process_registry_lock, flags);
            return true;
        }
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);
    return false;
}

bool process_split_path(process_t *process, const char *input,
                        char *parent, usize parent_size,
                        char *name, usize name_size)
{
    char resolved[512];
    char *slash;
    usize name_length;

    if (!process || !input || !parent || !name || parent_size < 2 ||
        name_size < 2 || !process_resolve_path(process, input, resolved,
                                                sizeof(resolved)) ||
        strcmp(resolved, "/") == 0) {
        return false;
    }
    slash = resolved + strlen(resolved);
    while (slash > resolved && *slash != '/') slash--;
    if (*slash != '/' || slash[1] == '\0') return false;
    name_length = strlen(slash + 1);
    if (name_length >= name_size) return false;
    memcpy(name, slash + 1, name_length + 1);
    if (slash == resolved) {
        if (parent_size < 2) return false;
        parent[0] = '/';
        parent[1] = '\0';
    } else {
        usize parent_length = (usize)(slash - resolved);
        if (parent_length >= parent_size) return false;
        memcpy(parent, resolved, parent_length);
        parent[parent_length] = '\0';
    }
    return true;
}

static bool process_memory_size_to_pages(usize size, usize *out_pages)
{
    if (!size || !out_pages || size > ~(usize)0 - (VMM_PAGE_SIZE - 1)) {
        return false;
    }
    *out_pages = (size + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
    return *out_pages != 0;
}

static uintptr_t process_memory_find_space(process_t *process,
                                           usize page_count)
{
    process_memory_mapping_t *mapping;
    uintptr_t candidate = VMM_USER_ANON_BASE;
    usize request_bytes;

    if (!process || page_count >
        (VMM_USER_ANON_LIMIT - VMM_USER_ANON_BASE) / VMM_PAGE_SIZE) {
        return 0;
    }
    request_bytes = page_count * VMM_PAGE_SIZE;
    mapping = (process_memory_mapping_t *)process->memory_mappings;
    while (mapping) {
        if (candidate <= mapping->address &&
            mapping->address - candidate >= request_bytes) {
            return candidate;
        }
        if (mapping->page_count > ~(usize)0 / VMM_PAGE_SIZE) return 0;
        usize mapping_bytes = mapping->page_count * VMM_PAGE_SIZE;
        if (mapping->address > VMM_USER_ANON_LIMIT - mapping_bytes) return 0;
        candidate = mapping->address + mapping_bytes;
        if (candidate > VMM_USER_ANON_LIMIT ||
            VMM_USER_ANON_LIMIT - candidate < request_bytes) {
            return 0;
        }
        mapping = mapping->next;
    }
    return VMM_USER_ANON_LIMIT - candidate >= request_bytes ? candidate : 0;
}

static bool process_memory_release_mapping(process_t *process,
                                           process_memory_mapping_t *mapping)
{
    bool released = true;
    usize i;

    if (!process || !mapping) return false;
    for (i = 0; i < mapping->page_count; i++) {
        phys_addr_t frame;
        uintptr_t address = mapping->address + i * VMM_PAGE_SIZE;
        if (!vmm_unmap_user_page(process->address_space,
                                 (void *)address, &frame)) {
            released = false;
            continue;
        }
        pmm_free_frame(frame);
    }
    return released;
}

static void process_memory_release_all(process_t *process)
{
    process_memory_mapping_t *mapping;

    if (!process) return;
    if (!mutex_lock(&process->memory_lock))
        panic("process: failed to lock address-space mappings");
    mapping = (process_memory_mapping_t *)process->memory_mappings;
    process->memory_mappings = NULL;
    mutex_unlock(&process->memory_lock);
    while (mapping) {
        process_memory_mapping_t *next = mapping->next;
        (void)process_memory_release_mapping(process, mapping);
        kfree(mapping);
        mapping = next;
    }
}

i64 process_memory_map(process_t *process, usize size,
                       uintptr_t *out_address)
{
    process_memory_mapping_t *mapping;
    process_memory_mapping_t **cursor;
    uintptr_t address;
    phys_addr_t *frames;
    usize page_count;
    usize i;

    if (!process || process->state != PROCESS_STATE_ACTIVE || !out_address ||
        !process_memory_size_to_pages(size, &page_count)) {
        return MG_ERR_BAD_ARGUMENT;
    }
    mapping = (process_memory_mapping_t *)kmalloc(sizeof(*mapping));
    if (!mapping) return MG_ERR_NO_MEMORY;
    if (page_count > ~(usize)0 / sizeof(*frames)) {
        kfree(mapping);
        return MG_ERR_BAD_ARGUMENT;
    }
    frames = (phys_addr_t *)kmalloc(page_count * sizeof(*frames));
    if (!frames) {
        kfree(mapping);
        return MG_ERR_NO_MEMORY;
    }
    mapping->page_count = page_count;
    mapping->next = NULL;

    /* Acquire physical pages before taking the mapping-list lock.  The VMM
     * user mapper may allocate page-table ownership metadata through the
     * heap, so keeping PMM acquisition outside this lock avoids a PMM -> VMM
     * metadata inversion. */
    for (i = 0; i < page_count; i++) {
        frames[i] = pmm_alloc_frame();
        if (!frames[i]) {
            while (i) pmm_free_frame(frames[--i]);
            kfree(frames);
            kfree(mapping);
            return MG_ERR_NO_MEMORY;
        }
    }

    if (!mutex_lock(&process->memory_lock)) {
        for (i = 0; i < page_count; i++)
            pmm_free_frame(frames[i]);
        kfree(frames);
        kfree(mapping);
        return MG_ERR_WOULD_BLOCK;
    }
    address = process_memory_find_space(process, page_count);
    if (!address) {
        mutex_unlock(&process->memory_lock);
        for (i = 0; i < page_count; i++) pmm_free_frame(frames[i]);
        kfree(frames);
        kfree(mapping);
        return MG_ERR_NO_MEMORY;
    }
    mapping->address = address;
    for (i = 0; i < page_count; i++) {
        if (!vmm_map_user_page(process->address_space,
                                         (void *)(address + i * VMM_PAGE_SIZE),
                                         frames[i],
                                         PTE_USER | PTE_READWRITE | PTE_NX)) {
            for (usize mapped = 0; mapped < i; mapped++) {
                phys_addr_t mapped_frame;
                if (vmm_unmap_user_page(
                        process->address_space,
                        (void *)(address + mapped * VMM_PAGE_SIZE),
                                        &mapped_frame)) {
                    pmm_free_frame(mapped_frame);
                } else {
                    pmm_free_frame(frames[mapped]);
                }
            }
            for (usize remaining = i; remaining < page_count; remaining++)
                pmm_free_frame(frames[remaining]);
            mutex_unlock(&process->memory_lock);
            kfree(frames);
            kfree(mapping);
            return MG_ERR_NO_MEMORY;
        }
    }

    cursor = (process_memory_mapping_t **)&process->memory_mappings;
    while (*cursor && (*cursor)->address < address) cursor = &(*cursor)->next;
    mapping->next = *cursor;
    *cursor = mapping;
    *out_address = address;
    mutex_unlock(&process->memory_lock);
    kfree(frames);
    return MG_OK;
}

i64 process_memory_unmap(process_t *process, uintptr_t address)
{
    process_memory_mapping_t **cursor;
    process_memory_mapping_t *mapping;
    bool released;

    if (!process || process->state != PROCESS_STATE_ACTIVE ||
        (address & (VMM_PAGE_SIZE - 1))) {
        return MG_ERR_BAD_ARGUMENT;
    }
    if (!mutex_lock(&process->memory_lock))
        return MG_ERR_WOULD_BLOCK;
    cursor = (process_memory_mapping_t **)&process->memory_mappings;
    while (*cursor && (*cursor)->address != address) cursor = &(*cursor)->next;
    if (!*cursor) {
        mutex_unlock(&process->memory_lock);
        return MG_ERR_NOT_FOUND;
    }
    mapping = *cursor;
    *cursor = mapping->next;
    mutex_unlock(&process->memory_lock);
    released = process_memory_release_mapping(process, mapping);
    kfree(mapping);
    return released ? MG_OK : MG_ERR_IO;
}

bool process_exit(process_t *process, i32 status)
{
    kernel_thread_t *waiter = NULL;
    u64 flags;

    if (!process) {
        return false;
    }
    flags = spin_lock_irqsave(&process_registry_lock);
    if (process->state != PROCESS_STATE_ACTIVE) {
        spin_unlock_irqrestore(&process_registry_lock, flags);
        return false;
    }
    process->exit_status = status;
    process->state = PROCESS_STATE_TERMINATED;
    if (process->parent && process->parent->state == PROCESS_STATE_ACTIVE &&
        process->parent->waiting_child == process &&
        process->parent->waiting_thread) {
        waiter = process->parent->waiting_thread;
        process->parent->waiting_child = NULL;
        process->parent->waiting_thread = NULL;
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);

    /* A full-screen terminal lease belongs to a process, not to its final
     * userspace instruction.  Restore the shell screen after the registry
     * transition, including for external termination and faults. */
    (void)terminal_alternate_leave_process(process->pid);
    /* A fault or abrupt process exit must never leave presentation batching
     * owned by the dead process. */
    terminal_force_end_batch();
    ipc_process_exit(process);
    session_process_exited(process);
    /* Anonymous mappings are process-owned rather than zombie-owned; the
     * exit path releases them before the parent later collects the status. */
    process_memory_release_all(process);
    process_handle_close_all(process);
    if (waiter) {
        (void)scheduler_unblock(waiter);
    }
    return true;
}

bool process_terminate_current_exception(i32 status)
{
    process_t *process = process_current();
    kernel_thread_t *thread = thread_current();

    /* The IDT checked the saved privilege level.  Keep a second ownership
     * check here so an exception path can never terminate an unrelated
     * kernel/service thread. */
    if (!process || !thread || process->state != PROCESS_STATE_ACTIVE ||
        process->main_thread != thread || !process_exit(process, status)) {
        return false;
    }
    return scheduler_terminate();
}

static void process_unlink_locked(process_t *process)
{
    process_t **cursor;
    if (!process || !process->parent) return;
    cursor = &process->parent->first_child;
    while (*cursor && *cursor != process) cursor = &(*cursor)->next_sibling;
    if (*cursor == process) *cursor = process->next_sibling;
    process->parent = NULL;
    process->next_sibling = NULL;
}

static void process_unlink_all_locked(process_t *process)
{
    process_t **cursor;

    if (!process) return;
    cursor = &all_processes;
    while (*cursor && *cursor != process) cursor = &(*cursor)->next_all;
    if (*cursor == process) *cursor = process->next_all;
    process->next_all = NULL;
}

static void process_unlink(process_t *process)
{
    u64 flags;

    if (!process) return;
    flags = spin_lock_irqsave(&process_registry_lock);
    process_unlink_locked(process);
    spin_unlock_irqrestore(&process_registry_lock, flags);
}

static void process_unlink_all(process_t *process)
{
    u64 flags;

    if (!process) return;
    flags = spin_lock_irqsave(&process_registry_lock);
    process_unlink_all_locked(process);
    spin_unlock_irqrestore(&process_registry_lock, flags);
}

static void process_abort(process_t *process)
{
    u64 flags;

    if (!process) return;
    flags = spin_lock_irqsave(&process_registry_lock);
    if (process->state != PROCESS_STATE_ACTIVE) {
        spin_unlock_irqrestore(&process_registry_lock, flags);
        return;
    }
    process->state = PROCESS_STATE_TERMINATED;
    process_unlink_locked(process);
    process_unlink_all_locked(process);
    spin_unlock_irqrestore(&process_registry_lock, flags);
    (void)terminal_alternate_leave_process(process->pid);
    ipc_process_exit(process);
    process_handle_close_all(process);
    process->owner_reference_held = false;
    object_release(&process->object);
}

bool process_terminate_external(process_t *process, i32 status)
{
    kernel_thread_t *main_thread;
    u64 flags;

    if (!process || process == process_current()) {
        return false;
    }
    flags = spin_lock_irqsave(&process_registry_lock);
    if (process->state != PROCESS_STATE_ACTIVE || !process->main_thread) {
        spin_unlock_irqrestore(&process_registry_lock, flags);
        return false;
    }
    main_thread = process->main_thread;
    spin_unlock_irqrestore(&process_registry_lock, flags);
    if (!scheduler_terminate_thread(main_thread)) return false;
    console_cancel_waiter(main_thread);
    return process_exit(process, status);
}

bool process_terminate_session_members(mg_session_id_t session_id)
{
    bool result = true;

    if (session_id == PROCESS_NO_SESSION) return false;
    for (;;) {
        process_t *process = NULL;
        u64 flags = spin_lock_irqsave(&process_registry_lock);

        for (process = all_processes; process; process = process->next_all) {
            if (process->state == PROCESS_STATE_ACTIVE &&
                process->session_id == session_id &&
                object_reference(&process->object)) {
                break;
            }
        }
        spin_unlock_irqrestore(&process_registry_lock, flags);
        if (!process) break;
        if (!process_terminate_external(process, -1)) {
            result = false;
            object_release(&process->object);
            break;
        }
        object_release(&process->object);
    }
    return result;
}

void process_reap_session_members(mg_session_id_t session_id)
{
    for (;;) {
        process_t *process = NULL;
        u64 flags = spin_lock_irqsave(&process_registry_lock);

        for (process = all_processes; process; process = process->next_all) {
            if (process->session_id == session_id &&
                process->state == PROCESS_STATE_TERMINATED &&
                process->owner_reference_held &&
                !scheduler_thread_is_running(process->main_thread) &&
                __atomic_load_n(&process->object.ref_count, __ATOMIC_ACQUIRE) ==
                    1U)
                break;
        }
        if (process) {
            process_unlink_locked(process);
            process->owner_reference_held = false;
        }
        spin_unlock_irqrestore(&process_registry_lock, flags);
        if (!process) break;
        object_release(&process->object);
    }
}

mg_session_id_t process_session_id(const process_t *process)
{
    return process ? process->session_id : PROCESS_NO_SESSION;
}

bool process_is_session_shell(const process_t *process)
{
    return process && process->session_shell;
}

typedef struct process_args {
    int argc;
    char *argv_buf[PROCESS_MAX_ARGUMENTS];
    char raw_args[PROCESS_COMMAND_LINE_CAPACITY];
} process_args_t;

static bool parse_spawn_cmdline(const char *cmdline, char *bin_path, usize bin_path_size,
                                process_args_t *args)
{
    if (!cmdline || !args) return false;
    strncpy(args->raw_args, cmdline, sizeof(args->raw_args) - 1);
    args->raw_args[sizeof(args->raw_args) - 1] = '\0';

    args->argc = 0;
    char *cursor = args->raw_args;
    while (*cursor == ' ' || *cursor == '\t') cursor++;

    while (*cursor != '\0') {
        char *write;
        char quote = '\0';
        if (args->argc >= PROCESS_MAX_ARGUMENTS) break;
        args->argv_buf[args->argc++] = cursor;
        write = cursor;
        while (*cursor != '\0') {
            if (quote != '\0') {
                if (*cursor == quote) {
                    quote = '\0';
                    cursor++;
                } else {
                    *write++ = *cursor++;
                }
            } else if (*cursor == '\'' || *cursor == '"') {
                quote = *cursor++;
            } else if (*cursor == ' ' || *cursor == '\t') {
                break;
            } else {
                *write++ = *cursor++;
            }
        }
        if (*cursor != '\0') cursor++;
        *write = '\0';
        while (*cursor == ' ' || *cursor == '\t') cursor++;
    }
    if (args->argc == 0) return false;

    strncpy(bin_path, args->argv_buf[0], bin_path_size - 1);
    bin_path[bin_path_size - 1] = '\0';
    return true;
}

static bool copy_spawn_argv(const char *const *argv, u32 argc,
                            process_args_t *args)
{
    usize used = 0;

    if (!argv || !args || argc == 0 || argc > PROCESS_MAX_ARGUMENTS)
        return false;
    memset(args, 0, sizeof(*args));
    args->argc = (int)argc;
    for (u32 index = 0; index < argc; index++) {
        usize length;

        if (!argv[index]) return false;
        length = strlen(argv[index]);
        if (length + 1U > sizeof(args->raw_args) - used) return false;
        args->argv_buf[index] = args->raw_args + used;
        memcpy(args->argv_buf[index], argv[index], length + 1U);
        used += length + 1U;
    }
    return true;
}

static bool setup_user_stack_args(process_t *process, const process_args_t *args)
{
    u8 *frame_base = (u8 *)phys_to_virt(process->top_stack_frame);
    if (!frame_base || args->argc == 0) {
        process->user_stack_sp = process->user_stack_top - 16;
        process->user_argc = 0;
        process->user_argv = 0;
        return true;
    }

    usize offset = 0x1000;
    uintptr_t argv_ptrs[PROCESS_MAX_ARGUMENTS];

    for (int i = args->argc - 1; i >= 0; i--) {
        usize len = strlen(args->argv_buf[i]) + 1;
        if (offset < len) return false;
        offset -= len;
        memcpy(frame_base + offset, args->argv_buf[i], len);
        argv_ptrs[i] = (uintptr_t)(0x00007fffffeff000ULL + offset);
    }

    if (offset < sizeof(u64)) return false;
    offset -= sizeof(u64);
    *(u64 *)(frame_base + offset) = 0ULL;

    for (int i = args->argc - 1; i >= 0; i--) {
        if (offset < sizeof(u64)) return false;
        offset -= sizeof(u64);
        *(u64 *)(frame_base + offset) = (u64)argv_ptrs[i];
    }
    uintptr_t user_argv = (uintptr_t)(0x00007fffffeff000ULL + offset);

    offset &= ~15ULL;
    uintptr_t user_sp = (uintptr_t)(0x00007fffffeff000ULL + offset);

    process->user_stack_sp = user_sp;
    process->user_argc = (uintptr_t)args->argc;
    process->user_argv = user_argv;
    return true;
}

bool process_setup_cmdline(process_t *process, const char *cmdline)
{
    char bin_path[256];
    process_args_t args;
    if (!process || !cmdline) return false;
    if (!parse_spawn_cmdline(cmdline, bin_path, sizeof(bin_path), &args)) return false;
    return setup_user_stack_args(process, &args);
}

static void process_user_thread_entry(void *argument)
{
    process_t *process = (process_t *)argument;
    if (!process || process->state != PROCESS_STATE_ACTIVE ||
        !process->image_loaded) {
        (void)process_exit(process, -1);
        (void)scheduler_terminate();
        return;
    }
    ring3_enter(process->entry_point, process->user_stack_sp,
                process->user_argc, process->user_argv);
    (void)process_exit(process, -1);
    (void)scheduler_terminate();
}

static bool process_spawn_args_with_context_internal(
    process_t *parent, const process_args_t *args,
    const process_credentials_t *credentials, mg_session_id_t session_id,
    bool session_shell, const char *initial_cwd, bool system_service,
    u32 service_id, bool inherit_output, process_handle_t output_handle,
    process_handle_t *out_handle)
{
    kernel_thread_t *thread;
    process_t *child;
    kernel_object_t *console;
    kernel_object_t *output;
    process_handle_t child_handle;
    process_handle_t child_input_handle;
    char bin_path[256];
    char resolved_path[512];
    char process_name[32];
    const char *process_name_source;

    if (!parent || parent->state != PROCESS_STATE_ACTIVE || !args ||
        !out_handle || (credentials && !identity_credentials_valid(credentials)) ||
        (!credentials && parent->system_service) ||
        (system_service && (!credentials || !identity_credentials_is_system(credentials) ||
                            !service_id)) ||
        (initial_cwd && (initial_cwd[0] != '/' ||
                         strlen(initial_cwd) >= sizeof(parent->cwd)))) {
        return false;
    }

    if (args->argc <= 0 || args->argc > PROCESS_MAX_ARGUMENTS ||
        !args->argv_buf[0] || !args->argv_buf[0][0]) return false;
    strncpy(bin_path, args->argv_buf[0], sizeof(bin_path) - 1U);
    bin_path[sizeof(bin_path) - 1U] = '\0';

    if (!process_resolve_path(parent, bin_path, resolved_path, sizeof(resolved_path))) return false;

    /* Every child receives a keyboard input handle.  A redirected output
     * handle is inherited separately so interactive children keep reading
     * from the console while their output goes to a file. */
    console = process_handle_lookup(parent, PROCESS_INITIAL_STDIN_HANDLE,
                                    OBJECT_TYPE_CONSOLE,
                                    OBJECT_RIGHT_READ | OBJECT_RIGHT_WRITE);
    if (!console) {
        /* PID 1 predates the split handles and owns only its initial console
         * slot.  Keep it usable as the parent of system services. */
        console = process_handle_lookup(parent, PROCESS_INITIAL_CONSOLE_HANDLE,
                                        OBJECT_TYPE_CONSOLE,
                                        OBJECT_RIGHT_READ | OBJECT_RIGHT_WRITE);
    }
    if (!console) return false;
    output = console;
    if (inherit_output) {
        output = process_handle_lookup_any(parent,
                                           PROCESS_INITIAL_CONSOLE_HANDLE,
                                           OBJECT_TYPE_INVALID,
                                           OBJECT_RIGHT_WRITE);
        if (!output || (output->type != OBJECT_TYPE_FILE &&
                        output->type != OBJECT_TYPE_CONSOLE)) return false;
    } else if (output_handle != 0) {
        output = process_handle_lookup(parent, output_handle,
                                       OBJECT_TYPE_FILE, OBJECT_RIGHT_WRITE);
        if (!output) return false;
    }

    process_name_source = bin_path;
    for (const char *cursor = bin_path; *cursor; cursor++) {
        if (*cursor == '/') process_name_source = cursor + 1;
    }
    if (!*process_name_source) return false;
    strncpy(process_name, process_name_source, sizeof(process_name) - 1U);
    process_name[sizeof(process_name) - 1U] = '\0';

    thread = thread_create_suspended("user", process_user_thread_entry, NULL);
    if (!thread) return false;
    
    child = process_create(process_name, parent, thread);
    if (!child) {
        (void)thread_destroy(thread);
        return false;
    }
    thread->entry_argument = child;
    strncpy(child->executable_path, resolved_path,
            sizeof(child->executable_path) - 1U);
    child->executable_path[sizeof(child->executable_path) - 1U] = '\0';

    if (credentials) {
        child->credentials = *credentials;
        child->credentials_initialized = true;
    }
    child->system_service = system_service;
    child->service_id = system_service ? service_id : 0;
    if (session_id != PROCESS_NO_SESSION) {
        child->session_id = session_id;
        child->session_shell = session_shell;
        if (initial_cwd) {
            strncpy(child->cwd, initial_cwd, sizeof(child->cwd) - 1U);
            child->cwd[sizeof(child->cwd) - 1U] = '\0';
        }
    }
    
    if (!elf_load_process(child, resolved_path, &child->entry_point,
                          &child->user_stack_top)) {
        process_abort(child);
        return false;
    }

    if (!setup_user_stack_args(child, args)) {
        process_abort(child);
        return false;
    }

    if (!process_handle_install(child, output, OBJECT_RIGHT_WRITE,
                                &child_handle) ||
        !process_handle_install(child, console,
                                OBJECT_RIGHT_READ | OBJECT_RIGHT_WRITE,
                                &child_input_handle)) {
        process_abort(child);
        return false;
    }
    
    if (!process_handle_install(parent, &child->object, 0, out_handle)) {
        process_handle_close(child, child_handle);
        process_abort(child);
        return false;
    }

    if (!scheduler_enqueue(thread)) {
        process_handle_close(parent, *out_handle);
        process_abort(child);
        return false;
    }


    return true;
}

static bool process_spawn_with_context_internal(
    process_t *parent, const char *cmdline,
    const process_credentials_t *credentials, mg_session_id_t session_id,
    bool session_shell, const char *initial_cwd, bool system_service,
    u32 service_id, process_handle_t output_handle,
    process_handle_t *out_handle)
{
    char bin_path[256];
    process_args_t args;

    if (!cmdline ||
        !parse_spawn_cmdline(cmdline, bin_path, sizeof(bin_path), &args))
        return false;
    return process_spawn_args_with_context_internal(
        parent, &args, credentials, session_id, session_shell, initial_cwd,
        system_service, service_id, false, output_handle, out_handle);
}

bool process_spawn(process_t *parent, const char *cmdline,
                   process_handle_t *out_handle)
{
    return process_spawn_with_context_internal(parent, cmdline, NULL,
                                               PROCESS_NO_SESSION, false, NULL,
                                               false, 0, 0, out_handle);
}

bool process_spawn_argv(process_t *parent, const char *const *argv,
                        u32 argc, process_handle_t *out_handle)
{
    process_args_t args;

    if (!copy_spawn_argv(argv, argc, &args)) return false;
    return process_spawn_args_with_context_internal(
        parent, &args, NULL, PROCESS_NO_SESSION, false, NULL,
        false, 0, true, 0, out_handle);
}

bool process_spawn_with_output(process_t *parent, const char *cmdline,
                               process_handle_t output_handle,
                               process_handle_t *out_handle)
{
    return process_spawn_with_context_internal(parent, cmdline, NULL,
                                               PROCESS_NO_SESSION, false, NULL,
                                               false, 0, output_handle,
                                               out_handle);
}

bool process_redirect_output(process_t *process, process_handle_t output_handle,
                             process_handle_t *saved_handle)
{
    process_handle_slot_t *slots;
    kernel_object_t *output;
    kernel_object_t *current;
    kernel_object_t *old_output;
    process_handle_t saved;
    u64 flags;

    if (!process || process != process_current() ||
        process->state != PROCESS_STATE_ACTIVE || !saved_handle) return false;
    output = process_handle_lookup_reference(process, output_handle,
                                             OBJECT_TYPE_FILE,
                                             OBJECT_RIGHT_WRITE);
    current = process_handle_lookup_reference(
        process, PROCESS_INITIAL_CONSOLE_HANDLE, OBJECT_TYPE_CONSOLE,
        OBJECT_RIGHT_WRITE);
    if (!output || !current) {
        if (output) object_release(output);
        if (current) object_release(current);
        return false;
    }

    if (!process_handle_install(process, current, OBJECT_RIGHT_WRITE, &saved)) {
        object_release(output);
        object_release(current);
        return false;
    }

    slots = handle_slots(process);
    flags = spin_lock_irqsave(&process->handle_lock);
    if (process->output_redirected || !slots[0].active ||
        !slots[0].object) {
        spin_unlock_irqrestore(&process->handle_lock, flags);
        (void)process_handle_close(process, saved);
        object_release(output);
        object_release(current);
        return false;
    }
    old_output = slots[0].object;
    slots[0].object = output;
    slots[0].rights = OBJECT_RIGHT_WRITE;
    process->output_redirected = true;
    process->redirected_output_saved_handle = saved;
    *saved_handle = saved;
    spin_unlock_irqrestore(&process->handle_lock, flags);
    object_release(old_output);
    object_release(current);
    return true;
}

bool process_restore_output(process_t *process, process_handle_t saved_handle)
{
    process_handle_slot_t *slots;
    kernel_object_t *saved;
    kernel_object_t *saved_handle_object;
    kernel_object_t *old_output;
    u32 saved_index;
    u16 saved_generation;
    u64 flags;

    if (!process || process != process_current() ||
        process->state != PROCESS_STATE_ACTIVE ||
        !decode_handle(saved_handle, &saved_index, &saved_generation) ||
        saved_index >= process->handle_capacity) return false;
    saved = process_handle_lookup_reference(process, saved_handle,
                                            OBJECT_TYPE_CONSOLE,
                                            OBJECT_RIGHT_WRITE);
    if (!saved) return false;
    slots = handle_slots(process);
    flags = spin_lock_irqsave(&process->handle_lock);
    if (!process->output_redirected ||
        saved_handle != process->redirected_output_saved_handle ||
        !slots[0].active || slots[0].generation != 1U ||
        !slots[saved_index].active ||
        slots[saved_index].generation != saved_generation ||
        slots[saved_index].object != saved) {
        spin_unlock_irqrestore(&process->handle_lock, flags);
        object_release(saved);
        return false;
    }
    saved_handle_object = slots[saved_index].object;
    slots[saved_index].object = NULL;
    slots[saved_index].rights = 0;
    slots[saved_index].active = false;
    old_output = slots[0].object;
    slots[0].object = saved;
    slots[0].rights = OBJECT_RIGHT_WRITE;
    process->output_redirected = false;
    process->redirected_output_saved_handle = 0;
    spin_unlock_irqrestore(&process->handle_lock, flags);
    object_release(old_output);
    object_release(saved_handle_object);
    return true;
}

bool process_spawn_with_context(process_t *parent, const char *cmdline,
                                const process_credentials_t *credentials,
                                mg_session_id_t session_id,
                                bool session_shell, const char *initial_cwd,
                                process_handle_t *out_handle)
{
    return process_spawn_with_context_internal(parent, cmdline, credentials,
                                               session_id, session_shell,
                                               initial_cwd, false, 0,
                                               0, out_handle);
}

int process_spawn_service_result(process_t *parent, u32 service_id,
                                 process_handle_t *out_handle)
{
    const kernel_service_definition_t *definition;
    kernel_object_t *object;
    process_t *child;
    process_credentials_t credentials;

    if (!parent || parent->pid != 1U || parent->state != PROCESS_STATE_ACTIVE ||
        !parent->system_service || parent->service_id != MG_SERVICE_SPROUT ||
        service_id == MG_SERVICE_SPROUT ||
        !parent->credentials_initialized ||
        !identity_credentials_valid(&parent->credentials) ||
        !service_definition_lookup(service_id, &definition) ||
        !identity_credentials_is_system(&parent->credentials)) {
        return MG_ERR_PRIVILEGE_REQUIRED;
    }
    credentials = identity_system_credentials();
    credentials.service_privileges = definition->privileges;
    if (!process_spawn_with_context_internal(
            parent, definition->path, &credentials, PROCESS_NO_SESSION, false,
            NULL, true, service_id, 0, out_handle)) {
        return MG_ERR_INVALID_EXEC;
    }
    object = process_handle_lookup_any(parent, *out_handle,
                                       OBJECT_TYPE_PROCESS, 0);
    if (!object) {
        (void)process_handle_close(parent, *out_handle);
        return MG_ERR_IO;
    }
    child = object_process(object);
    return MG_OK;
}

bool process_spawn_service(process_t *parent, u32 service_id,
                           process_handle_t *out_handle)
{
    return process_spawn_service_result(parent, service_id, out_handle) ==
           MG_OK;
}

u64 process_handle_pid(process_t *process, process_handle_t handle)
{
    kernel_object_t *object;
    process_t *child;

    object = process_handle_lookup_any(process, handle, OBJECT_TYPE_PROCESS, 0);
    if (!object) return 0;
    child = object_process(object);
    return child->pid;
}

int process_poll(process_t *parent, process_handle_t handle, i32 *out_status)
{
    kernel_object_t *object;
    process_t *child;

    if (!parent || parent->state != PROCESS_STATE_ACTIVE || !out_status)
        return MG_ERR_BAD_ARGUMENT;
    object = process_handle_lookup_any(parent, handle, OBJECT_TYPE_PROCESS, 0);
    if (!object) return MG_ERR_INVALID_HANDLE;
    child = object_process(object);
    u64 flags = spin_lock_irqsave(&process_registry_lock);
    if (child == parent || child->parent != parent || child->wait_collected) {
        spin_unlock_irqrestore(&process_registry_lock, flags);
        return MG_ERR_NOT_CHILD;
    }
    if (child->state == PROCESS_STATE_ACTIVE) {
        spin_unlock_irqrestore(&process_registry_lock, flags);
        return MG_ERR_WOULD_BLOCK;
    }
    if (scheduler_thread_is_running(child->main_thread)) {
        spin_unlock_irqrestore(&process_registry_lock, flags);
        return MG_ERR_WOULD_BLOCK;
    }
    *out_status = child->exit_status;
    spin_unlock_irqrestore(&process_registry_lock, flags);
    return MG_OK;
}

int process_terminate_child(process_t *parent, process_handle_t handle,
                            i32 status)
{
    kernel_object_t *object;
    process_t *child;

    if (!parent || parent->state != PROCESS_STATE_ACTIVE)
        return MG_ERR_BAD_ARGUMENT;
    object = process_handle_lookup_any(parent, handle, OBJECT_TYPE_PROCESS, 0);
    if (!object) return MG_ERR_INVALID_HANDLE;
    child = object_process(object);
    u64 flags = spin_lock_irqsave(&process_registry_lock);
    if (child == parent || child->parent != parent ||
        child->state != PROCESS_STATE_ACTIVE) {
        spin_unlock_irqrestore(&process_registry_lock, flags);
        return MG_ERR_NOT_CHILD;
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);
    return process_terminate_external(child, status) ? MG_OK : MG_ERR_BUSY;
}

bool process_wait(process_t *parent, process_handle_t handle,
                  i32 *out_status)
{
    kernel_object_t *object;
    process_t *child;
    kernel_thread_t *thread;

    if (!parent || parent->state != PROCESS_STATE_ACTIVE || !out_status) {
        return false;
    }
    object = process_handle_lookup_any(parent, handle, OBJECT_TYPE_PROCESS, 0);
    if (!object) return false;
    child = object_process(object);
    u64 wait_flags = process_cli_save();
    spin_lock(&process_registry_lock);
    if (child == parent || child->parent != parent || child->wait_collected) {
        spin_unlock(&process_registry_lock);
        process_cli_restore(wait_flags);
        return false;
    }
    if (child->state == PROCESS_STATE_ACTIVE) {
        thread = thread_current();
        if (parent->waiting_child || !thread) {
            spin_unlock(&process_registry_lock);
            process_cli_restore(wait_flags);
            return false;
        }
        parent->waiting_child = child;
        parent->waiting_thread = thread;
        spin_unlock(&process_registry_lock);
        if (!scheduler_block()) {
            spin_lock(&process_registry_lock);
            if (parent->waiting_thread == thread &&
                parent->waiting_child == child) {
                parent->waiting_child = NULL;
                parent->waiting_thread = NULL;
            }
            spin_unlock(&process_registry_lock);
            process_cli_restore(wait_flags);
            return false;
        }
        spin_lock(&process_registry_lock);
        if (parent->waiting_thread == thread &&
            parent->waiting_child == child) {
            parent->waiting_child = NULL;
            parent->waiting_thread = NULL;
        }
        if (child->state != PROCESS_STATE_TERMINATED) {
            spin_unlock(&process_registry_lock);
            process_cli_restore(wait_flags);
            return false;
        }
    } else {
        /* process_exit() publishes termination before the dying thread has
         * necessarily completed its scheduler handoff.  Do not let wait/reap
         * destroy that thread or its address space in the handoff window. */
        if (scheduler_thread_is_running(child->main_thread)) {
            spin_unlock(&process_registry_lock);
            process_cli_restore(wait_flags);
            return false;
        }
    }
    *out_status = child->exit_status;
    child->wait_collected = true;
    process_unlink_locked(child);
    child->owner_reference_held = false;
    spin_unlock(&process_registry_lock);
    process_cli_restore(wait_flags);
    object_release(&child->object); /* drop the process's owner reference */
    return true;
}

bool process_handle_install(process_t *process, kernel_object_t *object,
                            u32 rights, process_handle_t *out_handle)
{
    process_handle_slot_t *slots;
    u32 i;
    u64 flags;
    if (!process || process->state != PROCESS_STATE_ACTIVE || !object ||
        !__atomic_load_n(&object->ref_count, __ATOMIC_ACQUIRE) || !out_handle ||
        (rights & ~(OBJECT_RIGHT_READ | OBJECT_RIGHT_WRITE))) return false;
    slots = handle_slots(process);
    flags = spin_lock_irqsave(&process->handle_lock);
    for (i = 0; i < process->handle_capacity; i++) {
        if (!slots[i].active) {
            u16 generation = slots[i].generation + 1U;
            if (!generation) generation = 1;
            if (!object_reference(object)) {
                spin_unlock_irqrestore(&process->handle_lock, flags);
                return false;
            }
            slots[i].object = object;
            slots[i].rights = rights;
            slots[i].generation = generation;
            slots[i].active = true;
            *out_handle = encode_handle(i, generation);
            spin_unlock_irqrestore(&process->handle_lock, flags);
            return true;
        }
    }
    spin_unlock_irqrestore(&process->handle_lock, flags);
    return false;
}

kernel_object_t *process_handle_lookup_any(process_t *process,
                                           process_handle_t handle,
                                           kernel_object_type_t type,
                                           u32 required_rights)
{
    process_handle_slot_t *slots;
    u32 index;
    u16 generation;
    u64 flags;
    if (!process || !process->handle_table ||
        !decode_handle(handle, &index, &generation) ||
        index >= process->handle_capacity) return NULL;
    slots = handle_slots(process);
    flags = spin_lock_irqsave(&process->handle_lock);
    if (!slots[index].active || slots[index].generation != generation ||
        !slots[index].object ||
        (type != OBJECT_TYPE_INVALID && slots[index].object->type != type) ||
        (slots[index].rights & required_rights) != required_rights) {
        spin_unlock_irqrestore(&process->handle_lock, flags);
        return NULL;
    }
    kernel_object_t *object = slots[index].object;
    spin_unlock_irqrestore(&process->handle_lock, flags);
    return object;
}

static kernel_object_t *process_handle_lookup_reference(
    process_t *process, process_handle_t handle,
    kernel_object_type_t type, u32 required_rights)
{
    process_handle_slot_t *slots;
    u32 index;
    u16 generation;
    kernel_object_t *object;
    u64 flags;

    if (!process || !process->handle_table ||
        !decode_handle(handle, &index, &generation) ||
        index >= process->handle_capacity) return NULL;
    slots = handle_slots(process);
    flags = spin_lock_irqsave(&process->handle_lock);
    if (!slots[index].active || slots[index].generation != generation ||
        !slots[index].object ||
        (type != OBJECT_TYPE_INVALID && slots[index].object->type != type) ||
        (slots[index].rights & required_rights) != required_rights ||
        !object_reference(slots[index].object)) {
        spin_unlock_irqrestore(&process->handle_lock, flags);
        return NULL;
    }
    object = slots[index].object;
    spin_unlock_irqrestore(&process->handle_lock, flags);
    return object;
}

kernel_object_t *process_handle_lookup(process_t *process,
                                       process_handle_t handle,
                                       kernel_object_type_t type,
                                       u32 required_rights)
{
    if (!process || process->state != PROCESS_STATE_ACTIVE) return NULL;
    return process_handle_lookup_any(process, handle, type, required_rights);
}

bool process_handle_close(process_t *process, process_handle_t handle)
{
    process_handle_slot_t *slots;
    u32 index;
    u16 generation;
    kernel_object_t *object;
    u64 flags;
    if (!process || !decode_handle(handle, &index, &generation) ||
        index >= process->handle_capacity) return false;
    slots = handle_slots(process);
    flags = spin_lock_irqsave(&process->handle_lock);
    if (!slots[index].active || slots[index].generation != generation) {
        spin_unlock_irqrestore(&process->handle_lock, flags);
        return false;
    }
    object = slots[index].object;
    slots[index].object = NULL;
    slots[index].rights = 0;
    slots[index].active = false;
    spin_unlock_irqrestore(&process->handle_lock, flags);
    object_release(object);
    return true;
}

void process_handle_close_all(process_t *process)
{
    process_handle_slot_t *slots;
    u32 i;
    if (!process || !process->handle_table) return;
    slots = handle_slots(process);
    for (i = 0; i < process->handle_capacity; i++) {
        kernel_object_t *object = NULL;
        u64 flags = spin_lock_irqsave(&process->handle_lock);
        if (slots[i].active) {
            object = slots[i].object;
            slots[i].object = NULL;
            slots[i].rights = 0;
            slots[i].active = false;
        }
        spin_unlock_irqrestore(&process->handle_lock, flags);
        if (object) object_release(object);
    }
}

const char *process_state_name(process_state_t state)
{
    switch (state) {
        case PROCESS_STATE_ACTIVE: return "active";
        case PROCESS_STATE_TERMINATED: return "terminated";
        default: return "unknown";
    }
}

static const char *process_role_name(mg_identity_role_t role)
{
    if (role == MG_IDENTITY_ROLE_REGULAR) return "regular";
    if (role == MG_IDENTITY_ROLE_ADMIN) return "admin";
    return "unknown";
}

void process_dump(void)
{
    process_t *process;
    u64 flags = spin_lock_irqsave(&process_registry_lock);

    kprint("Processes:\n");
    for (process = all_processes; process; process = process->next_all) {
        process_credentials_t credentials = process->credentials;
        if (process->state == PROCESS_STATE_ACTIVE &&
            process->credentials_initialized) {
            (void)identity_credentials_effective(&process->credentials,
                                                  &credentials);
        }
        kprint("  pid=%llu name=%s state=%s uid=%u role=%s session=%llu"
               " shell=%s service=%s(%u) privileges=0x%x\n",
               process->pid, process->name, process_state_name(process->state),
               credentials.uid, process_role_name(credentials.role),
               process->session_id, process->session_shell ? "yes" : "no",
               process->system_service ? "yes" : "no", process->service_id,
               credentials.service_privileges);
    }
    spin_unlock_irqrestore(&process_registry_lock, flags);
}

static u32 process_inspection_state(const process_t *process)
{
    if (!process || process->state == PROCESS_STATE_TERMINATED)
        return MG_PROCESS_INSPECTION_EXITED;
    if (!process->main_thread)
        return MG_PROCESS_INSPECTION_BLOCKED;
    switch (process->main_thread->state) {
        case THREAD_STATE_RUNNING:
            return MG_PROCESS_INSPECTION_RUNNING;
        case THREAD_STATE_READY:
            return MG_PROCESS_INSPECTION_READY;
        case THREAD_STATE_BLOCKED:
            return MG_PROCESS_INSPECTION_BLOCKED;
        case THREAD_STATE_TERMINATED:
            return MG_PROCESS_INSPECTION_EXITED;
        default:
            return MG_PROCESS_INSPECTION_BLOCKED;
    }
}

u32 process_snapshot_read(u32 offset, mg_process_info_t *output,
                          u32 capacity, u32 *out_total)
{
    process_t *process;
    u32 total = 0;
    u32 copied = 0;

    if (!output || !capacity || capacity > MG_PROCESS_SNAPSHOT_PAGE_MAX ||
        !out_total) return 0;
    u64 flags = spin_lock_irqsave(&process_registry_lock);
    for (process = all_processes; process; process = process->next_all) {
        process_credentials_t credentials = process->credentials;
        mg_process_info_t info;
        user_identity_t account;
        const kernel_service_definition_t *definition = NULL;

        if (total >= offset && copied < capacity) {
            memset(&info, 0, sizeof(info));
            info.pid = process->pid;
            info.parent_pid = process->parent ? process->parent->pid : 0;
            info.session_id = process->session_id;
            info.state = process_inspection_state(process);
            info.running_cpu = scheduler_thread_running_cpu(
                process->main_thread);
            info.flags = (process->system_service
                          ? MG_PROCESS_FLAG_SYSTEM_SERVICE : 0U) |
                         (process->session_shell
                          ? MG_PROCESS_FLAG_SESSION_SHELL : 0U);
            if (process->credentials_initialized &&
                identity_credentials_effective(&process->credentials,
                                               &credentials)) {
                /* Use live human role data, while preserving explicit
                 * service capabilities for system processes. */
            }
            info.uid = credentials.uid;
            if (credentials.uid == MG_UID_SYSTEM) {
                info.role = MG_INSPECTION_ROLE_SYSTEM;
                strncpy(info.username, "system", sizeof(info.username) - 1U);
            } else {
                info.role = credentials.role;
                if (identity_registry_lookup_uid(credentials.uid, &account))
                    strncpy(info.username, account.username,
                            sizeof(info.username) - 1U);
            }
            strncpy(info.name, process->name, sizeof(info.name) - 1U);
            info.cpu_time_ms = __atomic_load_n(&process->cpu_time_ticks,
                                               __ATOMIC_RELAXED);
            info.memory_bytes = vmm_address_space_user_memory_bytes(
                process->address_space);
            strncpy(info.executable_path, process->executable_path,
                    sizeof(info.executable_path) - 1U);
            if (process->system_service &&
                service_definition_lookup(process->service_id, &definition) &&
                definition && definition->name) {
                strncpy(info.service, definition->name,
                        sizeof(info.service) - 1U);
                if (!info.executable_path[0] && definition->path) {
                    strncpy(info.executable_path, definition->path,
                            sizeof(info.executable_path) - 1U);
                }
            }
            output[copied++] = info;
        }
        total++;
    }
    *out_total = total;
    spin_unlock_irqrestore(&process_registry_lock, flags);
    return copied;
}
