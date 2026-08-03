#pragma once

#include <mg/error.h>

/* Size is rounded by the kernel to pages; mappings belong to the process. */
mg_result_t memory_map(usize size, void **out_address);
/* Address must be the base of a live mapping. */
mg_result_t memory_unmap(void *address);
