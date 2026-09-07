/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <mg/log_service.h>
#include <mg/service.h>
#include <stdio.h>
#include <string.h>

#define LOGD_DIRECTORY_PATH "/sys/logs"
#define LOGD_STORAGE_PATH   "/sys/logs/system.log"
#define LOGD_OLD_PATH       "/sys/logs/system.log.old"
#define LOGD_RECOVERY_PATH  "/sys/logs/system.log.recover"
#define LOGD_STORAGE_MAX    (8U * 1024U * 1024U)
#define LOGD_BOOT_LINE_MAX  MG_LOG_MESSAGE_MAX
#define LOGD_ADOPTION_BATCH 16U
#define LOGD_EARLY_RECORD_MAX 8U
#define LOGD_RECOVERY_BATCH (sizeof(mg_log_persistent_record_t) * 15U)

static u64 next_sequence = 1;
static bool storage_initialized;

static mg_result_t initialize_storage(void);

static usize bounded_length(const char *text, usize capacity, bool allow_empty)
{
    if (!text || capacity == 0) return 0;
    for (usize index = 0; index < capacity; index++) {
        if (text[index] == '\0') {
            if (index == 0 && !allow_empty) return 0;
            return index;
        }
    }
    return 0;
}

static void sanitize_message(char *destination, usize capacity,
                             const char *source, usize length)
{
    usize written = 0;

    if (!destination || capacity == 0) return;
    while (source && written + 1U < capacity && length != 0) {
        char value = *source++;
        if (value == '\n' || value == '\r' || value == '\t' ||
            (u8)value < 0x20U || (u8)value == 0x7fU)
            value = ' ';
        destination[written++] = value;
        length--;
    }
    destination[written] = '\0';
}

static u32 crc32_bytes(const void *data, usize length)
{
    const u8 *bytes = (const u8 *)data;
    u32 crc = 0xffffffffU;

    while (length--) {
        crc ^= *bytes++;
        for (u32 bit = 0; bit < 8U; bit++)
            crc = (crc & 1U) ? (crc >> 1U) ^ 0xedb88320U : crc >> 1U;
    }
    return crc ^ 0xffffffffU;
}

static u32 persistent_checksum(const mg_log_persistent_record_t *record)
{
    mg_log_persistent_record_t copy;

    if (!record) return 0;
    copy = *record;
    copy.checksum = 0;
    return crc32_bytes(&copy, sizeof(copy));
}

static bool record_valid(const mg_log_persistent_record_t *record)
{
    const mg_log_record_t *value;

    if (!record || record->magic != MG_LOG_RECORD_MAGIC ||
        record->version != MG_LOG_RECORD_VERSION ||
        record->header_length != 16U ||
        record->record_length != sizeof(*record) ||
        record->checksum != persistent_checksum(record)) return false;
    value = &record->record;
    if (value->severity > MG_LOG_ERROR ||
        value->source_length == 0 ||
        value->source_length >= MG_LOG_SOURCE_MAX ||
        value->message_length == 0 ||
        value->message_length >= MG_LOG_MESSAGE_MAX ||
        value->realtime.nanoseconds >= MG_TIME_NANOSECONDS_PER_SEC)
        return false;
    if (value->source[value->source_length] != '\0' ||
        value->message[value->message_length] != '\0') return false;
    return true;
}

static mg_result_t read_exact(mg_handle_t file, void *buffer, usize length)
{
    usize offset = 0;

    while (offset < length) {
        mg_result_t result = object_read(file, (u8 *)buffer + offset,
                                          length - offset);
        if (result <= 0 || (usize)result > length - offset)
            return result < 0 ? result : MG_ERR_END_OF_FILE;
        offset += (usize)result;
    }
    return MG_OK;
}

static mg_result_t ensure_file(const char *path)
{
    mg_path_info_t info;
    mg_result_t result = path_info(path, &info);

    if (result == MG_OK)
        return info.type == MG_PATH_TYPE_FILE ? MG_OK : MG_ERR_IO;
    if (result != MG_ERR_NOT_FOUND) return result;
    result = file_create(path);
    if (result == MG_ERR_ALREADY_EXISTS) return MG_OK;
    return result;
}

