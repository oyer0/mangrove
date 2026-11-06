/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gop_modes.h>

#ifndef X250_BOOT_TRACE
#define X250_BOOT_TRACE 0
#endif

/* Bound work against malformed firmware descriptors while admitting current
 * high-resolution panels. This is a framebuffer allocation policy, not a
 * display-model or mode-number limit. */
#define GOP_MAX_FRAMEBUFFER_BYTES (256ULL * 1024ULL * 1024ULL)

typedef struct {
    u32 number;
    u32 width;
    u32 height;
    u32 pitch;
    u64 pixels;
    bool valid;
    bool attempted;
} gop_candidate_t;

typedef struct {
    u32 number;
    u32 width;
    u32 height;
    u32 pixel_format;
    u32 pitch;
    u64 framebuffer_base;
    usize framebuffer_size;
} gop_active_t;

static EFI_STATUS validate_mode_info(
    const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info,
    usize info_size, u64 *required_bytes)
{
    u64 pixels;
    u64 bytes;

    if (!info || info_size < sizeof(*info))
        return EFI_COMPROMISED_DATA;
    if (info->Version != 0U)
        return EFI_COMPROMISED_DATA;
    if (info->PixelFormat != EFI_GRAPHICS_PIXEL_FORMAT_BGR_RESERVED)
        return EFI_UNSUPPORTED;
    if (!info->HorizontalResolution || !info->VerticalResolution ||
        info->PixelsPerScanLine < info->HorizontalResolution)
        return EFI_COMPROMISED_DATA;

    pixels = (u64)info->PixelsPerScanLine * info->VerticalResolution;
    if (pixels > GOP_MAX_FRAMEBUFFER_BYTES / 4ULL)
        return EFI_UNSUPPORTED;
    bytes = pixels * 4ULL;
    if (!bytes || bytes > GOP_MAX_FRAMEBUFFER_BYTES)
        return EFI_UNSUPPORTED;
    if (required_bytes)
        *required_bytes = bytes;
    return EFI_SUCCESS;
}

static EFI_STATUS read_active_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                                   gop_active_t *active)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
    EFI_STATUS status;
    u64 required_bytes;
    u64 framebuffer_end;

    if (!gop || !gop->Mode || !active)
        return EFI_INVALID_PARAMETER;
    mode = gop->Mode;
    info = mode->Info;
    status = validate_mode_info(info, mode->SizeOfInfo, &required_bytes);
    if (status != EFI_SUCCESS)
        return status;
    if (mode->Mode >= mode->MaxMode || !mode->FrameBufferBase ||
        !mode->FrameBufferSize ||
        (u64)mode->FrameBufferSize < required_bytes ||
        (u64)mode->FrameBufferSize > GOP_MAX_FRAMEBUFFER_BYTES ||
        mode->FrameBufferBase > ~(u64)0 - (u64)mode->FrameBufferSize)
        return EFI_COMPROMISED_DATA;

    framebuffer_end = mode->FrameBufferBase + (u64)mode->FrameBufferSize;
    if (framebuffer_end <= mode->FrameBufferBase)
        return EFI_COMPROMISED_DATA;

    active->number = mode->Mode;
    active->width = info->HorizontalResolution;
    active->height = info->VerticalResolution;
    active->pixel_format = info->PixelFormat;
    active->pitch = info->PixelsPerScanLine;
    active->framebuffer_base = mode->FrameBufferBase;
    active->framebuffer_size = mode->FrameBufferSize;
    return EFI_SUCCESS;
}

static bool candidate_better(const gop_candidate_t *candidate,
                             const gop_candidate_t *current)
{
    if (!current->valid)
        return true;
    if (candidate->pixels != current->pixels)
        return candidate->pixels > current->pixels;
    if (candidate->width != current->width)
        return candidate->width > current->width;
    if (candidate->height != current->height)
        return candidate->height > current->height;
    return candidate->number < current->number;
}

