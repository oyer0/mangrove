/* SPDX-License-Identifier: GPL-3.0-only */
#include <mangrove.h>
#include <mg/device_service.h>
#include <mg/error.h>
#include <mg/event.h>
#include <mg/ipc.h>
#include <mg/log_service.h>
#include <mg/volume_service.h>
#include <stdio.h>
#include <string.h>

static mg_volume_info_t volume_table[MG_VOLUME_MAX_RECORDS];
typedef struct {
    u64 instance_id;
    bool mount_failed;
    bool suppressed;
    /* vfs_mount_path() uses a runtime-only child entry under /vol.  This
     * records ownership of that namespace entry without creating a fake
     * persistent filesystem directory. */
    bool namespace_entry_owned;
    char mount_point[MG_VOLUME_MOUNT_MAX];
} volume_runtime_t;

static volume_runtime_t volume_runtime[MG_VOLUME_MAX_RECORDS];
/* Snapshot rebuilds can be entered from the IPC event/request handler, which
 * has a deeper call chain than startup.  Keep the bounded scratch tables out
 * of the service stack so a normal mount request cannot exhaust it.  Volumed
 * is single-threaded, so one shared rebuild workspace is sufficient. */
static mg_device_info_t snapshot_devices[MG_VOLUME_MAX_RECORDS];
static mg_volume_info_t snapshot_next[MG_VOLUME_MAX_RECORDS];
static volume_runtime_t snapshot_next_runtime[MG_VOLUME_MAX_RECORDS];
static u32 volume_count;
static bool have_snapshot;

#define VOLUME_AUTOMOUNT_ROOT "/vol/"
#define VOLUME_LABEL_BYTES    63U

static void log_service_failure(const char *operation, mg_result_t result)
{
    char message[MG_LOG_MESSAGE_MAX];

    if (!operation) return;
    snprintf(message, sizeof(message), "volumed stopped: %s (%s)",
             operation, error_string(result));
    (void)mg_log_submit(MG_LOG_ERROR, message);
}

static void copy_text(char *out, usize capacity, const char *text)
{
    if (!out || capacity == 0U) return;
    out[0] = '\0';
    if (!text) return;
    strncpy(out, text, capacity - 1U);
    out[capacity - 1U] = '\0';
}

static bool label_codepoint_allowed(u32 codepoint)
{
    if (codepoint < 0x20U || (codepoint >= 0x7fU && codepoint <= 0x9fU))
        return false;
    if (codepoint < 0x80U) {
        return (codepoint >= 'A' && codepoint <= 'Z') ||
               (codepoint >= 'a' && codepoint <= 'z') ||
               (codepoint >= '0' && codepoint <= '9') ||
               codepoint == ' ' || codepoint == '_' || codepoint == '-' ||
               codepoint == '.';
    }
    /* This bounded policy accepts ordinary letter/digit scripts and combining
     * marks, while excluding spaces/format controls, symbols and pictographs
     * that are unsafe or misleading as native mountpoint components. */
    if ((codepoint >= 0xa0U && codepoint <= 0xbfU) ||
        (codepoint >= 0x2000U && codepoint <= 0x2fffU) ||
        (codepoint >= 0x1f000U && codepoint <= 0x1ffffU) ||
        (codepoint >= 0xfe00U && codepoint <= 0xfeffU) ||
        (codepoint >= 0xe0000U && codepoint <= 0xe0fffU) ||
        (codepoint >= 0x202aU && codepoint <= 0x202eU) ||
        (codepoint >= 0x2066U && codepoint <= 0x2069U)) return false;
    return true;
}

static bool label_whitespace(u32 codepoint)
{
    return codepoint == 0x20U || codepoint == 0xa0U ||
           (codepoint >= 0x2000U && codepoint <= 0x200aU) ||
           codepoint == 0x2028U || codepoint == 0x2029U ||
           codepoint == 0x202fU || codepoint == 0x205fU ||
           codepoint == 0x3000U;
}

static bool label_is_usable(const char *label)
{
    usize length;
    usize offset = 0;
    u32 first = 0;
    u32 last = 0;
    bool content = false;

    if (!label || (length = strlen(label)) == 0U || length > VOLUME_LABEL_BYTES)
        return false;
    while (offset < length) {
        u8 lead = (u8)label[offset++];
        u32 codepoint;
        u32 minimum;
        u32 count;

        if (lead < 0x80U) {
            codepoint = lead;
            minimum = 0U;
            count = 1U;
        } else if (lead >= 0xc2U && lead <= 0xdfU) {
            codepoint = lead & 0x1fU;
            minimum = 0x80U;
            count = 2U;
        } else if (lead >= 0xe0U && lead <= 0xefU) {
            codepoint = lead & 0x0fU;
            minimum = 0x800U;
            count = 3U;
        } else if (lead >= 0xf0U && lead <= 0xf4U) {
            codepoint = lead & 0x07U;
            minimum = 0x10000U;
            count = 4U;
        } else return false;
        if (offset + count - 1U > length) return false;
        for (u32 index = 1U; index < count; index++) {
            u8 continuation = (u8)label[offset++];
            if ((continuation & 0xc0U) != 0x80U) return false;
            codepoint = (codepoint << 6) | (continuation & 0x3fU);
        }
        if (codepoint < minimum || codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU) ||
            !label_codepoint_allowed(codepoint)) return false;
        if (!first) first = codepoint;
        last = codepoint;
        if (!label_whitespace(codepoint)) content = true;
    }
    return content && !label_whitespace(first) && !label_whitespace(last);
}

