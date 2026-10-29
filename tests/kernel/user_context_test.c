/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production initial-context, FS-base handoff, and worker-return paths. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uintptr_t usize;
typedef unsigned char u8;
typedef struct { u64 words[16]; } fpu_state_t;
typedef enum { PROCESS_STATE_ACTIVE, PROCESS_STATE_EXITING } process_state_t;
typedef enum { THREAD_PROCESS_UNATTACHED, THREAD_PROCESS_ATTACHED } thread_process_state_t;
typedef enum { PROCESS_USER_REGION_INITIAL_STACK=1,
               PROCESS_USER_REGION_WORKER_STACK,
               PROCESS_USER_REGION_THREAD_TLS } process_user_region_kind_t;
typedef enum { PROCESS_REGION_RELEASE_INVALID=0,
               PROCESS_REGION_RELEASE_DEFERRED,
               PROCESS_REGION_RELEASED } process_region_release_result_t;
#define PROCESS_USER_STACK_PAGES 8U
#define VMM_PAGE_SIZE 4096U
#define PROCESS_USER_THREAD_RETURN_ADDRESS ((uintptr_t)0x00400000U)
struct process_user_region;
enum { THREAD_STATE_RUNNING, THREAD_STATE_READY, THREAD_STATE_BLOCKED,
       THREAD_STATE_TERMINATED };
#define THREAD_CPU_NONE (~(u32)0)
#define MSR_IA32_FS_BASE 0xc0000100U
typedef struct user_execution_context {
    uintptr_t instruction_pointer, stack_pointer;
    uintptr_t rdi, rsi, rdx, rcx, fs_base;
    struct process_user_region *stack_region, *tls_region;
    bool valid;
} user_execution_context_t;
typedef struct process process_t;
typedef struct { unsigned references; } kernel_object_t;
typedef struct process_user_region {
    uintptr_t reservation_base;
    usize reservation_pages;
    uintptr_t mapping_base;
    usize mapping_pages;
    process_user_region_kind_t kind;
    bool worker_owned, release_pending;
} process_user_region_t;
typedef void (*thread_entry_t)(void *);
typedef struct kernel_thread {
    uintptr_t saved_stack_pointer;
    bool saved_context_valid;
    fpu_state_t fpu_state;
    u32 running_cpu;
    bool context_switch_pending, queued;
    int state;
    process_t *process;
    thread_process_state_t process_state;
    bool process_initial;
    void *worker_record;
    uintptr_t user_fs_base;
    user_execution_context_t user_context;
    thread_entry_t entry;
    void *entry_argument;
} kernel_thread_t;
struct process {
    kernel_object_t object;
    process_state_t state;
    bool exit_requested;
    bool image_loaded;
    void *address_space;
    bool tls_present;
    kernel_thread_t *main_thread;
    uintptr_t entry_point, user_stack_sp, user_stack_top;
    uintptr_t user_argc, user_argv;
};
typedef struct cpu_local {
    kernel_thread_t *scheduler_idle_thread;
} cpu_local_t;

static kernel_thread_t *current_thread;
static kernel_thread_t bootstrap_thread;
static cpu_local_t fake_cpu;
static bool scheduler_context_switch_in_progress;
static u64 scheduler_lock;
static uintptr_t fake_fs_base;
static unsigned completed_workers, terminated_workers, process_exits;
static u64 process_exit_result;
static uintptr_t fake_next_region=0x400000000000ULL;
static uintptr_t fake_tls_base=0x600000000000ULL;
static bool fail_fake_tls, fake_stack_released;
static bool terminate_during_fake_tls;
static unsigned fake_regions_released;

static u64 spin_lock_irqsave(u64 *lock) { (void)lock; return 0; }
static void spin_unlock_irqrestore(u64 *lock, u64 flags)
{ (void)lock; (void)flags; }
static cpu_local_t *scheduler_cpu_local(void) { return &fake_cpu; }
static void fpu_state_save(fpu_state_t *state) { (void)state; }
static void fpu_state_restore(const fpu_state_t *state) { (void)state; }
static u64 rdmsr(u32 msr) { assert(msr == 0xc0000100U); return fake_fs_base; }
static void wrmsr(u32 msr, u64 value)
{ assert(msr == 0xc0000100U); fake_fs_base = (uintptr_t)value; }
static bool scheduler_enqueue_locked(kernel_thread_t *thread)
{ thread->queued = true; return true; }
static bool process_scheduler_context_handoff_complete(kernel_thread_t *thread)
{ (void)thread; return true; }
static bool object_reference(kernel_object_t *object)
{ return object != NULL; }
static kernel_thread_t *thread_current(void) { return current_thread; }
static void process_worker_exit(kernel_thread_t *thread, u64 result)
{ assert(thread && !thread->process_initial); completed_workers++;
  process_exit_result = result; }
