#pragma once

#include <types.h>
#include <idt.h>

void panic(const char *message, struct cpu_registers *regs);