static mg_result_t ensure_storage(void)
{
    mg_path_info_t info = {0};
    mg_result_t result = path_info(LOGD_DIRECTORY_PATH, &info);

    if (result == MG_ERR_NOT_FOUND)
        result = directory_create(LOGD_DIRECTORY_PATH);
    if (result != MG_OK) return result;
    if (info.type != 0 && info.type != MG_PATH_TYPE_DIRECTORY)
        return MG_ERR_NOT_DIRECTORY;
    return ensure_file(LOGD_STORAGE_PATH);
}

static void note_sequence(const mg_log_persistent_record_t *record)
{
    if (record && record->record.sequence >= next_sequence)
        next_sequence = record->record.sequence + 1U;
    if (next_sequence == 0) next_sequence = 1;
}

/* The public truncate syscall intentionally means "replace with an empty
 * file".  Recovery needs to retain a valid prefix, so rebuild that bounded
 * prefix through ordinary file handles before replacing the damaged file. */
static mg_result_t recover_file_prefix(const char *path, u64 length)
{
    u8 buffer[1024];
    mg_handle_t source;
    mg_handle_t destination;
    u64 copied = 0;
    mg_result_t result;

    if (!path) return MG_ERR_BAD_ARGUMENT;
    if (length == 0) {
        mg_handle_t file;
        result = file_open(path, MG_OPEN_RDWR);
        if (result < 0) return result;
        file = (mg_handle_t)result;
        result = file_truncate(file);
        (void)handle_close(file);
        return result;
    }
    result = path_remove(LOGD_RECOVERY_PATH);
    if (result != MG_OK && result != MG_ERR_NOT_FOUND) return result;
    result = file_create(LOGD_RECOVERY_PATH);
    if (result != MG_OK) return result;
    result = file_open(path, MG_OPEN_READ);
    if (result < 0) {
        (void)path_remove(LOGD_RECOVERY_PATH);
        return result;
    }
    source = (mg_handle_t)result;
    result = file_open(LOGD_RECOVERY_PATH, MG_OPEN_WRITE);
    if (result < 0) {
        (void)handle_close(source);
        (void)path_remove(LOGD_RECOVERY_PATH);
        return result;
    }
    destination = (mg_handle_t)result;
    while (copied < length) {
        usize chunk = (usize)(length - copied);
        mg_result_t got;
        if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
        got = object_read(source, buffer, chunk);
        if (got <= 0 || (usize)got > chunk ||
            object_write_all(destination, buffer, (usize)got) != got) {
            result = MG_ERR_IO;
            break;
        }
        copied += (usize)got;
    }
    if (copied == length) result = MG_OK;
    (void)handle_close(destination);
    (void)handle_close(source);
    if (result != MG_OK) {
        (void)path_remove(LOGD_RECOVERY_PATH);
        return result;
    }
    result = path_remove(path);
    if (result == MG_OK) result = path_move(LOGD_RECOVERY_PATH, path);
    if (result != MG_OK) (void)path_remove(LOGD_RECOVERY_PATH);
    return result;
}

/* Validate records without retaining the log in RAM.  A malformed or torn
 * suffix is treated as the end of the valid prefix and removed. */
static mg_result_t recover_file(const char *path)
{
    mg_path_info_t info;
    mg_handle_t file;
    u64 valid = 0;
    u64 limit;
    mg_result_t result;
    u8 batch[LOGD_RECOVERY_BATCH];

    result = path_info(path, &info);
    if (result == MG_ERR_NOT_FOUND) return MG_OK;
    if (result != MG_OK || info.type != MG_PATH_TYPE_FILE)
        return MG_ERR_IO;
    result = file_open(path, MG_OPEN_RDWR);
    if (result < 0) return result;
    file = (mg_handle_t)result;
    limit = info.size < LOGD_STORAGE_MAX ? info.size : LOGD_STORAGE_MAX;
    while (valid + sizeof(mg_log_persistent_record_t) <= limit) {
        u64 remaining = limit - valid;
        usize chunk = remaining < sizeof(batch) ? (usize)remaining :
                      sizeof(batch);
        result = read_exact(file, batch, chunk);
        if (result != MG_OK) break;
        for (usize offset = 0;
             offset + sizeof(mg_log_persistent_record_t) <= chunk;
             offset += sizeof(mg_log_persistent_record_t)) {
            mg_log_persistent_record_t record;
            memcpy(&record, batch + offset, sizeof(record));
            if (!record_valid(&record)) {
                result = MG_ERR_IO;
                break;
            }
            valid += sizeof(record);
            note_sequence(&record);
        }
        if (result != MG_OK) break;
        /* A short final batch may contain a torn suffix after its complete
         * records; the prefix length remains the recovery boundary. */
        if (chunk < sizeof(batch)) break;
    }
    if (valid != info.size) {
        (void)handle_close(file);
        return recover_file_prefix(path, valid);
    } else {
        result = MG_OK;
    }
    (void)handle_close(file);
    return result;
}

