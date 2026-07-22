#include <framebuffer.h>

static u32 *framebuffer = 0;
static u32 width = 0;
static u32 height = 0;
static u32 pitch = 0; // Pixels per scanline

void framebuffer_init(BOOT_INFO *BootInfo)
{
    framebuffer = (u32 *)BootInfo->FramebufferBase;
    width = BootInfo->FramebufferWidth;
    height = BootInfo->FramebufferHeight;
    pitch = BootInfo->PixelsPerScanLine;
}

u32 framebuffer_width(void) { return width; }
u32 framebuffer_height(void) { return height; }
u32 framebuffer_pitch(void) { return pitch; }
void* framebuffer_ptr(void) { return (void*)framebuffer; }

void framebuffer_put_pixel(u32 x, u32 y, u32 color)
{
    if (x >= width || y >= height) return;
    framebuffer[(y * pitch) + x] = color;
}

void framebuffer_clear(u32 color)
{
    framebuffer_fill_rows(0, height, color);
}

void framebuffer_copy_rows(u32 dst_y, u32 src_y, u32 copy_height)
{
    if (dst_y >= height || src_y >= height) return;

    if (dst_y + copy_height > height) copy_height = height - dst_y;
    if (src_y + copy_height > height) copy_height = height - src_y;

    u32 *dst = framebuffer + (dst_y * pitch);
    u32 *src = framebuffer + (src_y * pitch);
    u64 total_pixels = (u64)copy_height * pitch;

    if (total_pixels == 0) return;

    if (dst_y < src_y) {
        // Forward copy (safe for shifting UP)
        // Bypasses Clang SSE auto-vectorization using native CPU block copy
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

    for (u32 y = 0; y < fill_height; y++) {
        u32 *dst = framebuffer + ((start_y + y) * pitch);
        u64 count = width;
        
        // Native CPU block fill (safe from SSE traps)
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
