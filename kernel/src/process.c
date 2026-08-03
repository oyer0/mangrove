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

#ifndef NULL
#define NULL ((void *)0)
#endif

static u64 next_pid;

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
    next_pid = 1;
    return true;
}

process_t *process_create(const char *name, process_t *parent,
                          struct kernel_thread *main_thread)
{
    process_t *process;

    if (!name || !main_thread || main_thread->process ||
        (parent && parent->state != PROCESS_STATE_ACTIVE) || next_pid == 0) {
        return NULL;
    }

    process = (process_t *)kmalloc(sizeof(*process));
    if (!process) {
        return NULL;
    }
    memset(process, 0, sizeof(*process));
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
    process->pid = next_pid++;
    process->state = PROCESS_STATE_ACTIVE;
    process->parent = parent;
    process->main_thread = main_thread;
    strncpy(process->name, name, sizeof(process->name) - 1);
    process->name[sizeof(process->name) - 1] = '\0';
    if (parent) {
        strncpy(process->cwd, parent->cwd, sizeof(process->cwd) - 1);
    } else {
        strncpy(process->cwd, "/", sizeof(process->cwd) - 1);
    }
    process->cwd[sizeof(process->cwd) - 1] = '\0';

    if (parent) {
        process->next_sibling = parent->first_child;
        parent->first_child = process;
    }

    main_thread->process = process;
    return process;
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

bool process_chdir(process_t *process, const char *input)
{
    char path[512];
    vfs_node_t *node = NULL;

    if (!process_resolve_path(process, input, path, sizeof(path))) {
        return false;
    }
    int res = vfs_lookup(path, &node);
    if (res != VFS_OK || !node || node->type != VFS_TYPE_DIRECTORY) {
        return false;
    }
    strncpy(process->cwd, path, sizeof(process->cwd) - 1);
    process->cwd[sizeof(process->cwd) - 1] = '\0';
    return true;
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
        void *frame;
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
    mapping = (process_memory_mapping_t *)process->memory_mappings;
    process->memory_mappings = NULL;
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
    usize page_count;
    usize i;

    if (!process || process->state != PROCESS_STATE_ACTIVE || !out_address ||
        !process_memory_size_to_pages(size, &page_count)) {
        return MG_ERR_BAD_ARGUMENT;
    }
    address = process_memory_find_space(process, page_count);
    if (!address) return MG_ERR_NO_MEMORY;
    mapping = (process_memory_mapping_t *)kmalloc(sizeof(*mapping));
    if (!mapping) return MG_ERR_NO_MEMORY;
    mapping->address = address;
    mapping->page_count = page_count;
    mapping->next = NULL;

    for (i = 0; i < page_count; i++) {
        void *frame = pmm_alloc_frame();
        if (!frame || !vmm_map_user_page(process->address_space,
                                         (void *)(address + i * VMM_PAGE_SIZE),
                                         frame,
                                         PTE_USER | PTE_READWRITE | PTE_NX)) {
            if (frame) pmm_free_frame(frame);
            while (i != 0) {
                void *mapped_frame;
                i--;
                if (vmm_unmap_user_page(process->address_space,
                                        (void *)(address + i * VMM_PAGE_SIZE),
                                        &mapped_frame)) {
                    pmm_free_frame(mapped_frame);
                }
            }
            kfree(mapping);
            return MG_ERR_NO_MEMORY;
        }
    }

    cursor = (process_memory_mapping_t **)&process->memory_mappings;
    while (*cursor && (*cursor)->address < address) cursor = &(*cursor)->next;
    mapping->next = *cursor;
    *cursor = mapping;
    *out_address = address;
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
    cursor = (process_memory_mapping_t **)&process->memory_mappings;
    while (*cursor && (*cursor)->address != address) cursor = &(*cursor)->next;
    if (!*cursor) return MG_ERR_NOT_FOUND;
    mapping = *cursor;
    *cursor = mapping->next;
    released = process_memory_release_mapping(process, mapping);
    kfree(mapping);
    return released ? MG_OK : MG_ERR_IO;
}

bool process_exit(process_t *process, i32 status)
{
    if (!process || process->state != PROCESS_STATE_ACTIVE) {
        return false;
    }
    process->exit_status = status;
    process->state = PROCESS_STATE_TERMINATED;
    /* Anonymous mappings are process-owned rather than zombie-owned; the
     * exit path releases them before the parent later collects the status. */
    process_memory_release_all(process);
    process_handle_close_all(process);
    if (process->parent && process->parent->waiting_child == process &&
        process->parent->waiting_thread) {
        kernel_thread_t *waiter = process->parent->waiting_thread;
        process->parent->waiting_thread = NULL;
        (void)scheduler_unblock(waiter);
    }
    return true;
}

static void process_unlink(process_t *process)
{
    process_t **cursor;
    if (!process || !process->parent) return;
    cursor = &process->parent->first_child;
    while (*cursor && *cursor != process) cursor = &(*cursor)->next_sibling;
    if (*cursor == process) *cursor = process->next_sibling;
    process->parent = NULL;
    process->next_sibling = NULL;
}

static void process_abort(process_t *process)
{
    if (!process) return;
    process_unlink(process);
    process->state = PROCESS_STATE_TERMINATED;
    process_handle_close_all(process);
    object_release(&process->object);
}

typedef struct process_args {
    int argc;
    char *argv_buf[16];
    char raw_args[256];
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
        if (args->argc >= 16) break;
        args->argv_buf[args->argc++] = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') cursor++;
        if (*cursor != '\0') *cursor++ = '\0';
        while (*cursor == ' ' || *cursor == '\t') cursor++;
    }
    if (args->argc == 0) return false;

