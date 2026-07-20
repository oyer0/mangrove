#include <gdt.h>

static struct gdt_entry gdt[5];
static struct gdt_ptr   gdt_pointer;

static struct tss_entry tss;
static u8 emergency_stack[4096];

extern void gdt_flush(u64 gdt_ptr_addr);
extern void tss_load(u16 tss_selector);

static void gdt_set_gate(i32 num, u32 base, u32 limit, u8 access, u8 gran) {
    gdt[num].base_low       = (base & 0xFFFF);
    gdt[num].base_middle    = (base >> 16) & 0xFF;
    gdt[num].base_high      = (base >> 24) & 0xFF;

    gdt[num].limit_low      = (limit & 0xFFFF);
    gdt[num].granularity    = (limit >> 16) & 0x0F;

    gdt[num].granularity    |= gran & 0xF0;
    gdt[num].access         = access;
}

static void gdt_set_tss(i32 num, u64 base, u32 limit) {
    struct gdt_system_entry *tss_gate = (struct gdt_system_entry *)&gdt[num];

    tss_gate->limit_low     = (limit & 0xFFFF);
    tss_gate->base_low      = (base & 0xFFFF);
    tss_gate->base_middle   = (base >> 16) & 0xFF;
    tss_gate->access        = 0x89;
    tss_gate->granularity   = (limit >> 16) & 0xFF;
    tss_gate->base_high     = (base >> 24) & 0xFF;
    tss_gate->base_highest  = (base >> 32) & 0xFFFFFFFF;
    tss_gate->reserved      = 0;
}

void gdt_init(void) {
    gdt_pointer.limit = sizeof(gdt)- 1;
    gdt_pointer.base = (u64)&gdt;

    gdt_set_gate(0, 0, 0, 0, 0);

    gdt_set_gate(1, 0, 0, 0x9A, 0x20);

    gdt_set_gate(2, 0, 0, 0x92, 0x00);

    u64 stack_top = (u64)emergency_stack + sizeof(emergency_stack);
    tss.ist1 = stack_top;
    tss.iomap_base = sizeof(tss);

    gdt_set_tss(3, (u64)&tss, sizeof(tss) - 1);

    gdt_flush((u64)&gdt_pointer);
    tss_load(0x18);
}