static volume_runtime_t *runtime_for(volume_runtime_t *runtime, u32 count,
                                      u64 instance_id)
{
    if (!runtime || !instance_id) return NULL;
    for (u32 index = 0; index < count; index++)
        if (runtime[index].instance_id == instance_id) return &runtime[index];
    return NULL;
}

static const mg_device_info_t *find_device(const mg_device_info_t *devices,
                                           u32 count, u64 id)
{
    if (!devices || !id) return NULL;
    for (u32 index = 0; index < count; index++)
        if (devices[index].id == id) return &devices[index];
    return NULL;
}

static bool volume_exists(const mg_volume_info_t *volumes, u32 count, u64 id)
{
    if (!volumes || !id) return false;
    for (u32 index = 0; index < count; index++)
        if (volumes[index].instance_id == id) return true;
    return false;
}

static const mg_volume_info_t *find_volume(const mg_volume_info_t *volumes,
                                           u32 count, u64 id)
{
    if (!volumes || !id) return NULL;
    for (u32 index = 0; index < count; index++)
        if (volumes[index].instance_id == id) return &volumes[index];
    return NULL;
}

static bool make_display_name(const mg_device_info_t *device,
                              const mg_device_info_t *devices, u32 count,
                              char *out, usize capacity)
{
    u32 child_index = 0;
    int written;

    if (!device || !out || capacity < 2U) return false;
    if (device->category == MG_DEVICE_CATEGORY_BLOCK &&
        device->disk_number != 0U) {
        written = snprintf(out, capacity, "disk%u", (u32)device->disk_number);
        return written >= 0 && (usize)written < capacity;
    }
    if (device->disk_number != 0U && device->partition_number != 0U) {
        written = snprintf(out, capacity, "disk%up%u",
                           (u32)device->disk_number,
                           (u32)device->partition_number);
        return written >= 0 && (usize)written < capacity;
    }

    /* GPT supplies the normal partition number.  This deterministic fallback
     * is for a future partition source with no native number; it is scoped to
     * the current parent and snapshot order, never a persistent identity. */
    for (u32 index = 0; index < count; index++) {
        if (devices[index].category != MG_DEVICE_CATEGORY_PARTITION ||
            devices[index].parent_id != device->parent_id) continue;
        if (devices[index].id == device->id) break;
        child_index++;
    }
    if (device->disk_number != 0U) {
        written = snprintf(out, capacity, "disk%up%u",
                           (u32)device->disk_number, child_index + 1U);
        return written >= 0 && (usize)written < capacity;
    }
    copy_text(out, capacity, device->name);
    return out[0] != '\0';
}

static bool has_partition_child(const mg_device_info_t *devices, u32 count,
                                u64 instance_id)
{
    if (!devices || !instance_id) return false;
    for (u32 index = 0; index < count; index++)
        if (devices[index].category == MG_DEVICE_CATEGORY_PARTITION &&
            devices[index].parent_id == instance_id) return true;
    return false;
}

static bool supported_filesystem(const mg_volume_info_t *volume)
{
    return volume &&
           ((!strcmp(volume->filesystem, "mgfs")) ||
            (!strcmp(volume->filesystem, "fat32")) ||
            (!strcmp(volume->filesystem, "exfat")));
}

static bool mountpoint_component_safe(const char *component)
{
    if (!component || !component[0] || !strcmp(component, ".") ||
        !strcmp(component, "..")) return false;
    for (const char *cursor = component; *cursor; cursor++)
        if (*cursor == '/') return false;
    return true;
}

static const char *mountpoint_base(const mg_volume_info_t *volume)
{
    const char *base;

    if (!volume) return NULL;
    base = label_is_usable(volume->label) ? volume->label : volume->name;
    return mountpoint_component_safe(base) ? base : "volume";
}

static bool mountpoint_available(const char *path)
{
    mg_path_info_t info;
    mg_result_t result;

    if (!path) return false;
    result = path_info(path, &info);
    return result == MG_ERR_NOT_FOUND;
}

