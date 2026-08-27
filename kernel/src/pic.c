#include <pic.h>
#include <io.h>

#define PIC1_COMMAND    0x20
#define PIC1_DATA       0x21

#define PIC2_COMMAND    0xA0
#define PIC2_DATA       0xA1

void pic_disable(void) {
    /* The APIC path owns all runtime interrupt acknowledgement.  Mask both
     * PICs directly; there is no reason to remap or otherwise initialize a
     * controller that must never deliver an interrupt. */
    outb(PIC1_DATA, 0xFF);
    io_wait();
    outb(PIC2_DATA, 0xFF);
    io_wait();
}