static bool scheduler_terminate(void)
{ terminated_workers++; return true; }
static void panic(const char *message) { (void)message; assert(false); }
static process_user_region_t *process_user_region_find(
    process_t *process, process_user_region_kind_t kind)
{
    static process_user_region_t initial={.kind=PROCESS_USER_REGION_INITIAL_STACK};
    assert(process);
    return kind==PROCESS_USER_REGION_INITIAL_STACK ? &initial : NULL;
}
static bool vmm_user_range_executable(void *space, uintptr_t address, usize size)
{ return space && address && size==1U; }
static process_user_region_t *process_user_region_create(
    process_t *process, process_user_region_kind_t kind, usize guards,
    usize pages, usize alignment, const void *data, usize data_size,
    usize initial_offset, usize tls_block_size, uintptr_t *out_data)
{
    static process_user_region_t regions[8];
    static unsigned next;
    (void)alignment;(void)tls_block_size;
    assert(process&&pages&&guards==1U);
    if(kind==PROCESS_USER_REGION_WORKER_STACK){
        uintptr_t return_slot=0;
        assert(data_size==sizeof(return_slot)&&initial_offset==pages*VMM_PAGE_SIZE-sizeof(return_slot));
        memcpy(&return_slot,data,sizeof(return_slot));
        assert(return_slot==PROCESS_USER_THREAD_RETURN_ADDRESS);
    }
    process_user_region_t *region=&regions[next++];
    *region=(process_user_region_t){.reservation_base=fake_next_region,
        .reservation_pages=guards+pages,.mapping_base=fake_next_region+guards*VMM_PAGE_SIZE,
        .mapping_pages=pages,.kind=kind};
    fake_next_region += (guards+pages)*VMM_PAGE_SIZE;
    if(out_data)*out_data=region->mapping_base;
    return region;
}
static process_user_region_t *process_user_tls_create(process_t *process,
                                                        uintptr_t *fs_base)
{
    static process_user_region_t tls_regions[8];
    static unsigned next;
    if(fail_fake_tls)return NULL;
    tls_regions[next]=(process_user_region_t){.reservation_base=fake_tls_base,
        .reservation_pages=2,.mapping_base=fake_tls_base+VMM_PAGE_SIZE,
        .mapping_pages=1,.kind=PROCESS_USER_REGION_THREAD_TLS};
    process_user_region_t *result=&tls_regions[next++];
    fake_tls_base += 2U*VMM_PAGE_SIZE;
    *fs_base=result->mapping_base+64U;
    if (terminate_during_fake_tls) {
        process->state=PROCESS_STATE_EXITING;
        process->exit_requested=true;
    }
    assert(process);
    return result;
}
static bool process_user_region_set_worker_owned(
    process_t *process, process_user_region_t *stack,
    process_user_region_t *tls, bool worker_owned)
{ (void)tls; (void)worker_owned; return process && stack; }
static process_region_release_result_t process_user_region_release(
    process_t *process, process_user_region_t *r)
{ assert(process&&r);fake_regions_released++;
  if(r->kind==PROCESS_USER_REGION_WORKER_STACK)fake_stack_released=true;
  return PROCESS_REGION_RELEASED; }
static void process_user_thread_entry(void *argument) { (void)argument; }

/* PROCESS_INITIAL_CONTEXT */
/* PROCESS_WORKER_CONTEXT */
/* SCHEDULER_CONTEXT */
/* WORKER_RETURN */

static void test_initial_contexts_are_thread_owned(void)
{
    process_t first = {.state=PROCESS_STATE_ACTIVE,.image_loaded=true,
        .tls_present=true,
        .address_space=(void *)1,.entry_point=0x401000,
        .user_stack_sp=0x7fffffefeff0, .user_stack_top=0x7ffffff00000,
        .user_argc=3, .user_argv=0x7fffff000000};
    process_t second = {.state=PROCESS_STATE_ACTIVE,.image_loaded=true,
        .tls_present=true,
        .address_space=(void *)1,.entry_point=0x402000,
        .user_stack_sp=0x7fffffefffe0, .user_stack_top=0x7fffffeffff0,
        .user_argc=1, .user_argv=0x7ffffef00000};
    kernel_thread_t first_thread = {.process=&first, .process_initial=true};
    kernel_thread_t second_thread = {.process=&second, .process_initial=true};
    first.main_thread=&first_thread;
    second.main_thread=&second_thread;

    assert(process_user_context_prepare_initial(&first));
    assert(process_user_context_prepare_initial(&second));
    assert(first_thread.user_context.valid && second_thread.user_context.valid);
    assert(first_thread.user_context.instruction_pointer == 0x401000);
    assert(second_thread.user_context.instruction_pointer == 0x402000);
    assert(first_thread.user_context.stack_pointer == first.user_stack_sp);
    assert(second_thread.user_context.stack_pointer == second.user_stack_sp);
    assert(first_thread.user_context.rdi == 3 &&
           first_thread.user_context.rsi == first.user_argv);
    assert(second_thread.user_context.rdi == 1 &&
           second_thread.user_context.rsi == second.user_argv);
    assert(first_thread.user_context.tls_region &&
           second_thread.user_context.tls_region &&
           first_thread.user_context.fs_base != second_thread.user_context.fs_base);
    assert(!process_user_context_prepare_initial(&first));
    first_thread.user_context.valid=false;
    first.entry_point=0;
    assert(!process_user_context_prepare_initial(&first));
}