static bool choose_mountpoint(const mg_volume_info_t *volume,
                              char *out_path, usize capacity)
{
    const char *base;
    char component[MG_VOLUME_MOUNT_MAX];

    if (!volume || !out_path || capacity < 2U) return false;
    base = mountpoint_base(volume);
    if (!base) return false;
    copy_text(component, sizeof(component), base);
    for (u32 suffix = 1U; suffix <= MG_VOLUME_MAX_RECORDS; suffix++) {
        int written = suffix == 1U
            ? snprintf(out_path, capacity, VOLUME_AUTOMOUNT_ROOT "%s", component)
            : snprintf(out_path, capacity, VOLUME_AUTOMOUNT_ROOT "%s-%u",
                       component, suffix);
        if (written < 0 || (usize)written >= capacity) return false;
        if (mountpoint_available(out_path)) return true;
    }
    return false;
}

static mg_result_t read_block_snapshot(mg_device_info_t *devices,
                                        u32 capacity, u32 *out_count)
{
    u32 restart;

    if (!devices || !out_count || capacity == 0U) return MG_ERR_BAD_ARGUMENT;
    for (restart = 0; restart < 4U; restart++) {
        u32 offset = 0;
        u32 total = 0;
        u64 generation = 0;
        bool retry = false;

        while (offset < capacity) {
            mg_device_snapshot_request_t request = {0};
            u32 page_capacity = capacity - offset;
            u32 page_count;
            mg_result_t result;

            if (page_capacity > MG_DEVICE_RESPONSE_MAX)
                page_capacity = MG_DEVICE_RESPONSE_MAX;
            request.filter = MG_DEVICE_FILTER_BLOCKS;
            request.offset = offset;
            request.result_capacity = page_capacity;
            request.result = devices + offset;
            request.out_total = &total;
            request.snapshot_generation = generation;
            request.out_snapshot_generation = &generation;
            result = device_snapshot(&request);
            if (result == MG_ERR_RETRY) {
                retry = true;
                break;
            }
            if (result < 0) return result;
            page_count = (u32)result;
            if (page_count > page_capacity || total < offset + page_count)
                return MG_ERR_PROTOCOL;
            offset += page_count;
            if (offset >= total) {
                if (total > capacity) return MG_ERR_BUFFER_TOO_SMALL;
                *out_count = offset;
                return MG_OK;
            }
            if (page_count == 0U) return MG_ERR_PROTOCOL;
        }
        if (!retry && total > capacity) return MG_ERR_BUFFER_TOO_SMALL;
    }
    return MG_ERR_RETRY;
}

static void log_volume_changes(const mg_volume_info_t *old_volumes,
                               u32 old_count,
                               const mg_volume_info_t *new_volumes,
                               u32 new_count)
{
    char message[MG_LOG_MESSAGE_MAX];

    for (u32 index = 0; index < new_count; index++) {
        if (volume_exists(old_volumes, old_count,
                          new_volumes[index].instance_id)) continue;
        snprintf(message, sizeof(message), "volume discovered: %s",
                 new_volumes[index].name);
        (void)mg_log_submit(MG_LOG_INFO, message);
    }
    for (u32 index = 0; index < old_count; index++) {
        if (volume_exists(new_volumes, new_count,
                          old_volumes[index].instance_id)) continue;
        snprintf(message, sizeof(message), "volume removed: %s",
                 old_volumes[index].name);
        (void)mg_log_submit(MG_LOG_INFO, message);
    }
}

static void log_automount_failure(const mg_volume_info_t *volume,
                                  volume_runtime_t *runtime,
                                  mg_result_t result)
{
    char message[MG_LOG_MESSAGE_MAX];

    if (!volume || !runtime || runtime->mount_failed) return;
    runtime->mount_failed = true;
    snprintf(message, sizeof(message), "automount failed for %s: %s",
             volume->name, error_string(result));
    (void)mg_log_submit(MG_LOG_ERROR, message);
}

