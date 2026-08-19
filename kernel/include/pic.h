#pragma once

#include <types.h>

void pic_init(void);
void pic_send_eoi(unsigned char irq);
