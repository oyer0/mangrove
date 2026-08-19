#pragma once

#include <types.h>
#include <address_space.h>
#include <object.h>

struct kernel_thread;
struct page_table;
struct process_memory_mapping;
typedef struct process process_t;
typedef u32 process_handle_t;

/* First installed handle in a fresh process (slot 0, generation 1). */
#define PROCESS_INITIAL_CONSOLE_HANDLE ((process_handle_t)0x00010001U)

#define PROCESS_HANDLE_RIGHT_READ  OBJECT_RIGHT_READ
#define PROCESS_HANDLE_RIGHT_WRITE OBJECT_RIGHT_WRITE

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
    char cwd[256];
    kernel_object_t object;
    void *handle_table;
    usize handle_capacity;
    struct process_memory_mapping *memory_mappings;
    char name[32];
};

bool process_init(void);
process_t *process_create(const char *name, process_t *parent,
                          struct kernel_thread *main_thread);
bool process_attach_thread(process_t *process, struct kernel_thread *thread);
process_t *process_current(void);
bool process_exit(process_t *process, i32 status);
bool process_setup_cmdline(process_t *process, const char *cmdline);
bool process_spawn(process_t *parent, const char *cmdline,
                   process_handle_t *out_handle);
bool process_wait(process_t *parent, process_handle_t handle,
                  i32 *out_status);
bool process_resolve_path(process_t *process, const char *input,
                          char *output, usize output_size);
bool process_chdir(process_t *process, const char *input);
bool process_split_path(process_t *process, const char *input,
                        char *parent, usize parent_size,
                        char *name, usize name_size);
i64 process_memory_map(process_t *process, usize size,
                       uintptr_t *out_address);
i64 process_memory_unmap(process_t *process, uintptr_t address);
kernel_object_t *process_handle_lookup_any(process_t *process,
                                           process_handle_t handle,
                                           kernel_object_type_t type,
                                           u32 required_rights);
bool process_handle_install(process_t *process, kernel_object_t *object,
                            u32 rights, process_handle_t *out_handle);
kernel_object_t *process_handle_lookup(process_t *process,
                                       process_handle_t handle,
                                       kernel_object_type_t type,
                                       u32 required_rights);
bool process_handle_close(process_t *process, process_handle_t handle);
void process_handle_close_all(process_t *process);
const char *process_state_name(process_state_t state);