static void publish_active(BOOT_INFO *boot_info,
                           const gop_active_t *active)
{
    boot_info->FramebufferBase = (void *)(uintptr_t)active->framebuffer_base;
    boot_info->FramebufferPhysicalBase = active->framebuffer_base;
    boot_info->FramebufferSize = active->framebuffer_size;
    boot_info->FramebufferWidth = active->width;
    boot_info->FramebufferHeight = active->height;
    boot_info->PixelsPerScanLine = active->pitch;
#if X250_BOOT_TRACE
    boot_info->GopCurrentMode = active->number;
    boot_info->GopActivePixelFormat = active->pixel_format;
#else
    (void)active->number;
    (void)active->pixel_format;
#endif
}

static void record_mode(BOOT_INFO *boot_info, u32 number,
                        EFI_STATUS query_status,
                        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info,
                        usize info_size, EFI_STATUS validation_status,
                        EFI_STATUS free_status)
{
#if X250_BOOT_TRACE
    if (boot_info->GopModeCount < BOOT_GOP_MODE_LIMIT) {
        BOOT_GOP_MODE_INFO *entry =
            &boot_info->GopModes[boot_info->GopModeCount++];
        entry->Number = number;
        entry->QueryStatus = query_status;
        entry->ValidationStatus = validation_status;
        entry->FreeStatus = free_status;
        entry->Valid = validation_status == EFI_SUCCESS ? 1U : 0U;
        if (query_status == EFI_SUCCESS && info &&
            info_size >= sizeof(*info)) {
            entry->HorizontalResolution = info->HorizontalResolution;
            entry->VerticalResolution = info->VerticalResolution;
            entry->PixelFormat = info->PixelFormat;
            entry->PixelsPerScanLine = info->PixelsPerScanLine;
        }
    }
#else
    (void)boot_info;
    (void)number;
    (void)query_status;
    (void)info;
    (void)info_size;
    (void)validation_status;
    (void)free_status;
#endif
}

static EFI_STATUS restore_original(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                                   EFI_GRAPHICS_OUTPUT_SET_MODE set_mode,
                                   u32 original_mode,
                                   gop_active_t *active,
                                   BOOT_INFO *boot_info)
{
    EFI_STATUS status;
    EFI_STATUS active_status;

    if (!gop->Mode || gop->Mode->Mode == original_mode) {
        active_status = read_active_mode(gop, active);
        if (active_status == EFI_SUCCESS)
            return EFI_SUCCESS;
    } else if (set_mode) {
        status = set_mode(gop, original_mode);
#if X250_BOOT_TRACE
        boot_info->GopRestoreModeStatus = status;
#else
        (void)boot_info;
#endif
        active_status = read_active_mode(gop, active);
        if (active_status == EFI_SUCCESS)
            return EFI_SUCCESS;
        if (status == EFI_SUCCESS)
            return active_status;
        return status;
    }
    return active_status;
}