    strncpy(bin_path, args->argv_buf[0], bin_path_size - 1);
    bin_path[bin_path_size - 1] = '\0';
    return true;
}

static bool setup_user_stack_args(process_t *process, const process_args_t *args)
{
    u8 *frame_base = (u8 *)process->top_stack_frame;
    if (!frame_base || args->argc == 0) {
        process->user_stack_sp = process->user_stack_top - 16;
        process->user_argc = 0;
        process->user_argv = 0;
        return true;
    }

    usize offset = 0x1000;
    uintptr_t argv_ptrs[16];

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

bool process_spawn(process_t *parent, const char *cmdline,
                   process_handle_t *out_handle)
{
    kernel_thread_t *thread;
    process_t *child;
    kernel_object_t *console;
    process_handle_t child_handle;
    char bin_path[256];
    char resolved_path[512];
    process_args_t args;

    if (!parent || parent->state != PROCESS_STATE_ACTIVE || !cmdline ||
        !out_handle) return false;

    if (!parse_spawn_cmdline(cmdline, bin_path, sizeof(bin_path), &args)) return false;

    if (!process_resolve_path(parent, bin_path, resolved_path, sizeof(resolved_path))) return false;

    thread = thread_create("user", process_user_thread_entry, NULL);
    if (!thread) return false;
    child = process_create("child", parent, thread);
    if (!child) {
        (void)thread_destroy(thread);
        return false;
    }
    thread->entry_argument = child;
    if (!elf_load_process(child, resolved_path, &child->entry_point,
                          &child->user_stack_top)) {
        process_abort(child);
        return false;
    }

    if (!setup_user_stack_args(child, &args)) {
        process_abort(child);
        return false;
    }

    console = process_handle_lookup(parent, PROCESS_INITIAL_CONSOLE_HANDLE,
                                    OBJECT_TYPE_CONSOLE,
                                    OBJECT_RIGHT_READ | OBJECT_RIGHT_WRITE);
    if (!console || !process_handle_install(child, console,
                                             OBJECT_RIGHT_READ | OBJECT_RIGHT_WRITE,
                                             &child_handle)) {
        process_abort(child);
        return false;
    }
    if (!process_handle_install(parent, &child->object, 0, out_handle)) {
        process_handle_close(child, child_handle);
        process_abort(child);
        return false;
    }
    return true;
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
    if (child == parent || child->parent != parent || child->wait_collected) {
        return false;
    }
    if (child->state == PROCESS_STATE_ACTIVE) {
        thread = thread_current();
        if (parent->waiting_child || !thread) {
            return false;
        }
        parent->waiting_child = child;
        parent->waiting_thread = thread;
        if (!scheduler_block()) {
            parent->waiting_child = NULL;
            parent->waiting_thread = NULL;
            return false;
        }
        parent->waiting_child = NULL;
        parent->waiting_thread = NULL;
        if (child->state != PROCESS_STATE_TERMINATED) return false;
    }
    *out_status = child->exit_status;
    child->wait_collected = true;
    process_unlink(child);
    object_release(&child->object); /* drop the process's owner reference */
    return true;
}

bool process_handle_install(process_t *process, kernel_object_t *object,
                            u32 rights, process_handle_t *out_handle)
{
    process_handle_slot_t *slots;
    u32 i;
    if (!process || process->state != PROCESS_STATE_ACTIVE || !object ||
        !object->ref_count || !out_handle ||
        (rights & ~(OBJECT_RIGHT_READ | OBJECT_RIGHT_WRITE))) return false;
    slots = handle_slots(process);
    for (i = 0; i < process->handle_capacity; i++) {
        if (!slots[i].active) {
            u16 generation = slots[i].generation + 1U;
            if (!generation) generation = 1;
            if (!object_reference(object)) return false;
            slots[i].object = object;
            slots[i].rights = rights;
            slots[i].generation = generation;
            slots[i].active = true;
            *out_handle = encode_handle(i, generation);
            return true;
        }
    }
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
    if (!process || !process->handle_table ||
        !decode_handle(handle, &index, &generation) ||
        index >= process->handle_capacity) return NULL;
    slots = handle_slots(process);
    if (!slots[index].active || slots[index].generation != generation ||
        !slots[index].object ||
        (type != OBJECT_TYPE_INVALID && slots[index].object->type != type) ||
        (slots[index].rights & required_rights) != required_rights) return NULL;
    return slots[index].object;
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
    if (!process || !decode_handle(handle, &index, &generation) ||
        index >= process->handle_capacity) return false;
    slots = handle_slots(process);
    if (!slots[index].active || slots[index].generation != generation) return false;
    object_release(slots[index].object);
    slots[index].object = NULL;
    slots[index].rights = 0;
    slots[index].active = false;
    return true;
}

void process_handle_close_all(process_t *process)
{
    process_handle_slot_t *slots;
    u32 i;
    if (!process || !process->handle_table) return;
    slots = handle_slots(process);
    for (i = 0; i < process->handle_capacity; i++) {
        if (slots[i].active) {
            object_release(slots[i].object);
            slots[i].object = NULL;
            slots[i].rights = 0;
            slots[i].active = false;
        }
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