static mg_result_t mount_volume(mg_volume_info_t *volume,
                                volume_runtime_t *runtime,
                                bool explicit_request,
                                char *out_mount_point)
{
    mg_volume_mount_request_t request = {0};
    mg_path_info_t mount_info;
    mg_result_t result;
    char mount_point[MG_VOLUME_MOUNT_MAX];

    if (!volume || !runtime || (!explicit_request && runtime->mount_failed) ||
        (!explicit_request && runtime->suppressed) ||
        (volume->flags & (MG_VOLUME_FLAG_MOUNTED |
                          MG_VOLUME_FLAG_REMOVABLE |
                          MG_VOLUME_FLAG_SYSTEM_MANAGED)) !=
            MG_VOLUME_FLAG_REMOVABLE ||
        !(volume->flags & MG_VOLUME_FLAG_MOUNTABLE) ||
        !supported_filesystem(volume)) return MG_ERR_UNSUPPORTED;

    if (out_mount_point) out_mount_point[0] = '\0';
    if (explicit_request) runtime->mount_failed = false;

    /* The device snapshot is authoritative, but the mount state may be
     * published just after the snapshot page was taken.  Remember a mount
     * created by this service and confirm that its namespace entry is still
     * present before issuing another mount request.  This closes the small
     * event/reconciliation window without treating a display name or a
     * reused device number as identity. */
    if (runtime->namespace_entry_owned && runtime->mount_point[0]) {
        if (path_info(runtime->mount_point, &mount_info) == MG_OK) {
            volume->flags |= MG_VOLUME_FLAG_MOUNTED;
            copy_text(volume->mount_point, sizeof(volume->mount_point),
                      runtime->mount_point);
            if (out_mount_point)
                copy_text(out_mount_point, MG_VOLUME_MOUNT_MAX,
                          runtime->mount_point);
            return MG_OK;
        }
        runtime->namespace_entry_owned = false;
        runtime->mount_point[0] = '\0';
    }

    if (!choose_mountpoint(volume, mount_point, sizeof(mount_point))) {
        if (!explicit_request) log_automount_failure(volume, runtime, MG_ERR_BUSY);
        return MG_ERR_BUSY;
    }
    request.instance_id = volume->instance_id;
    request.parent_instance_id = volume->parent_instance_id;
    if (volume->flags & MG_VOLUME_FLAG_READ_ONLY)
        request.flags |= MG_VOLUME_MOUNT_FLAG_READ_ONLY;
    copy_text(request.filesystem, sizeof(request.filesystem),
              volume->filesystem);
    copy_text(request.mount_point, sizeof(request.mount_point), mount_point);
    result = volume_mount(&request);
    if (result == MG_ERR_ALREADY_EXISTS) {
        /* A path can become occupied between path_info() and the restricted
         * mount syscall.  Select the next deterministic suffix once. */
        for (u32 attempt = 2U; attempt <= MG_VOLUME_MAX_RECORDS; attempt++) {
            int written = snprintf(mount_point, sizeof(mount_point),
                                   VOLUME_AUTOMOUNT_ROOT "%s-%u",
                                   mountpoint_base(volume), attempt);
            if (written < 0 || (usize)written >= sizeof(mount_point)) break;
            if (!mountpoint_available(mount_point)) continue;
            copy_text(request.mount_point, sizeof(request.mount_point),
                      mount_point);
            result = volume_mount(&request);
            if (result != MG_ERR_ALREADY_EXISTS) break;
        }
    }
    if (result != MG_OK) {
        if (!explicit_request) log_automount_failure(volume, runtime, result);
        return result;
    }
    volume->flags |= MG_VOLUME_FLAG_MOUNTED;
    copy_text(volume->mount_point, sizeof(volume->mount_point), mount_point);
    runtime->suppressed = false;
    runtime->mount_failed = false;
    runtime->namespace_entry_owned = true;
    copy_text(runtime->mount_point, sizeof(runtime->mount_point), mount_point);
    if (out_mount_point)
        copy_text(out_mount_point, MG_VOLUME_MOUNT_MAX, mount_point);
    {
        char message[MG_LOG_MESSAGE_MAX];
        snprintf(message, sizeof(message), "automounted %s at %s",
                 volume->name, mount_point);
        (void)mg_log_submit(MG_LOG_INFO, message);
    }
    return MG_OK;
}

static void automount_volume(mg_volume_info_t *volume,
                             volume_runtime_t *runtime)
{
    (void)mount_volume(volume, runtime, false, NULL);
}