static mg_result_t rotate_if_needed(void)
{
    mg_path_info_t current;
    mg_path_info_t old;
    mg_result_t result = path_info(LOGD_STORAGE_PATH, &current);

    if (result != MG_OK || current.type != MG_PATH_TYPE_FILE)
        return MG_ERR_IO;
    if (current.size + sizeof(mg_log_persistent_record_t) <= LOGD_STORAGE_MAX)
        return MG_OK;

    result = path_info(LOGD_OLD_PATH, &old);
    if (result == MG_OK) {
        if (old.type != MG_PATH_TYPE_FILE || path_remove(LOGD_OLD_PATH) != MG_OK)
            return MG_ERR_IO;
    } else if (result != MG_ERR_NOT_FOUND) {
        return result;
    }
    result = path_move(LOGD_STORAGE_PATH, LOGD_OLD_PATH);
    if (result != MG_OK) return result;
    return ensure_file(LOGD_STORAGE_PATH);
}

static const char *service_source(const mg_ipc_requester_t *requester)
{
    if (!requester || !requester->system_service) return NULL;
    switch (requester->service_id) {
        case MG_SERVICE_SPROUT: return "sprout";
        case MG_SERVICE_SESSIOND: return "sessiond";
        case MG_SERVICE_LOGIND: return "logind";
        case MG_SERVICE_NETWORKD: return "networkd";
        case MG_SERVICE_DEVICED: return "deviced";
        case MG_SERVICE_VOLUMED: return "volumed";
        case MG_SERVICE_LOGD: return "logd";
        default: return NULL;
    }
}

static mg_result_t build_record(mg_log_persistent_record_t *persistent,
                                const char *source, u64 pid,
                                mg_log_severity_t severity,
                                const char *message, usize message_length,
                                bool stamp_now)
{
    mg_log_record_t *record;
    usize source_length;

    if (!persistent) return MG_ERR_BAD_ARGUMENT;
    memset(persistent, 0, sizeof(*persistent));
    record = &persistent->record;
    source_length = bounded_length(source, MG_LOG_SOURCE_MAX, false);
    if (!source_length || !message || !message_length ||
        message_length >= MG_LOG_MESSAGE_MAX || severity > MG_LOG_ERROR)
        return MG_ERR_BAD_ARGUMENT;
    if (stamp_now && mg_clock_boot_id() == 0) return MG_ERR_IO;

    record->sequence = next_sequence++;
    if (next_sequence == 0) next_sequence = 1;
    record->boot_id = mg_clock_boot_id();
    record->monotonic_ms = stamp_now ? uptime_ms() : 0;
    record->severity = severity;
    record->pid = pid;
    record->source_length = (u16)source_length;
    record->message_length = (u16)message_length;
    memcpy(record->source, source, source_length + 1U);
    sanitize_message(record->message, sizeof(record->message), message,
                     message_length);
    if (stamp_now) {
        mg_mangrove_time_t realtime;
        if (mg_clock_realtime(&realtime) == MG_OK) {
            record->realtime_available = 1;
            record->realtime = realtime;
        }
    }
    persistent->magic = MG_LOG_RECORD_MAGIC;
    persistent->version = MG_LOG_RECORD_VERSION;
    persistent->header_length = 16U;
    persistent->record_length = sizeof(*persistent);
    persistent->checksum = persistent_checksum(persistent);
    return MG_OK;
}

