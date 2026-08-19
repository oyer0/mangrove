#include <framebuffer.h>
#include <heap.h>
#include <terminal.h>

static u32 *framebuffer = 0;
static u32 *backbuffer = 0;
static u32 width = 0;
static u32 height = 0;
static u32 pitch = 0; // Pixels per scanline

void framebuffer_init(BOOT_INFO *BootInfo)
{
    framebuffer = (u32 *)BootInfo->FramebufferBase;
    width = BootInfo->FramebufferWidth;
    height = BootInfo->FramebufferHeight;
    pitch = BootInfo->PixelsPerScanLine;
    backbuffer = 0;
}

void framebuffer_set_mmio(void *virtual_address)
{
    if (virtual_address) framebuffer = (u32 *)virtual_address;
}

void framebuffer_enable_backbuffer(void)
{
    if (backbuffer) return;

    u64 size = (u64)pitch * height * sizeof(u32);
    backbuffer = (u32 *)kmalloc(size);
    if (backbuffer) {
        /* Copy early boot VRAM text into the RAM backbuffer */
        u32 *src = framebuffer;
        u32 *dst = backbuffer;
        u64 total_pixels = (u64)height * pitch;
        asm volatile (
            ".intel_syntax noprefix\n\t"
            "rep movsd\n\t"
            ".att_syntax\n\t"
            : "+D" (dst), "+S" (src), "+c" (total_pixels)
            :
            : "memory"
        );
        terminal_redraw();
    }
}

u32 framebuffer_width(void) { return width; }
u32 framebuffer_height(void) { return height; }
u32 framebuffer_pitch(void) { return pitch; }
void* framebuffer_ptr(void) { return (void*)framebuffer; }

void framebuffer_put_pixel(u32 x, u32 y, u32 color)
{
    if (x >= width || y >= height) return;
    u32 *target = backbuffer ? backbuffer : framebuffer;
    target[(y * pitch) + x] = color;
}

void framebuffer_clear(u32 color)
{
    framebuffer_fill_rows(0, height, color);
    framebuffer_flush_rows(0, height);
}

void framebuffer_copy_rows(u32 dst_y, u32 src_y, u32 copy_height)
{
    if (dst_y >= height || src_y >= height) return;

    if (dst_y + copy_height > height) copy_height = height - dst_y;
    if (src_y + copy_height > height) copy_height = height - src_y;

    u32 *target = backbuffer ? backbuffer : framebuffer;
    u32 *dst = target + (dst_y * pitch);
    u32 *src = target + (src_y * pitch);
    u64 total_pixels = (u64)copy_height * pitch;

    if (total_pixels == 0) return;

    if (dst_y < src_y) {
        // Forward copy (safe for shifting UP)
        asm volatile (
            ".intel_syntax noprefix\n\t"
            "rep movsd\n\t"
            ".att_syntax\n\t"
            : "+D" (dst), "+S" (src), "+c" (total_pixels)
            :
            : "memory"
        );
    } else {
        // Backward copy (safe for shifting DOWN)
        dst += total_pixels - 1;
        src += total_pixels - 1;
        asm volatile (
            ".intel_syntax noprefix\n\t"
            "std\n\t"
            "rep movsd\n\t"
            "cld\n\t"
            ".att_syntax\n\t"
            : "+D" (dst), "+S" (src), "+c" (total_pixels)
            :
            : "memory"
        );
    }
}

void framebuffer_fill_rows(u32 start_y, u32 fill_height, u32 color)
{
    if (start_y >= height) return;
    if (start_y + fill_height > height) fill_height = height - start_y;

    u32 *target = backbuffer ? backbuffer : framebuffer;

    for (u32 y = 0; y < fill_height; y++) {
        u32 *dst = target + ((start_y + y) * pitch);
        u64 count = width;

        // Native CPU block fill
        asm volatile (
            ".intel_syntax noprefix\n\t"
            "rep stosd\n\t"
            ".att_syntax\n\t"
            : "+D" (dst), "+c" (count)
            : "a" (color)
            : "memory"
        );
    }
}

void framebuffer_flush_rows(u32 start_y, u32 flush_height)
{
    if (!backbuffer || backbuffer == framebuffer) return;
    if (start_y >= height) return;
    if (start_y + flush_height > height) flush_height = height - start_y;
    if (flush_height == 0) return;

    u32 *src = backbuffer + (start_y * pitch);
    u32 *dst = framebuffer + (start_y * pitch);
    u64 total_pixels = (u64)flush_height * pitch;

    /* Single contiguous forward copy to VRAM */
    asm volatile (
        ".intel_syntax noprefix\n\t"
        "rep movsd\n\t"
        ".att_syntax\n\t"
        : "+D" (dst), "+S" (src), "+c" (total_pixels)
        :
        : "memory"
    );
}