static mg_result_t rebuild_snapshot(bool log_changes)
{
    u32 device_count = 0;
    u32 next_count = 0;
    mg_result_t result;

    result = read_block_snapshot(snapshot_devices,
                                 sizeof(snapshot_devices) /
                                     sizeof(snapshot_devices[0]),
                                 &device_count);
    if (result != MG_OK) return result;

    memset(snapshot_next, 0, sizeof(snapshot_next));
    memset(snapshot_next_runtime, 0, sizeof(snapshot_next_runtime));
    for (u32 index = 0; index < device_count; index++) {
        const mg_device_info_t *device = &snapshot_devices[index];
        const mg_device_info_t *parent;
        mg_volume_info_t *volume;
        volume_runtime_t *runtime;
        bool is_partition = device->category == MG_DEVICE_CATEGORY_PARTITION;

        if (!is_partition &&
            (device->category != MG_DEVICE_CATEGORY_BLOCK ||
             has_partition_child(snapshot_devices, device_count, device->id)))
            continue;
        if (next_count >= MG_VOLUME_MAX_RECORDS) {
            (void)mg_log_submit(MG_LOG_WARNING,
                                "volume snapshot limit reached");
            break;
        }
        parent = find_device(snapshot_devices, device_count, device->parent_id);
        volume = &snapshot_next[next_count];
        memset(volume, 0, sizeof(*volume));
        volume->instance_id = device->id;
        volume->parent_instance_id = device->parent_id;
        volume->size_bytes = device->size_bytes;
        volume->state = MG_VOLUME_STATE_PRESENT;
        if (device->filesystem[0]) {
            volume->flags |= MG_VOLUME_FLAG_MOUNTABLE;
            copy_text(volume->filesystem, sizeof(volume->filesystem),
                      device->filesystem);
            copy_text(volume->label, sizeof(volume->label), device->label);
        }
        if (device->flags & MG_DEVICE_FLAG_MOUNTED) {
            volume->flags |= MG_VOLUME_FLAG_MOUNTED;
            copy_text(volume->mount_point, sizeof(volume->mount_point),
                      device->mount_point);
        }
        if ((device->flags & MG_DEVICE_FLAG_REMOVABLE) ||
            (parent && (parent->flags & MG_DEVICE_FLAG_REMOVABLE)))
            volume->flags |= MG_VOLUME_FLAG_REMOVABLE;
        if ((device->flags & MG_DEVICE_FLAG_SYSTEM_MANAGED) ||
            (parent && (parent->flags & MG_DEVICE_FLAG_SYSTEM_MANAGED)))
            volume->flags |= MG_VOLUME_FLAG_SYSTEM_MANAGED;
        if ((device->flags & MG_DEVICE_FLAG_READ_ONLY) ||
            (parent && (parent->flags & MG_DEVICE_FLAG_READ_ONLY)))
            volume->flags |= MG_VOLUME_FLAG_READ_ONLY;
        volume->disk_number = device->disk_number;
        volume->partition_number = device->partition_number;
        copy_text(volume->role, sizeof(volume->role), device->role);
        if (!make_display_name(device, snapshot_devices, device_count,
                               volume->name,
                               sizeof(volume->name)))
            copy_text(volume->name, sizeof(volume->name), "volume");
        runtime = &snapshot_next_runtime[next_count];
        runtime->instance_id = volume->instance_id;
        runtime->suppressed = (device->flags &
                               MG_DEVICE_FLAG_AUTOMOUNT_SUPPRESSED) != 0U;
        {
            volume_runtime_t *old_runtime = runtime_for(
                volume_runtime, volume_count, volume->instance_id);
            const mg_volume_info_t *old_volume = find_volume(
                volume_table, volume_count, volume->instance_id);
            if (old_runtime && old_volume &&
                !strcmp(old_volume->filesystem, volume->filesystem) &&
                !strcmp(old_volume->label, volume->label)) {
                runtime->mount_failed = old_runtime->mount_failed;
                if (!(device->flags & MG_DEVICE_FLAG_AUTOMOUNT_SUPPRESSED))
                    runtime->suppressed = old_runtime->suppressed;
                runtime->namespace_entry_owned =
                    old_runtime->namespace_entry_owned;
                copy_text(runtime->mount_point, sizeof(runtime->mount_point),
                          old_runtime->mount_point);
            }
        }
        next_count++;
    }

    if (log_changes && have_snapshot)
        log_volume_changes(volume_table, volume_count, snapshot_next,
                           next_count);
    for (u32 index = 0; index < next_count; index++)
        automount_volume(&snapshot_next[index], &snapshot_next_runtime[index]);
    memcpy(volume_table, snapshot_next, sizeof(snapshot_next));
    memcpy(volume_runtime, snapshot_next_runtime, sizeof(snapshot_next_runtime));
    volume_count = next_count;
    have_snapshot = true;
    return MG_OK;
}

static bool send_response(const mg_ipc_received_t *received,
                          const mg_volume_response_t *response)
{
    mg_ipc_message_t message = {0};

    if (!received || !response) return false;
    message.version = MG_IPC_PROTOCOL_VERSION;
    message.type = MG_VOLUME_RESPONSE;
    message.payload_length = sizeof(*response);
    memcpy(message.payload, response, sizeof(*response));
    return ipc_reply(received->request, &message) == MG_OK;
}

static bool send_operation_response(
    const mg_ipc_received_t *received,
    const mg_volume_operation_response_t *response)
{
    mg_ipc_message_t message = {0};

    if (!received || !response) return false;
    message.version = MG_IPC_PROTOCOL_VERSION;
    message.type = MG_VOLUME_RESPONSE;
    message.payload_length = sizeof(*response);
    memcpy(message.payload, response, sizeof(*response));
    return ipc_reply(received->request, &message) == MG_OK;
}

static mg_result_t resolve_target(const char *target, bool allow_mount_point,
                                  const mg_volume_info_t **out_volume)
{
    const mg_volume_info_t *match = NULL;
    u32 matches = 0;

    if (!target || !target[0] || !out_volume) return MG_ERR_BAD_ARGUMENT;
    if (target[0] == '/' && !allow_mount_point)
        return MG_ERR_BAD_ARGUMENT;
    for (u32 index = 0; index < volume_count; index++) {
        const mg_volume_info_t *volume = &volume_table[index];
        bool equal = !strcmp(target, volume->name);
        if (allow_mount_point && target[0] == '/' && volume->mount_point[0])
            equal = !strcmp(target, volume->mount_point);
        if (!equal) continue;
        match = volume;
        matches++;
    }
    if (matches != 1U) return matches ? MG_ERR_BAD_ARGUMENT : MG_ERR_NOT_FOUND;
    *out_volume = match;
    return MG_OK;
}