static mg_result_t append_records(const mg_log_persistent_record_t *records,
                                  usize count)
{
    mg_path_info_t current;
    mg_handle_t file;
    mg_result_t result;

    if (!records || count == 0 || count > LOGD_ADOPTION_BATCH)
        return MG_ERR_BAD_ARGUMENT;
    result = path_info(LOGD_STORAGE_PATH, &current);
    if (result != MG_OK || current.type != MG_PATH_TYPE_FILE)
        return MG_ERR_IO;
    if (current.size > LOGD_STORAGE_MAX ||
        (u64)count > (LOGD_STORAGE_MAX - current.size) /
            sizeof(mg_log_persistent_record_t)) {
        result = rotate_if_needed();
        if (result != MG_OK) return result;
        result = path_info(LOGD_STORAGE_PATH, &current);
        if (result != MG_OK || current.type != MG_PATH_TYPE_FILE ||
            (u64)count * sizeof(mg_log_persistent_record_t) >
                LOGD_STORAGE_MAX - current.size)
            return MG_ERR_IO;
    }
    result = file_open(LOGD_STORAGE_PATH, MG_OPEN_RDWR);
    if (result < 0) return result;
    file = (mg_handle_t)result;
    result = file_seek(file, 0, MG_SEEK_END);
    if (result == MG_OK && object_write_all(
            file, records, count * sizeof(mg_log_persistent_record_t)) !=
            (mg_result_t)(count * sizeof(mg_log_persistent_record_t)))
        result = MG_ERR_IO;
    (void)handle_close(file);
    return result;
}

static mg_result_t append_record(const char *source, u64 pid,
                                 mg_log_severity_t severity,
                                 const char *message, usize message_length,
                                 bool stamp_now)
{
    mg_log_persistent_record_t persistent;
    mg_result_t result = build_record(&persistent, source, pid, severity,
                                      message, message_length, stamp_now);

    if (result != MG_OK) return result;
    return append_records(&persistent, 1U);
}

static bool append_adoption_batch(mg_log_persistent_record_t *batch,
                                  usize *count)
{
    if (!batch || !count || *count == 0) return true;
    if (append_records(batch, *count) != MG_OK) return false;
    *count = 0;
    return true;
}

static bool current_boot_is_logged(u64 boot_id)
{
    const char *paths[] = {LOGD_OLD_PATH, LOGD_STORAGE_PATH};

    for (usize path_index = 0; path_index < 2U; path_index++) {
        mg_path_info_t info;
        mg_result_t opened;
        mg_handle_t file;
        u64 offset = 0;

        if (path_info(paths[path_index], &info) != MG_OK ||
            info.type != MG_PATH_TYPE_FILE) continue;
        opened = file_open(paths[path_index], MG_OPEN_READ);
        if (opened < 0) continue;
        file = (mg_handle_t)opened;
        while (offset + sizeof(mg_log_persistent_record_t) <= info.size) {
            mg_log_persistent_record_t record;
            if (read_exact(file, &record, sizeof(record)) != MG_OK) break;
            offset += sizeof(record);
            if (record_valid(&record) && record.record.boot_id == boot_id) {
                (void)handle_close(file);
                return true;
            }
        }
        (void)handle_close(file);
    }
    return false;
}

/* The kernel's early compatibility log has no structured timestamps.  It is
 * imported once as kernel-sourced, realtime-unavailable records.  The boot ID
 * check makes a logd restart idempotent while still distinguishing boots. */
