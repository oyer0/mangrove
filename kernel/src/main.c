#include <bootinfo.h>
#include <font.h>

extern char __stack_top[];
extern char __stack_bottom[];

void kmain(BOOT_INFO *BootInfo)
{
    // 1. Cast the void* framebuffer to a 32-bit pixel pointer (4 bytes per pixel)
    u32 *fb = (u32 *)BootInfo->FramebufferBase;
    
    // 2. Calculate the total number of pixels to clear
    usize total_pixels = BootInfo->FramebufferSize / sizeof(u32);

    // 3. Loop through and blast zeros (black) across the whole memory block
    for (usize i = 0; i < total_pixels; i++)
    {
        fb[i] = 0x00000000;
    }

    draw_char('M', 100, 100, 0x00FF00, BootInfo);

    // 4. Halt the CPU cleanly so it doesn't run off into the void
    for (;;)
    {
        __asm__ volatile ("hlt");
    }
}
