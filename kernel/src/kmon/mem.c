#include <kmon/mem.h>
#include <units.h>
#include <pmm.h>
#include <kprint.h>

void shell_mem(void){
    u64 total = pmm_get_total_memory();
    u64 used = pmm_get_used_memory();
    u64 free = pmm_get_free_memory();

    kprint("Memory Information\n\n");

    kprint("Total: %u MiB\n", (u32)bytes_to_mib(total));
    kprint("Used: %u MiB\n", (u32)bytes_to_mib(used));
    kprint("Free: %u MiB\n", (u32)bytes_to_mib(free));
}