static void adopt_early_log(void)
{
    mg_path_info_t info;
    mg_handle_t file;
    char input[256];
    char line[LOGD_BOOT_LINE_MAX];
    mg_log_persistent_record_t batch[LOGD_ADOPTION_BATCH];
    usize batch_count = 0;
    usize adopted = 0;
    usize line_length = 0;
    mg_result_t opened;

    if (current_boot_is_logged(mg_clock_boot_id()) ||
        path_info("/sys/logs/boot.log", &info) != MG_OK ||
        info.type != MG_PATH_TYPE_FILE) return;
    opened = file_open("/sys/logs/boot.log", MG_OPEN_READ);
    if (opened < 0) return;
    file = (mg_handle_t)opened;
    for (;;) {
        mg_result_t result = object_read(file, input, sizeof(input));
        if (result <= 0) break;
        for (usize index = 0; index < (usize)result; index++) {
            char value = input[index];
            if (value == '\n') {
                if (line_length && build_record(&batch[batch_count],
                                                "kernel", 0, MG_LOG_INFO,
                                                line, line_length, false) ==
                                       MG_OK) {
                    batch_count++;
                    adopted++;
                    if (batch_count == LOGD_ADOPTION_BATCH &&
                        !append_adoption_batch(batch, &batch_count)) {
                        (void)handle_close(file);
                        return;
                    }
                    if (adopted == LOGD_EARLY_RECORD_MAX) {
                        (void)append_adoption_batch(batch, &batch_count);
                        (void)handle_close(file);
                        return;
                    }
                }
                line_length = 0;
            } else if (line_length + 1U < sizeof(line)) {
                line[line_length++] = value;
                line[line_length] = '\0';
            }
        }
    }
    if (line_length && build_record(&batch[batch_count], "kernel", 0,
                                    MG_LOG_INFO, line, line_length, false) ==
                           MG_OK) {
        batch_count++;
        adopted++;
    }
    (void)append_adoption_batch(batch, &batch_count);
    (void)handle_close(file);
}

static bool send_response(const mg_ipc_received_t *received,
                          const mg_log_response_t *response)
{
    mg_ipc_message_t message = {0};

    if (!received || !response) return false;
    message.version = MG_IPC_PROTOCOL_VERSION;
    message.type = MG_LOG_RESPONSE;
    message.payload_length = sizeof(*response);
    memcpy(message.payload, response, sizeof(*response));
    return ipc_reply(received->request, &message) == MG_OK;
}

static bool query_matches(const mg_log_record_t *record,
                          const mg_log_request_t *request, u64 boot_id)
{
    usize source_length;

    if (!record || !request) return false;
    if (request->operation == MG_LOG_OP_QUERY_BOOT &&
        record->boot_id != boot_id) return false;
    source_length = bounded_length(request->source, sizeof(request->source),
                                   true);
    if (!source_length && request->source[0] != '\0') return false;
    if (source_length &&
        (source_length != record->source_length ||
         memcmp(request->source, record->source, source_length) != 0))
        return false;
    if (request->severity != MG_LOG_SEVERITY_ANY &&
        record->severity < request->severity) return false;
    return true;
}

static mg_result_t scan_query_file(const char *path,
                                   const mg_log_request_t *request,
                                   u64 boot_id, u32 *matching,
                                   u32 *skip, mg_log_response_t *response)
{
    mg_path_info_t info;
    mg_handle_t file;
    u64 position = 0;
    mg_result_t result;

    result = path_info(path, &info);
    if (result == MG_ERR_NOT_FOUND) return MG_OK;
    if (result != MG_OK || info.type != MG_PATH_TYPE_FILE) return MG_ERR_IO;
    result = file_open(path, MG_OPEN_READ);
    if (result < 0) return result;
    file = (mg_handle_t)result;
    result = MG_OK;
    while (position + sizeof(mg_log_persistent_record_t) <= info.size) {
        mg_log_persistent_record_t persistent;
        mg_log_record_t *record = &persistent.record;

        result = read_exact(file, &persistent, sizeof(persistent));
        if (result != MG_OK) break;
        position += sizeof(persistent);
        if (!record_valid(&persistent) ||
            !query_matches(record, request, boot_id)) continue;
        if (matching) (*matching)++;
        if (skip && *skip != 0) {
            (*skip)--;
            continue;
        }
        if (response && response->count < MG_LOG_QUERY_MAX)
            response->records[response->count++] = *record;
    }
    (void)handle_close(file);
    /* A complete record boundary is the normal loop exit; read_exact() has
     * already returned MG_OK for the last record.  Do not leak that helper's
     * intermediate status as the IPC operation result. */
    return (result == MG_OK || result == MG_ERR_END_OF_FILE) ? MG_OK : result;
}