EFI_STATUS boot_gop_select_mode(EFI_SYSTEM_TABLE *system_table,
                                EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                                BOOT_INFO *boot_info)
{
    gop_candidate_t candidates[BOOT_GOP_SELECTION_MODE_LIMIT];
    gop_active_t original_active;
    gop_active_t active;
    EFI_GRAPHICS_OUTPUT_QUERY_MODE query_mode;
    EFI_GRAPHICS_OUTPUT_SET_MODE set_mode;
    EFI_FREE_POOL free_pool;
    EFI_STATUS original_status;
    EFI_STATUS scan_status = EFI_SUCCESS;
    u32 original_mode;
    u32 candidate_count = 0U;
    u32 scan_count;
    bool inventory_complete = true;
    bool mode_scan_truncated = false;

    if (!gop || !gop->Mode || !boot_info)
        return EFI_INVALID_PARAMETER;

    original_mode = gop->Mode->Mode;
    original_status = read_active_mode(gop, &original_active);
#if X250_BOOT_TRACE
    boot_info->GopInitialMode = original_mode;
    boot_info->GopCurrentMode = original_mode;
    boot_info->GopRequestedMode = original_mode;
    boot_info->GopSetModeAttempted = 0U;
    boot_info->GopFallbackUsed = 0U;
    boot_info->GopMaxMode = gop->Mode->MaxMode;
    boot_info->GopModeCount = 0U;
    boot_info->GopModeInventoryComplete = 0U;
    boot_info->GopActivePixelFormat = gop->Mode->Info
        ? gop->Mode->Info->PixelFormat : 0U;
    boot_info->GopSetModeStatus = EFI_NOT_FOUND;
    boot_info->GopRestoreModeStatus = EFI_NOT_FOUND;
    boot_info->GopSelectionStatus = EFI_NOT_FOUND;
#endif

    query_mode = (EFI_GRAPHICS_OUTPUT_QUERY_MODE)gop->QueryMode;
    set_mode = (EFI_GRAPHICS_OUTPUT_SET_MODE)gop->SetMode;
    free_pool = system_table && system_table->BootServices
        ? system_table->BootServices->FreePool : NULL;
    scan_count = gop->Mode->MaxMode;
    if (scan_count > BOOT_GOP_SELECTION_MODE_LIMIT) {
        scan_count = BOOT_GOP_SELECTION_MODE_LIMIT;
        inventory_complete = false;
        mode_scan_truncated = true;
    }

    if (query_mode && free_pool) {
        for (u32 number = 0U; number < scan_count; number++) {
            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
            usize info_size = 0U;
            u64 required_bytes = 0U;
            EFI_STATUS query_status = query_mode(gop, number, &info_size,
                                                  &info);
            EFI_STATUS validation_status = query_status;
            EFI_STATUS free_status = EFI_SUCCESS;

            if (query_status == EFI_SUCCESS) {
                validation_status = validate_mode_info(info, info_size,
                                                       &required_bytes);
                if (validation_status == EFI_SUCCESS &&
                    candidate_count < BOOT_GOP_SELECTION_MODE_LIMIT) {
                    gop_candidate_t *candidate =
                        &candidates[candidate_count++];
                    candidate->number = number;
                    candidate->width = info->HorizontalResolution;
                    candidate->height = info->VerticalResolution;
                    candidate->pitch = info->PixelsPerScanLine;
                    candidate->pixels =
                        (u64)candidate->width * candidate->height;
                    candidate->valid = true;
                    candidate->attempted = false;
                }
            } else {
                inventory_complete = false;
            }

            record_mode(boot_info, number, query_status, info, info_size,
                        validation_status, free_status);

            if (info) {
                free_status = free_pool(info);
#if X250_BOOT_TRACE
                if (number < BOOT_GOP_MODE_LIMIT &&
                    boot_info->GopModeCount > number)
                    boot_info->GopModes[number].FreeStatus = free_status;
#endif
                if (free_status != EFI_SUCCESS) {
                    inventory_complete = false;
                    scan_status = free_status;
                }
            }
            if (free_status != EFI_SUCCESS)
                break;
        }
    } else {
        inventory_complete = false;
    }

#if X250_BOOT_TRACE
    if (scan_count != gop->Mode->MaxMode ||
        boot_info->GopModeCount != gop->Mode->MaxMode)
        inventory_complete = false;
    boot_info->GopModeInventoryComplete = inventory_complete ? 1U : 0U;
#else
    (void)inventory_complete;
#endif

    if (original_status == EFI_SUCCESS)
        publish_active(boot_info, &original_active);

    /* Do not select from a partial list when the current framebuffer is
     * already usable: an unqueried firmware mode could rank higher. */
    if (mode_scan_truncated && original_status == EFI_SUCCESS) {
#if X250_BOOT_TRACE
        boot_info->GopRequestedMode = original_mode;
        boot_info->GopSelectionStatus = EFI_SUCCESS;
#endif
        return EFI_SUCCESS;
    }

    /* Deterministically prefer the greatest visible pixel area. Width,
     * height, and then lower mode number break ties. A currently usable mode
     * wins if it is already best, and remains the fallback if SetMode fails. */
    for (u32 attempt = 0U; attempt < candidate_count; attempt++) {
        u32 best_index = BOOT_GOP_SELECTION_MODE_LIMIT;
        for (u32 index = 0U; index < candidate_count; index++) {
            if (candidates[index].attempted)
                continue;
            if (best_index == BOOT_GOP_SELECTION_MODE_LIMIT ||
                candidate_better(&candidates[index],
                                 &candidates[best_index]))
                best_index = index;
        }
        if (best_index == BOOT_GOP_SELECTION_MODE_LIMIT)
            break;
        candidates[best_index].attempted = true;

        if (candidates[best_index].number == original_mode &&
            original_status == EFI_SUCCESS) {
            active = original_active;
            publish_active(boot_info, &active);
#if X250_BOOT_TRACE
            boot_info->GopRequestedMode = original_mode;
            boot_info->GopSelectionStatus = EFI_SUCCESS;
#endif
            return EFI_SUCCESS;
        }

        if (!set_mode || candidates[best_index].number == original_mode) {
            if (original_status == EFI_SUCCESS) {
                publish_active(boot_info, &original_active);
#if X250_BOOT_TRACE
                boot_info->GopFallbackUsed = 1U;
                boot_info->GopSelectionStatus = EFI_SUCCESS;
#endif
                return EFI_SUCCESS;
            }
            continue;
        }

#if X250_BOOT_TRACE
        boot_info->GopRequestedMode = candidates[best_index].number;
        boot_info->GopSetModeAttempted = 1U;
#endif
        EFI_STATUS set_status = set_mode(gop, candidates[best_index].number);
        if (set_status != EFI_SUCCESS)
            scan_status = set_status;
#if X250_BOOT_TRACE
        boot_info->GopSetModeStatus = set_status;
#endif
        EFI_STATUS active_status = read_active_mode(gop, &active);
        if (active_status != EFI_SUCCESS)
            scan_status = active_status;
        if (active_status == EFI_SUCCESS &&
            (set_status == EFI_SUCCESS || original_status != EFI_SUCCESS)) {
            publish_active(boot_info, &active);
#if X250_BOOT_TRACE
            boot_info->GopFallbackUsed =
                active.number != candidates[best_index].number ? 1U : 0U;
            boot_info->GopSelectionStatus = EFI_SUCCESS;
#endif
            return EFI_SUCCESS;
        }

        if (original_status == EFI_SUCCESS) {
            EFI_STATUS restore_status = restore_original(
                gop, set_mode, original_mode, &active, boot_info);
            if (restore_status == EFI_SUCCESS) {
                publish_active(boot_info, &active);
#if X250_BOOT_TRACE
                boot_info->GopFallbackUsed = 1U;
                boot_info->GopSelectionStatus = EFI_SUCCESS;
#endif
                return EFI_SUCCESS;
            }
            if (read_active_mode(gop, &active) == EFI_SUCCESS) {
                publish_active(boot_info, &active);
#if X250_BOOT_TRACE
                boot_info->GopFallbackUsed = 1U;
                boot_info->GopSelectionStatus = EFI_SUCCESS;
#endif
                return EFI_SUCCESS;
            }
            scan_status = restore_status;
            break;
        }
    }

    if (original_status == EFI_SUCCESS) {
        active = original_active;
        publish_active(boot_info, &active);
#if X250_BOOT_TRACE
        boot_info->GopCurrentMode = active.number;
        boot_info->GopSelectionStatus = EFI_SUCCESS;
#endif
        return EFI_SUCCESS;
    }

    if (scan_status == EFI_SUCCESS)
        scan_status = original_status;
#if X250_BOOT_TRACE
    boot_info->GopSelectionStatus = scan_status;
#endif
    return scan_status;
}
