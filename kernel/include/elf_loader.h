/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

struct process;
struct vfs_node;
int elf_load_process_node_result(struct process *process, struct vfs_node *node,
                                  uintptr_t *entry_point, uintptr_t *stack_pointer);
bool elf_load_process_node(struct process *process, struct vfs_node *node,
                           uintptr_t *entry_point, uintptr_t *stack_pointer);

bool elf_load_process(struct process *process, const char *path,
                      uintptr_t *entry_point, uintptr_t *stack_pointer);

