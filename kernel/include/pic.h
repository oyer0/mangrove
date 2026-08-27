#pragma once

#include <types.h>

/* Keep the compatibility PIC silent while APIC routing is active. */
void pic_disable(void);