static void test_worker_contexts_have_distinct_stacks_and_tls(void)
{
    process_t process={.state=PROCESS_STATE_ACTIVE,.image_loaded=true,
        .address_space=(void *)1,.tls_present=true};
    kernel_thread_t first={.process_state=THREAD_PROCESS_UNATTACHED};
    kernel_thread_t second={.process_state=THREAD_PROCESS_UNATTACHED};
    assert(process_user_context_prepare_worker(&process,&first,0x401000,0x1111));
    assert(process_user_context_prepare_worker(&process,&second,0x402000,0x2222));
    assert(first.user_context.valid&&second.user_context.valid);
    assert(first.user_context.instruction_pointer==0x401000&&
           second.user_context.instruction_pointer==0x402000);
    assert(first.user_context.rdi==0x1111&&second.user_context.rdi==0x2222);
    assert(first.user_context.stack_pointer+sizeof(uintptr_t)==
           first.user_context.stack_region->mapping_base+8U*VMM_PAGE_SIZE);
    assert(second.user_context.stack_region!=first.user_context.stack_region);
    assert(first.user_context.tls_region!=second.user_context.tls_region);
    assert(first.user_context.fs_base!=second.user_context.fs_base);
    assert(first.user_fs_base==first.user_context.fs_base);
    assert(first.entry==process_user_thread_entry&&second.entry==process_user_thread_entry);

    fake_stack_released=false;
    assert(process_user_context_discard_worker(&process,&first));
    assert(fake_stack_released&&!first.user_context.valid&&!first.entry);

    terminate_during_fake_tls=true;fake_stack_released=false;
    fake_regions_released=0;
    kernel_thread_t racing_exit={.process_state=THREAD_PROCESS_UNATTACHED};
    assert(!process_user_context_prepare_worker(&process,&racing_exit,
                                                0x403000,0));
    assert(fake_stack_released&&fake_regions_released==2&&
           !racing_exit.user_context.valid&&!racing_exit.entry);
    terminate_during_fake_tls=false;

    fail_fake_tls=true;fake_stack_released=false;
    kernel_thread_t failed={.process_state=THREAD_PROCESS_UNATTACHED};
    process.state=PROCESS_STATE_ACTIVE;process.exit_requested=false;
    assert(!process_user_context_prepare_worker(&process,&failed,0x404000,0));
    assert(fake_stack_released&&!failed.user_context.valid&&!failed.entry);
    fail_fake_tls=false;

    process.exit_requested=true;
    kernel_thread_t terminating={.process_state=THREAD_PROCESS_UNATTACHED};
    assert(!process_user_context_prepare_worker(&process,&terminating,0x405000,0));
    assert(!terminating.user_context.valid&&!terminating.entry);
}

static void test_fs_base_follows_migrating_context(void)
{
    kernel_thread_t first={.saved_stack_pointer=0x1000,
        .user_fs_base=0x11112000,.state=THREAD_STATE_BLOCKED};
    kernel_thread_t second={.saved_stack_pointer=0x2000,
        .user_fs_base=0x22223000,.state=THREAD_STATE_BLOCKED};

    current_thread=&second;
    fake_fs_base=0x11112000;
    scheduler_context_switch_saved(&first.saved_stack_pointer,
                                   second.saved_stack_pointer);
    scheduler_context_switch_complete(&first.saved_stack_pointer);
    assert(first.user_fs_base==0x11112000 && fake_fs_base==0x22223000);

    /* The resumed thread modified its own base before the next preemption. */
    current_thread=&first;
    fake_fs_base=0x22223456;
    scheduler_context_switch_saved(&second.saved_stack_pointer,
                                   first.saved_stack_pointer);
    scheduler_context_switch_complete(&second.saved_stack_pointer);
    assert(second.user_fs_base==0x22223456 && fake_fs_base==0x11112000);
}

static void test_return_retires_worker_only(void)
{
    kernel_thread_t worker={.process=(process_t *)1,.process_initial=false,
                            .worker_record=(void *)1};
    kernel_thread_t initial={.process=(process_t *)1,.process_initial=true};
    current_thread=&worker;
    completed_workers=terminated_workers=process_exits=0;
    process_exit_result = 0;
    assert(syscall_finish_worker_return(0x1234U));
    assert(completed_workers==1 && terminated_workers==1 && !process_exits &&
           process_exit_result==0x1234U);
    current_thread=&initial;
    assert(!syscall_finish_worker_return(0x5678U));
    assert(completed_workers==1 && terminated_workers==1 && !process_exits);
}

int main(void)
{
    test_initial_contexts_are_thread_owned();
    test_worker_contexts_have_distinct_stacks_and_tls();
    test_fs_base_follows_migrating_context();
    test_return_retires_worker_only();
    puts("Ring 3 per-thread entry state, FS-base handoff, and worker return PASS");
    return 0;
}
