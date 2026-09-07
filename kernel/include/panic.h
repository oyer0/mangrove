/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>
#include <idt.h>

void panic(const char *message);
void panic_exception(const char *message, struct cpu_registers *regs);
