#include <kmon/heap.h>

#include <heap.h>
#include <kprint.h>
#include <units.h>

void kmon_heap(void){
    kprint("Heap Information\n\n");

    kprint("Total: %u KiB\n",
        (u32)bytes_to_kib(heap_get_total_size()));

    kprint("Used: %u KiB\n",
        (u32)bytes_to_kib(heap_get_used_size()));

    kprint("Free: %u KiB\n",
        (u32)bytes_to_kib(heap_get_free_size()));
}
