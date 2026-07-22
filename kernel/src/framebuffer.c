#include <framebuffer.h>

static u32 *framebuffer = 0;

static u32 width = 0;
static u32 height = 0;
static u32 pitch = 0;

void framebuffer_init(BOOT_INFO *BootInfo)
{
    framebuffer = (u32 *)BootInfo->FramebufferBase;

    width = BootInfo->FramebufferWidth;
    height = BootInfo->FramebufferHeight;
    pitch = BootInfo->PixelsPerScanLine;
}

u32 framebuffer_width(void)
{
    return width;
}

u32 framebuffer_height(void)
{
    return height;
}

u32 framebuffer_pitch(void)
{
    return pitch;
}

void framebuffer_put_pixel(u32 x, u32 y, u32 color)
{
    if (x >= width || y >= height)
        return;

    framebuffer[(y * pitch) + x] = color;
}

void framebuffer_clear(u32 color)
{
    for (u32 y = 0; y < height; y++) {
        for (u32 x = 0; x < width; x++) {
            framebuffer[(y * pitch) + x] = color;
        }
    }
}
