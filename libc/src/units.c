#include <units.h>

u64 bytes_to_kib(u64 bytes){
    return bytes / 1024;
}

u64 bytes_to_mib(u64 bytes){
    return bytes / 1024 / 1024;
}

u64 bytes_to_gib(u64 bytes){
    return bytes / (1024ULL * 1024ULL * 1024ULL);
}
