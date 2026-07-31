#include <kmon/heap.h>
#include <heap.h>
#include <kprint.h>

void kmon_heap(int argc, char **argv) {
    (void)argc; (void)argv;
    heap_dump();
}