static mg_result_t resolve_eject_parent(const char *target, u64 *out_parent_id)
{
    const mg_volume_info_t *volume;
    mg_result_t result;
    u32 number = 0;
    usize index;
    bool has_number = false;

    if (!target || !out_parent_id) return MG_ERR_BAD_ARGUMENT;
    result = resolve_target(target, true, &volume);
    if (result == MG_OK) {
        *out_parent_id = volume->parent_instance_id
            ? volume->parent_instance_id : volume->instance_id;
        return MG_OK;
    }
    if (target[0] == '/' || strncmp(target, "disk", 4) != 0)
        return result;
    index = 4;
    while (target[index] >= '0' && target[index] <= '9') {
        has_number = true;
        if (number > 100000000U)
            return MG_ERR_BAD_ARGUMENT;
        number = number * 10U + (u32)(target[index++] - '0');
    }
    if (!has_number || target[index] != '\0' || number == 0U)
        return MG_ERR_BAD_ARGUMENT;
    for (u32 item = 0; item < volume_count; item++) {
        volume = &volume_table[item];
        if (volume->disk_number != number) continue;
        *out_parent_id = volume->parent_instance_id
            ? volume->parent_instance_id : volume->instance_id;
        return MG_OK;
    }
    return MG_ERR_NOT_FOUND;
}

static bool volume_belongs_to_parent(const mg_volume_info_t *volume,
                                     u64 parent_id)
{
    return volume && parent_id &&
           (volume->instance_id == parent_id ||
            volume->parent_instance_id == parent_id);
}

static void log_manual_volume_result(const char *operation,
                                     const mg_volume_info_t *volume,
                                     mg_result_t result)
{
    char message[MG_LOG_MESSAGE_MAX];

    if (!operation || !volume) return;
    if (result == MG_OK)
        snprintf(message, sizeof(message), "manual %s completed: %s",
                 operation, volume->name);
    else
        snprintf(message, sizeof(message), "manual %s failed for %s: %s",
                 operation, volume->name, error_string(result));
    (void)mg_log_submit(result == MG_OK ? MG_LOG_INFO : MG_LOG_WARNING,
                        message);
}

static mg_result_t set_volume_suppressed(const mg_volume_info_t *volume,
                                         bool suppressed)
{
    mg_volume_suppression_request_t request = {0};

    if (!volume) return MG_ERR_BAD_ARGUMENT;
    request.instance_id = volume->instance_id;
    request.parent_instance_id = volume->parent_instance_id;
    request.suppressed = suppressed ? 1U : 0U;
    return volume_set_suppressed(&request);
}

static mg_result_t handle_mount_request(const mg_ipc_received_t *received,
                                        mg_volume_operation_response_t *response,
                                        const mg_volume_request_t *request)
{
    const mg_volume_info_t *volume;
    volume_runtime_t *runtime;
    mg_result_t result;
    char mount_point[MG_VOLUME_MOUNT_MAX];

    result = rebuild_snapshot(false);
    if (result != MG_OK) return result;
    result = resolve_target(request->target, false, &volume);
    if (result != MG_OK) return result;
    response->instance_id = volume->instance_id;
    if (volume->flags & MG_VOLUME_FLAG_MOUNTED) {
        response->flags |= MG_VOLUME_RESULT_ALREADY_MOUNTED;
        copy_text(response->mount_point, sizeof(response->mount_point),
                  volume->mount_point);
        return MG_OK;
    }
    if ((volume->flags & MG_VOLUME_FLAG_REMOVABLE) == 0U ||
        (volume->flags & MG_VOLUME_FLAG_SYSTEM_MANAGED) != 0U ||
        !(volume->flags & MG_VOLUME_FLAG_MOUNTABLE) ||
        !supported_filesystem(volume)) return MG_ERR_UNSUPPORTED;
    result = pass_authorize_volume_request(received->request,
                                            MG_VOLUME_OP_MOUNT);
    if (result != MG_OK) return result;
    runtime = runtime_for(volume_runtime, volume_count, volume->instance_id);
    if (!runtime) return MG_ERR_RETRY;
    result = mount_volume((mg_volume_info_t *)volume, runtime, true,
                          mount_point);
    if (result != MG_OK) {
        log_manual_volume_result("mount", volume, result);
        return result;
    }
    response->instance_id = volume->instance_id;
    copy_text(response->mount_point, sizeof(response->mount_point),
              mount_point);
    log_manual_volume_result("mount", volume, MG_OK);
    return rebuild_snapshot(false);
}

