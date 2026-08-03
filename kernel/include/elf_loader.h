#pragma once

#include <types.h>

struct process;

bool elf_load_process(struct process *process, const char *path,
                      uintptr_t *entry_point, uintptr_t *stack_pointer);