static void handle_submit(const mg_ipc_received_t *received,
                          const mg_log_request_t *request)
{
    mg_log_response_t response = {0};
    const char *source = service_source(&received->requester);
    usize message_length = bounded_length(request->message,
                                          sizeof(request->message), false);

    /* Reject untrusted producers before touching the persistent store. */
    if (!source) {
        response.result = MG_ERR_PRIVILEGE_REQUIRED;
    } else if (!message_length) {
        response.result = MG_ERR_BAD_ARGUMENT;
    } else if (request->severity == MG_LOG_DEBUG) {
        /* Debug records are structurally supported but are not persisted by
         * the default policy.  Returning success keeps debug tracing best
         * effort and avoids making a producer depend on storage. */
        response.result = MG_OK;
    } else {
        mg_result_t storage_result = initialize_storage();
        if (storage_result != MG_OK) {
            response.result = storage_result;
        } else {
            response.result = append_record(source, received->requester.pid,
                                            (mg_log_severity_t)request->severity,
                                            request->message, message_length, true);
        }
    }
    (void)send_response(received, &response);
}

static void handle_query(const mg_ipc_received_t *received,
                         const mg_log_request_t *request)
{
    mg_log_response_t response = {0};
    u32 total = 0;
    u32 skip = request->offset;
    mg_result_t result;

    result = initialize_storage();
    if (result != MG_OK) {
        response.result = result;
        (void)send_response(received, &response);
        return;
    }
    result = scan_query_file(LOGD_OLD_PATH, request, mg_clock_boot_id(),
                             &total, &skip, &response);
    if (result == MG_OK)
        result = scan_query_file(LOGD_STORAGE_PATH, request, mg_clock_boot_id(),
                                 &total, &skip, &response);
    response.total = total;
    response.next_offset = request->offset + response.count < total
        ? request->offset + response.count : 0;
    response.result = result;
    (void)send_response(received, &response);
}

static void handle_request(const mg_ipc_received_t *received)
{
    mg_log_request_t request;
    mg_log_response_t response = {0};
    usize source_length;

    if (!received || received->message.type != MG_LOG_REQUEST ||
        received->message.payload_length != sizeof(request)) {
        response.result = MG_ERR_PROTOCOL;
        (void)send_response(received, &response);
        return;
    }
    memcpy(&request, received->message.payload, sizeof(request));
    source_length = bounded_length(request.source, sizeof(request.source), true);
    if (request.version != MG_LOG_PROTOCOL_VERSION ||
        (request.operation != MG_LOG_OP_SUBMIT &&
         request.operation != MG_LOG_OP_QUERY &&
         request.operation != MG_LOG_OP_QUERY_BOOT) ||
        (request.operation == MG_LOG_OP_SUBMIT &&
         (request.severity > MG_LOG_ERROR || !bounded_length(
             request.message, sizeof(request.message), false))) ||
        (request.operation != MG_LOG_OP_SUBMIT &&
         (!source_length && request.source[0] != '\0')) ||
        (request.operation != MG_LOG_OP_SUBMIT &&
         request.severity != MG_LOG_SEVERITY_ANY &&
         request.severity > MG_LOG_ERROR)) {
        response.result = MG_ERR_BAD_ARGUMENT;
        (void)send_response(received, &response);
        return;
    }
    if (request.operation == MG_LOG_OP_SUBMIT)
        handle_submit(received, &request);
    else
        handle_query(received, &request);
}

static void append_start_record(void)
{
    static const char message[] = "logging service started";
    (void)append_record("logd", 0, MG_LOG_INFO, message,
                        sizeof(message) - 1U, true);
}

/* Persistent storage is initialized on the first query/submission.  This
 * lets the logging service register early without performing MGFS metadata
 * writes while the rest of the system is still launching. */
static mg_result_t initialize_storage(void)
{
    mg_result_t result;

    if (storage_initialized) return MG_OK;
    result = ensure_storage();
    if (result != MG_OK) return result;
    (void)recover_file(LOGD_OLD_PATH);
    (void)recover_file(LOGD_STORAGE_PATH);
    adopt_early_log();
    append_start_record();
    storage_initialized = true;
    return MG_OK;
}

int main(void)
{
    mg_handle_t endpoint = 0;
    mg_ipc_received_t received;
    mg_result_t result;

    result = service_register("log", &endpoint);
    if (result != MG_OK) process_exit(1);
    for (;;) {
        result = ipc_receive(endpoint, &received);
        if (result != MG_OK) {
            (void)handle_close(endpoint);
            process_exit(1);
        }
        if (received.delivery_kind == MG_IPC_DELIVERY_REQUEST) {
            handle_request(&received);
            (void)handle_close(received.request);
        }
    }
}
