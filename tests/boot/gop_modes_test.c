/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <gop_modes.h>

#ifndef X250_BOOT_TRACE
#define X250_BOOT_TRACE 0
#endif

#define FIXTURE_MODE_LIMIT (BOOT_GOP_SELECTION_MODE_LIMIT + 1U)

typedef struct {
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION info;
    usize info_size;
} fixture_mode_t;

static fixture_mode_t fixture_modes[FIXTURE_MODE_LIMIT];
static EFI_GRAPHICS_OUTPUT_PROTOCOL *fixture_gop;
static u32 query_calls;
static u32 free_calls;
static u32 set_mode_calls;
static u32 set_mode_log[FIXTURE_MODE_LIMIT];
static u32 query_fail_mode;
static u32 free_fail_call;
static u32 set_fail_mode;
static u32 post_set_invalid_mode;
static u32 fixture_mode_count;

static EFI_STATUS EFIAPI query_mode(
    EFI_GRAPHICS_OUTPUT_PROTOCOL *protocol, u32 mode_number,
    usize *info_size, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **info)
{
    query_calls++;
    *info = NULL;
    *info_size = 0U;
    if (protocol != fixture_gop || mode_number >= fixture_mode_count)
        return EFI_INVALID_PARAMETER;
    if (mode_number == query_fail_mode)
        return EFI_DEVICE_ERROR;
    *info = malloc(sizeof(**info));
    if (!*info)
        return EFI_OUT_OF_RESOURCES;
    **info = fixture_modes[mode_number].info;
    *info_size = fixture_modes[mode_number].info_size;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI set_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *protocol,
                                  u32 mode_number)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode;

    assert(protocol == fixture_gop);
    assert(set_mode_calls < FIXTURE_MODE_LIMIT);
    set_mode_log[set_mode_calls++] = mode_number;
    if (mode_number == set_fail_mode)
        return EFI_DEVICE_ERROR;
    if (mode_number >= fixture_mode_count)
        return EFI_INVALID_PARAMETER;

    mode = protocol->Mode;
    mode->Mode = mode_number;
    mode->Info = &fixture_modes[mode_number].info;
    mode->SizeOfInfo = fixture_modes[mode_number].info_size;
    mode->FrameBufferBase = 0xe0000000ULL;
    mode->FrameBufferSize = (usize)mode->Info->PixelsPerScanLine *
                            mode->Info->VerticalResolution * 4U;
    if (mode_number == post_set_invalid_mode)
        mode->FrameBufferSize = 4U;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI free_pool(void *buffer)
{
    u32 call = free_calls++;
    free(buffer);
    return call == free_fail_call ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

static void set_fixture_mode(u32 number, u32 width, u32 height,
                             u32 pitch, u32 pixel_format)
{
    assert(number < FIXTURE_MODE_LIMIT);
    fixture_modes[number].info = (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION){
        .Version = 0U,
        .HorizontalResolution = width,
        .VerticalResolution = height,
        .PixelFormat = pixel_format,
        .PixelsPerScanLine = pitch
    };
    fixture_modes[number].info_size = sizeof(fixture_modes[number].info);
}

static void reset(void)
{
    query_calls = free_calls = set_mode_calls = 0U;
    query_fail_mode = set_fail_mode = post_set_invalid_mode = (u32)-1;
    free_fail_call = (u32)-1;
    fixture_mode_count = 4U;
    for (u32 index = 0U; index < FIXTURE_MODE_LIMIT; index++) {
        set_fixture_mode(index, 640U, 480U, 640U, 1U);
        set_mode_log[index] = (u32)-1;
    }

    /* Physical ThinkPad X250 GOP inventory supplied by the operator. */
    set_fixture_mode(0U, 640U, 480U, 640U, 1U);
    set_fixture_mode(1U, 1024U, 768U, 1024U, 1U);
    set_fixture_mode(2U, 800U, 600U, 800U, 1U);
    set_fixture_mode(3U, 1366U, 768U, 1376U, 1U);
}

static void setup(EFI_SYSTEM_TABLE *system, EFI_BOOT_SERVICES *services,
                  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode,
                  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *active,
                  u32 active_number)
{
    *services = (EFI_BOOT_SERVICES){.FreePool = free_pool};
    *system = (EFI_SYSTEM_TABLE){.BootServices = services};
    *mode = (EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE){
        .MaxMode = fixture_mode_count,
        .Mode = active_number,
        .Info = active,
        .SizeOfInfo = sizeof(*active),
        .FrameBufferBase = 0xe0000000ULL,
        .FrameBufferSize = (usize)active->PixelsPerScanLine *
                           active->VerticalResolution * 4U
    };
    *gop = (EFI_GRAPHICS_OUTPUT_PROTOCOL){
        .QueryMode = (void *)query_mode,
        .SetMode = (void *)set_mode,
        .Mode = mode
    };
    fixture_gop = gop;
}

static EFI_STATUS select_fixture(EFI_SYSTEM_TABLE *system,
                                 EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                                 BOOT_INFO *boot)
{
    return boot_gop_select_mode(system, gop, boot);
}

static void test_x250_prefers_native_mode(void)
{
    EFI_SYSTEM_TABLE system;
    EFI_BOOT_SERVICES services;
    EFI_GRAPHICS_OUTPUT_PROTOCOL gop;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION active = fixture_modes[0].info;
    BOOT_INFO boot = {0};

    setup(&system, &services, &gop, &mode, &active, 0U);
    assert(select_fixture(&system, &gop, &boot) == EFI_SUCCESS);
    assert(query_calls == 4U && free_calls == 4U && set_mode_calls == 1U);
    assert(set_mode_log[0] == 3U && mode.Mode == 3U);
    assert(boot.FramebufferWidth == 1366U &&
           boot.FramebufferHeight == 768U &&
           boot.PixelsPerScanLine == 1376U);
    assert(boot.FramebufferSize == 1376U * 768U * 4U);
#if X250_BOOT_TRACE
    assert(boot.GopInitialMode == 0U && boot.GopRequestedMode == 3U &&
           boot.GopCurrentMode == 3U && boot.GopSetModeAttempted == 1U &&
           boot.GopSetModeStatus == EFI_SUCCESS &&
           boot.GopSelectionStatus == EFI_SUCCESS &&
           boot.GopModeCount == 4U &&
           boot.GopModeInventoryComplete == 1U &&
           boot.GopModes[3].Valid == 1U &&
           boot.GopModes[3].PixelsPerScanLine == 1376U);
#endif
}

static void test_current_best_mode_is_not_reset(void)
{
    EFI_SYSTEM_TABLE system;
    EFI_BOOT_SERVICES services;
    EFI_GRAPHICS_OUTPUT_PROTOCOL gop;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION active = fixture_modes[3].info;
    BOOT_INFO boot = {0};

    setup(&system, &services, &gop, &mode, &active, 3U);
    assert(select_fixture(&system, &gop, &boot) == EFI_SUCCESS);
    assert(query_calls == 4U && free_calls == 4U && set_mode_calls == 0U);
    assert(mode.Mode == 3U && boot.FramebufferWidth == 1366U &&
           boot.PixelsPerScanLine == 1376U);
}

static void test_query_and_validation_failures_fall_back(void)
{
    EFI_SYSTEM_TABLE system;
    EFI_BOOT_SERVICES services;
    EFI_GRAPHICS_OUTPUT_PROTOCOL gop;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION active;
    BOOT_INFO boot = {0};

    reset();
    query_fail_mode = 3U;
    active = fixture_modes[0].info;
    setup(&system, &services, &gop, &mode, &active, 0U);
    assert(select_fixture(&system, &gop, &boot) == EFI_SUCCESS);
    assert(mode.Mode == 1U && boot.FramebufferWidth == 1024U &&
           query_calls == 4U && free_calls == 3U && set_mode_calls == 1U);
#if X250_BOOT_TRACE
    assert(boot.GopModeInventoryComplete == 0U &&
           boot.GopModes[3].QueryStatus == EFI_DEVICE_ERROR);
#endif

    reset();
    set_fixture_mode(3U, 1366U, 768U, 1365U, 1U);
    active = fixture_modes[0].info;
    setup(&system, &services, &gop, &mode, &active, 0U);
    boot = (BOOT_INFO){0};
    assert(select_fixture(&system, &gop, &boot) == EFI_SUCCESS);
    assert(mode.Mode == 1U && boot.FramebufferWidth == 1024U);
#if X250_BOOT_TRACE
    assert(boot.GopModes[3].Valid == 0U &&
           boot.GopModes[3].ValidationStatus == EFI_COMPROMISED_DATA);
#endif
}

static void test_set_failure_preserves_valid_fallback(void)
{
    EFI_SYSTEM_TABLE system;
    EFI_BOOT_SERVICES services;
    EFI_GRAPHICS_OUTPUT_PROTOCOL gop;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION active = fixture_modes[0].info;
    BOOT_INFO boot = {0};

    set_fail_mode = 3U;
    setup(&system, &services, &gop, &mode, &active, 0U);
    assert(select_fixture(&system, &gop, &boot) == EFI_SUCCESS);
    assert(set_mode_calls == 1U && set_mode_log[0] == 3U && mode.Mode == 0U);
    assert(boot.FramebufferWidth == 640U && boot.FramebufferHeight == 480U);
#if X250_BOOT_TRACE
    assert(boot.GopSetModeStatus == EFI_DEVICE_ERROR &&
           boot.GopFallbackUsed == 1U &&
           boot.GopCurrentMode == 0U &&
           boot.GopSelectionStatus == EFI_SUCCESS);
#endif
}

static void test_post_set_validation_restores_original(void)
{
    EFI_SYSTEM_TABLE system;
    EFI_BOOT_SERVICES services;
    EFI_GRAPHICS_OUTPUT_PROTOCOL gop;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION active = fixture_modes[0].info;
    BOOT_INFO boot = {0};

    post_set_invalid_mode = 3U;
    setup(&system, &services, &gop, &mode, &active, 0U);
    assert(select_fixture(&system, &gop, &boot) == EFI_SUCCESS);
    assert(set_mode_calls == 2U && set_mode_log[0] == 3U &&
           set_mode_log[1] == 0U && mode.Mode == 0U);
    assert(boot.FramebufferWidth == 640U && boot.FramebufferHeight == 480U);
#if X250_BOOT_TRACE
    assert(boot.GopRestoreModeStatus == EFI_SUCCESS &&
           boot.GopFallbackUsed == 1U);
#endif
}

static void test_invalid_current_can_select_valid_mode(void)
{
    EFI_SYSTEM_TABLE system;
    EFI_BOOT_SERVICES services;
    EFI_GRAPHICS_OUTPUT_PROTOCOL gop;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION active = fixture_modes[0].info;
    BOOT_INFO boot = {0};

    active.PixelFormat = 0U;
    setup(&system, &services, &gop, &mode, &active, 0U);
    set_fail_mode = 3U;
    assert(select_fixture(&system, &gop, &boot) == EFI_SUCCESS);
    assert(set_mode_calls == 2U && set_mode_log[0] == 3U &&
           set_mode_log[1] == 1U && mode.Mode == 1U);
    assert(boot.FramebufferWidth == 1024U && boot.PixelsPerScanLine == 1024U);
}

static void test_malformed_modes_and_unavailable_queries(void)
{
    EFI_SYSTEM_TABLE system;
    EFI_BOOT_SERVICES services;
    EFI_GRAPHICS_OUTPUT_PROTOCOL gop;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION active;
    BOOT_INFO boot = {0};

    reset();
    active = fixture_modes[0].info;
    fixture_modes[0].info.Version = 1U;
    fixture_modes[1].info.PixelFormat = 0U;
    fixture_modes[2].info.PixelsPerScanLine = 799U;
    fixture_modes[3].info.HorizontalResolution = 0U;
    setup(&system, &services, &gop, &mode, &active, 0U);
    assert(select_fixture(&system, &gop, &boot) == EFI_SUCCESS);
    assert(mode.Mode == 0U && set_mode_calls == 0U &&
           boot.FramebufferWidth == 640U);

    reset();
    active = fixture_modes[0].info;
    setup(&system, &services, &gop, &mode, &active, 0U);
    gop.QueryMode = NULL;
    assert(select_fixture(&system, &gop, &boot) == EFI_SUCCESS);
    assert(query_calls == 0U && set_mode_calls == 0U &&
           boot.FramebufferWidth == 640U);

    reset();
    active = fixture_modes[0].info;
    fixture_modes[2].info.HorizontalResolution = 0x80000000U;
    fixture_modes[2].info.VerticalResolution = 0x80000000U;
    fixture_modes[2].info.PixelsPerScanLine = 0x80000000U;
    fixture_modes[3].info_size = sizeof(fixture_modes[3].info) - 1U;
    setup(&system, &services, &gop, &mode, &active, 0U);
    boot = (BOOT_INFO){0};
    assert(select_fixture(&system, &gop, &boot) == EFI_SUCCESS);
    assert(mode.Mode == 1U && boot.FramebufferWidth == 1024U);
#if X250_BOOT_TRACE
    assert(boot.GopModes[2].ValidationStatus == EFI_UNSUPPORTED &&
           boot.GopModes[3].ValidationStatus == EFI_COMPROMISED_DATA);
#endif
}

static void test_no_usable_framebuffer_fails(void)
{
    EFI_SYSTEM_TABLE system;
    EFI_BOOT_SERVICES services;
    EFI_GRAPHICS_OUTPUT_PROTOCOL gop;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION active = fixture_modes[0].info;
    BOOT_INFO boot = {0};

    active.PixelFormat = 0U;
    for (u32 index = 0U; index < fixture_mode_count; index++)
        fixture_modes[index].info.PixelFormat = 0U;
    setup(&system, &services, &gop, &mode, &active, 0U);
    assert(select_fixture(&system, &gop, &boot) == EFI_UNSUPPORTED);
    assert(set_mode_calls == 0U);
}

static void test_pool_failure_keeps_valid_firmware_mode(void)
{
    EFI_SYSTEM_TABLE system;
    EFI_BOOT_SERVICES services;
    EFI_GRAPHICS_OUTPUT_PROTOCOL gop;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION active = fixture_modes[0].info;
    BOOT_INFO boot = {0};

    free_fail_call = 0U;
    setup(&system, &services, &gop, &mode, &active, 0U);
    assert(select_fixture(&system, &gop, &boot) == EFI_SUCCESS);
    assert(query_calls == 1U && free_calls == 1U && set_mode_calls == 0U);
    assert(mode.Mode == 0U && boot.FramebufferWidth == 640U);
}

static void test_truncated_inventory_keeps_valid_current_mode(void)
{
    EFI_SYSTEM_TABLE system;
    EFI_BOOT_SERVICES services;
    EFI_GRAPHICS_OUTPUT_PROTOCOL gop;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION active;
    BOOT_INFO boot = {0};

    reset();
    fixture_mode_count = FIXTURE_MODE_LIMIT;
    for (u32 index = 4U; index < fixture_mode_count; index++)
        set_fixture_mode(index, 640U + index, 480U, 640U + index, 1U);
    set_fixture_mode(BOOT_GOP_SELECTION_MODE_LIMIT, 8192U, 8192U,
                     8192U, 1U);
    active = fixture_modes[0].info;
    setup(&system, &services, &gop, &mode, &active, 0U);
    assert(select_fixture(&system, &gop, &boot) == EFI_SUCCESS);
    assert(query_calls == BOOT_GOP_SELECTION_MODE_LIMIT &&
           free_calls == BOOT_GOP_SELECTION_MODE_LIMIT &&
           set_mode_calls == 0U && mode.Mode == 0U &&
           boot.FramebufferWidth == 640U);
#if X250_BOOT_TRACE
    assert(boot.GopModeCount == BOOT_GOP_MODE_LIMIT &&
           boot.GopModeInventoryComplete == 0U);
#endif
}

int main(void)
{
    reset();
    test_x250_prefers_native_mode();
    reset();
    test_current_best_mode_is_not_reset();
    reset();
    test_query_and_validation_failures_fall_back();
    reset();
    test_set_failure_preserves_valid_fallback();
    reset();
    test_post_set_validation_restores_original();
    reset();
    test_invalid_current_can_select_valid_mode();
    reset();
    test_malformed_modes_and_unavailable_queries();
    reset();
    test_no_usable_framebuffer_fails();
    reset();
    test_pool_failure_keeps_valid_firmware_mode();
    reset();
    test_truncated_inventory_keeps_valid_current_mode();
    puts("GOP selection: native X250 geometry, stride, malformed modes, "
         "fallback and post-SetMode validation passed");
    return 0;
}