static mg_result_t handle_unmount_request(const mg_ipc_received_t *received,
                                          mg_volume_operation_response_t *response,
                                          const mg_volume_request_t *request)
{
    const mg_volume_info_t *volume;
    mg_volume_unmount_request_t unmount = {0};
    mg_result_t result;

    result = rebuild_snapshot(false);
    if (result != MG_OK) return result;
    result = resolve_target(request->target, true, &volume);
    if (result != MG_OK) return result;
    if (!(volume->flags & MG_VOLUME_FLAG_MOUNTED)) return MG_ERR_NOT_FOUND;
    response->instance_id = volume->instance_id;
    result = pass_authorize_volume_request(received->request,
                                            MG_VOLUME_OP_UNMOUNT);
    if (result != MG_OK) return result;
    unmount.instance_id = volume->instance_id;
    unmount.parent_instance_id = volume->parent_instance_id;
    result = volume_unmount(&unmount);
    if (result != MG_OK) {
        log_manual_volume_result("unmount", volume, result);
        return result;
    }
    result = set_volume_suppressed(volume, true);
    if (result != MG_OK) {
        log_manual_volume_result("unmount", volume, result);
        return result;
    }
    log_manual_volume_result("unmount", volume, MG_OK);
    return rebuild_snapshot(false);
}

static mg_result_t handle_eject_request(const mg_ipc_received_t *received,
                                        mg_volume_operation_response_t *response,
                                        const mg_volume_request_t *request)
{
    mg_volume_unmount_request_t unmount = {0};
    u64 parent_id;
    u32 mounted_count = 0;
    bool removable = false;
    mg_result_t result;

    result = rebuild_snapshot(false);
    if (result != MG_OK) return result;
    result = resolve_eject_parent(request->target, &parent_id);
    if (result != MG_OK) return result;
    for (u32 index = 0; index < volume_count; index++) {
        const mg_volume_info_t *volume = &volume_table[index];
        if (!volume_belongs_to_parent(volume, parent_id)) continue;
        if (volume->flags & MG_VOLUME_FLAG_SYSTEM_MANAGED)
            return MG_ERR_ACCESS_DENIED;
        if (volume->flags & MG_VOLUME_FLAG_REMOVABLE) removable = true;
        if (!(volume->flags & MG_VOLUME_FLAG_MOUNTED)) continue;
        mounted_count++;
    }
    if (!removable) return MG_ERR_ACCESS_DENIED;
    response->instance_id = parent_id;
    result = pass_authorize_volume_request(received->request,
                                            MG_VOLUME_OP_EJECT);
    if (result != MG_OK) return result;

    /* Preflight every child first so a busy partition cannot cause a partial
     * eject.  The actual kernel batch operation repeats the validation and
     * flushes/detaches the whole parent as one bounded transaction. */
    for (u32 index = 0; index < volume_count; index++) {
        const mg_volume_info_t *volume = &volume_table[index];
        if (!volume_belongs_to_parent(volume, parent_id) ||
            !(volume->flags & MG_VOLUME_FLAG_MOUNTED)) continue;
        memset(&unmount, 0, sizeof(unmount));
        unmount.instance_id = volume->instance_id;
        unmount.parent_instance_id = volume->parent_instance_id;
        unmount.flags = MG_VOLUME_UNMOUNT_FLAG_PREFLIGHT;
        result = volume_unmount(&unmount);
        if (result != MG_OK) {
            log_manual_volume_result("eject", volume, result);
            return result;
        }
    }
    memset(&unmount, 0, sizeof(unmount));
    unmount.instance_id = parent_id;
    unmount.flags = MG_VOLUME_UNMOUNT_FLAG_ALL_CHILDREN;
    result = volume_unmount(&unmount);
    if (result == MG_ERR_NOT_FOUND && mounted_count == 0U)
        result = MG_OK;
    if (result != MG_OK) {
        (void)mg_log_submit(MG_LOG_WARNING,
                            "eject failed while detaching removable media");
        return result;
    }
    for (u32 index = 0; index < volume_count; index++) {
        const mg_volume_info_t *volume = &volume_table[index];
        if (!volume_belongs_to_parent(volume, parent_id)) continue;
        result = set_volume_suppressed(volume, true);
        if (result != MG_OK) return result;
    }
    (void)mg_log_submit(MG_LOG_INFO, "removable device ejected");
    return rebuild_snapshot(false);
}

