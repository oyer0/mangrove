#include <kmon/mem.h>
#include <pmm.h>
#include <kprint.h>

void kmon_mem(int argc, char **argv) {
    (void)argc; (void)argv;
    kprint("Physical Memory Usage:\n");
    kprint("  Total: %llu MB\n", pmm_get_total_memory() / (1024 * 1024));
    kprint("  Used:  %llu MB\n", pmm_get_used_memory() / (1024 * 1024));
    kprint("  Free:  %llu MB\n", pmm_get_free_memory() / (1024 * 1024));
}