static void handle_mutation_request(const mg_ipc_received_t *received)
{
    mg_volume_request_t request;
    mg_volume_operation_response_t response = {0};
    mg_result_t result;

    if (!received || received->message.payload_length != sizeof(request)) {
        response.result = MG_ERR_PROTOCOL;
        (void)send_operation_response(received, &response);
        return;
    }
    memcpy(&request, received->message.payload, sizeof(request));
    if (request.version != MG_VOLUME_PROTOCOL_VERSION ||
        request.offset != 0U || request.limit != 0U ||
        request.instance_id != 0U || !request.target[0]) {
        response.result = MG_ERR_BAD_ARGUMENT;
    } else if (request.operation == MG_VOLUME_OP_MOUNT) {
        result = handle_mount_request(received, &response, &request);
        response.result = result;
    } else if (request.operation == MG_VOLUME_OP_UNMOUNT) {
        result = handle_unmount_request(received, &response, &request);
        response.result = result;
    } else if (request.operation == MG_VOLUME_OP_EJECT) {
        result = handle_eject_request(received, &response, &request);
        response.result = result;
    } else {
        response.result = MG_ERR_BAD_ARGUMENT;
    }
    (void)send_operation_response(received, &response);
}

static void handle_request(const mg_ipc_received_t *received)
{
    mg_volume_request_t request;
    mg_volume_response_t response = {0};
    u32 count;

    if (!received || received->message.type != MG_VOLUME_REQUEST ||
        received->message.payload_length != sizeof(request)) {
        response.result = MG_ERR_PROTOCOL;
        (void)send_response(received, &response);
        return;
    }
    memcpy(&request, received->message.payload, sizeof(request));
    if (request.operation == MG_VOLUME_OP_MOUNT ||
        request.operation == MG_VOLUME_OP_UNMOUNT ||
        request.operation == MG_VOLUME_OP_EJECT) {
        handle_mutation_request(received);
        return;
    }
    if (request.version != MG_VOLUME_PROTOCOL_VERSION ||
        request.operation != MG_VOLUME_OP_SNAPSHOT ||
        request.limit == 0U || request.limit > MG_VOLUME_RESPONSE_MAX ||
        request.instance_id != 0U || request.target[0] != '\0') {
        response.result = MG_ERR_BAD_ARGUMENT;
    } else if (request.offset > volume_count) {
        response.result = MG_ERR_BAD_ARGUMENT;
    } else {
        count = volume_count - request.offset;
        if (count > request.limit) count = request.limit;
        response.result = MG_OK;
        response.count = count;
        response.total = volume_count;
        response.next_offset = request.offset + count < volume_count
            ? request.offset + count : 0U;
        if (count)
            memcpy(response.volumes, volume_table + request.offset,
                   count * sizeof(volume_table[0]));
    }
    (void)send_response(received, &response);
}

static void handle_event(const mg_ipc_received_t *received)
{
    mg_event_t event;
    mg_result_t result;

    if (!received || (received->delivery_kind != MG_IPC_DELIVERY_EVENT &&
                      received->delivery_kind !=
                      MG_IPC_DELIVERY_EVENT_OVERFLOW)) return;
    if (received->message.payload_length != sizeof(event)) return;
    memcpy(&event, received->message.payload, sizeof(event));
    if (event.version != MG_EVENT_PROTOCOL_VERSION) return;
    result = rebuild_snapshot(true);
    if (event.type == MG_EVENT_QUEUE_OVERFLOW) {
        (void)mg_log_submit(result == MG_OK ? MG_LOG_WARNING : MG_LOG_ERROR,
                            result == MG_OK
                                ? "volume state rebuilt after event overflow"
                                : "volume state rebuild failed after event overflow");
    }
}

int main(void)
{
    mg_handle_t endpoint = 0;
    mg_ipc_received_t received;
    mg_result_t result;

    result = service_register("volume", &endpoint);
    if (result != MG_OK) {
        log_service_failure("volume endpoint registration failed", result);
        printf("Volumed: endpoint registration failed: %s\n",
               error_string(result));
        process_exit(1);
    }
    result = ipc_event_subscribe(endpoint, MG_EVENT_CLASS_DEVICE |
                                 MG_EVENT_CLASS_BLOCK |
                                 MG_EVENT_CLASS_USB);
    if (result != MG_OK) {
        log_service_failure("event subscription failed", result);
        (void)handle_close(endpoint);
        process_exit(1);
    }
    result = rebuild_snapshot(false);
    if (result != MG_OK)
        (void)mg_log_submit(MG_LOG_ERROR,
                            "volume snapshot initialization failed");
    else
        (void)mg_log_submit(MG_LOG_INFO, "volumed started");

    for (;;) {
        result = ipc_receive(endpoint, &received);
        if (result != MG_OK) {
            log_service_failure("IPC receive failed", result);
            (void)handle_close(endpoint);
            process_exit(1);
        }
        if (received.delivery_kind == MG_IPC_DELIVERY_EVENT ||
            received.delivery_kind == MG_IPC_DELIVERY_EVENT_OVERFLOW)
            handle_event(&received);
        else {
            handle_request(&received);
            (void)handle_close(received.request);
        }
    }
}
