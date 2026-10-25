/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <mg/error.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HEAP_PROFILE_GUEST
#include <heap_profile_diag.h>
#include <stdint.h>
#endif

#define PAYLOAD_SIZE 8193U
static unsigned char expected[PAYLOAD_SIZE];
static unsigned char observed[PAYLOAD_SIZE];
static unsigned operation;

#ifdef HEAP_PROFILE_GUEST
#define HEAP_PROFILE_DIAG_SYSCALL 95U

static int heap_profile_control(uint64_t operation_code, uint64_t phase,
                                void *snapshot, u64 snapshot_bytes)
{
    heap_profile_diag_request_t request = {
        .magic = HEAP_PROFILE_DIAG_MAGIC,
        .operation = operation_code,
        .phase = phase,
        .snapshot_address = (u64)(uintptr_t)snapshot,
        .snapshot_bytes = snapshot_bytes
    };
    register uint64_t number __asm__("rax") = HEAP_PROFILE_DIAG_SYSCALL;
    register heap_profile_diag_request_t *argument __asm__("rdi") = &request;
    __asm__ volatile("syscall"
                     : "+a"(number)
                     : "D"(argument)
                     : "rcx", "r11", "memory");
    return (int64_t)number == 0;
}

static bool heap_profile_start_workload(void)
{
    return heap_profile_control(HEAP_PROFILE_OP_MARK_PHASE, 1U, NULL, 0U);
}

static bool heap_profile_save(void)
{
    FILE *output;
    unsigned char buffer[HEAP_PROFILE_EXPORT_CHUNK_BYTES];
    const uint64_t total = sizeof(heap_profile_diag_snapshot_t) +
        HEAP_PROFILE_EVENT_CAPACITY * sizeof(heap_profile_event_t)
#ifdef HEAP_PROFILE_HYBRID_DIAGNOSTICS
        + sizeof(heap_profile_hybrid_snapshot_t)
#endif
        ;
    bool success = true;

    if (!heap_profile_control(HEAP_PROFILE_OP_FREEZE, 0U, NULL, 0U))
        return false;
    output = fopen("/temp/heap-profile.bin", "wb");
    if (!output)
        return false;

    for (uint64_t offset = 0; offset < total; ) {
        uint64_t remaining = total - offset;
        uint64_t amount = remaining < sizeof(buffer)
            ? remaining : sizeof(buffer);
        if (!heap_profile_control(HEAP_PROFILE_OP_READ, offset, buffer,
                                  amount) ||
            fwrite(buffer, 1U, (size_t)amount, output) != (size_t)amount) {
            success = false;
            break;
        }
        offset += amount;
    }
    if (fclose(output) != 0)
        success = false;
    return success;
}
#endif

static bool check(const char *step, mg_result_t want, mg_result_t got)
{
    operation++;
    if (printf("OP %u %s expected=%lld observed=%lld\n", operation, step,
               (long long)want, (long long)got) < 0) return false;
    if (want == got) return true;
    printf("FAIL %s: %s\n", step, error_string(got));
    return false;
}

static bool contents(const char *step)
{
    for (usize i = 0; i < PAYLOAD_SIZE; i++) {
        if (expected[i] != observed[i]) {
            printf("FAIL %s offset=%llu expected=%u observed=%u\n", step,
                   (unsigned long long)i, expected[i], observed[i]);
            return false;
        }
    }
    return check(step, PAYLOAD_SIZE, PAYLOAD_SIZE);
}

/* Confirm that the normal service loader survived unrelated MGFS mutations.
 * This is a bounded native inspection, not a synthetic startup marker. */
static bool deviced_running(void)
{
    static mg_process_info_t records[MG_PROCESS_SNAPSHOT_PAGE_MAX];
    u32 offset=0;
    bool found=false;
    while(offset<1024U) {
        u32 count=0,total=0;
        mg_process_snapshot_request_t request={0};
        request.offset=offset;request.result_capacity=MG_PROCESS_SNAPSHOT_PAGE_MAX;
        request.result=records;request.out_count=&count;request.out_total=&total;
        mg_result_t result=process_snapshot(&request);
        if(result!=MG_OK || count>MG_PROCESS_SNAPSHOT_PAGE_MAX || total>1024U) {
            printf("FAIL process snapshot: %s\n",error_string(result));
            return false;
        }
        for(u32 i=0;i<count;i++) {
            const mg_process_info_t *item=&records[i];
            if((item->flags&MG_PROCESS_FLAG_SYSTEM_SERVICE) &&
               item->state>=MG_PROCESS_INSPECTION_RUNNING &&
               item->state<=MG_PROCESS_INSPECTION_BLOCKED &&
               !strcmp(item->service,"deviced") &&
               !strcmp(item->executable_path,"/core/deviced")) found=true;
        }
        if(!count || offset>=total || count>=total-offset) return found;
        offset+=count;
    }
    return false;
}

int fstest_file_phase(const char *phase, const char *run, const char *target)
{
    operation = 0;
    const bool create = !strcmp(phase, "create");
    char name[64], path[256], trailing_path[256], directory_name[80];
    char directory_path[256], malformed_path[256];
    char *slash_boundary = (char *)malloc(MG_PATH_MAX_BYTES + 2U);
    if (!slash_boundary) return 43;
    snprintf(name, sizeof(name), "mgfs-%s.bin", run);
    snprintf(path, sizeof(path), "%s/%s", target, name);
    snprintf(trailing_path, sizeof(trailing_path), "%s/", path);
    snprintf(directory_name, sizeof(directory_name), "%s-dir", name);
    snprintf(directory_path, sizeof(directory_path), "%s/%s", target,
             directory_name);
    usize target_length = strlen(target);
    memcpy(malformed_path, target, target_length);
    malformed_path[target_length] = '/';
    malformed_path[target_length + 1] = (char)0xc0;
    malformed_path[target_length + 2] = 0;
    memset(slash_boundary, '/', MG_PATH_MAX_BYTES);
    slash_boundary[MG_PATH_MAX_BYTES] = 0;
    for (usize i = 0; i < PAYLOAD_SIZE; i++)
        expected[i] = (unsigned char)(i * 37U + (i >> 8) * 11U +
                                      (unsigned char)run[i % 16U]);
    if (printf("GUESTFS1 phase=%s run=%s bytes=%u\n", phase, run,
               PAYLOAD_SIZE) < 0) { free(slash_boundary); return 43; }

    mg_path_info_t info;
    mg_handle_t file = 0, directory = 0;
    mg_result_t result;
    int status = 43;
    if (!check("deviced-running",1,deviced_running())) goto done;
    if (!check("path-at-byte-limit", MG_OK, path_info(slash_boundary, &info)) ||
        !check("path-at-limit-root", MG_PATH_TYPE_DIRECTORY, info.type)) goto done;
    slash_boundary[MG_PATH_MAX_BYTES] = '/';
    slash_boundary[MG_PATH_MAX_BYTES + 1] = 0;
    if (!check("path-over-byte-limit", MG_ERR_NAME_TOO_LONG,
               path_info(slash_boundary, &info))) goto done;
    if (!check("malformed-utf8-path", MG_ERR_BAD_ARGUMENT,
               path_info(malformed_path, &info))) goto done;
    if (create) {
        snprintf(trailing_path, sizeof(trailing_path), "%s/", directory_path);
        if (!check("directory-absent", MG_ERR_NOT_FOUND,
                   path_info(directory_path, &info)) ||
            !check("directory-create-trailing", MG_OK,
                   directory_create(trailing_path)) ||
            !check("directory-trailing-stat", MG_OK,
                   path_info(trailing_path, &info)) ||
            !check("directory-trailing-type", MG_PATH_TYPE_DIRECTORY, info.type) ||
            !check("directory-remove-trailing", MG_OK,
                   path_remove(trailing_path)) ||
            !check("directory-removed", MG_ERR_NOT_FOUND,
                   path_info(directory_path, &info))) goto done;
        snprintf(trailing_path, sizeof(trailing_path), "%s/", path);
        if (!check("file-create-trailing", MG_ERR_NOT_DIRECTORY,
                   file_create(trailing_path)) ||
            !check("file-create-trailing-stat", MG_ERR_NOT_FOUND,
                   path_info(path, &info)) ||
            !check("absent", MG_ERR_NOT_FOUND, path_info(path, &info)) ||
            !check("create", MG_OK, file_create(path))) goto done;
    }
    snprintf(trailing_path, sizeof(trailing_path), "%s/", path);
    if (!check("file-trailing-stat", MG_ERR_NOT_DIRECTORY,
               path_info(trailing_path, &info)) ||
        !check("file-trailing-open", MG_ERR_NOT_DIRECTORY,
               file_open(trailing_path, MG_OPEN_READ)) ||
        !check("file-trailing-remove", MG_ERR_NOT_DIRECTORY,
               path_remove(trailing_path))) goto done;
    result = file_open(path, create ? MG_OPEN_RDWR : MG_OPEN_READ);
    if (!check("open", 1, result > 0 ? 1 : result)) goto done;
    file = (mg_handle_t)result;
    if (create && !check("write", PAYLOAD_SIZE,
                         object_write(file, expected, PAYLOAD_SIZE))) goto done;
    if (!check("seek-start", MG_OK, file_seek(file, 0, MG_SEEK_SET)) ||
        !check("read", PAYLOAD_SIZE, object_read(file, observed, PAYLOAD_SIZE)) ||
        !contents("bytes") ||
        !check("eof", 0, object_read(file, observed, 1))) goto done;

    static const u32 offsets[] = {511, 512, 4095, 4096, 8191, 8192};
    for (usize i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        if (!check("seek-boundary", MG_OK, file_seek(file, offsets[i], MG_SEEK_SET)) ||
            !check("read-boundary", 1, object_read(file, observed, 1)) ||
            !check("byte-boundary", expected[offsets[i]], observed[0])) goto done;
    }
    result = handle_close(file); file = 0;
    if (!check("close", MG_OK, result)) goto done;
    result = file_open(path, MG_OPEN_READ);
    if (!check("reopen", 1, result > 0 ? 1 : result)) goto done;
    file = (mg_handle_t)result;
    if (!check("reread", PAYLOAD_SIZE, object_read(file, observed, PAYLOAD_SIZE)) ||
        !contents("reopened-bytes")) goto done;
    result = handle_close(file); file = 0;
    if (!check("reclose", MG_OK, result) ||
        !check("stat", MG_OK, path_info(path, &info)) ||
        !check("type", MG_PATH_TYPE_FILE, info.type) ||
        !check("length", PAYLOAD_SIZE, (mg_result_t)info.size) ||
        !check("identity", 1, info.identifier != 0)) goto done;
    result = directory_open(target);
    if (!check("directory-open", 1, result > 0 ? 1 : result)) goto done;
    directory = (mg_handle_t)result;
    unsigned matches = 0, entries = 0;
    mg_directory_entry_t entry;
    while ((result = directory_read(directory, &entry)) == MG_OK) {
        if (++entries > 1024U) goto done;
        if (!strcmp(entry.name, name)) {
            matches++;
            if (!check("entry-type", MG_PATH_TYPE_FILE, entry.type) ||
                !check("entry-identity", (mg_result_t)info.identifier,
                       (mg_result_t)entry.identifier)) goto done;
        }
    }
    if (!check("directory-eof", MG_ERR_END_OF_FILE, result) ||
        !check("namespace", 1, matches)) goto done;
    result = handle_close(directory); directory = 0;
    if (!check("directory-close", MG_OK, result)) goto done;
    if (printf("GUESTFS1 complete phase=%s run=%s ops=%u identifier=%llu\n",
               phase, run, operation,
               (unsigned long long)info.identifier) < 0) goto done;
    status = 0;
done:
    if (file) (void)handle_close(file);
    if (directory) (void)handle_close(directory);
    free(slash_boundary);
    return status;
}

int fstest_volume_phase(int argc, char **argv);
int fstest_stdio_phase(const char *run, const char *target);
int fstest_time_phase(bool network_smoke);
int fstest_network_phase(void);
int fstest_math_phase(void);

static bool valid_run(const char *run)
{
    if (!run || strlen(run) != 16) return false;
    for (usize i = 0; i < 16; i++) {
        char c = run[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool fstest_target_valid(const char *target)
{
    usize length = strlen(target);
    if (!length || length > 160 || target[length-1] == '/' ||
        (strcmp(target,"/temp") && strncmp(target,"/vol/",5))) return false;
    for (usize i=0;i<length;i++) {
        char c=target[i];
        if (!((c>='a' && c<='z') || (c>='A' && c<='Z') ||
              (c>='0' && c<='9') || c=='/' || c=='-' || c=='_')) return false;
        if (c=='/' && i && target[i-1]=='/') return false;
    }
    return true;
}

static void runtime_exit_witness(void)
{
    (void)puts("CRT1 atexit");
}

int main(int argc, char **argv)
{
    int status;
#ifdef HEAP_PROFILE_GUEST
    if (!heap_profile_start_workload()) return 2;
#endif
    if (argc == 2 && !strcmp(argv[1], "time"))
        status = fstest_time_phase(false);
    else if (argc == 2 && !strcmp(argv[1], "network"))
        status = fstest_network_phase();
    else if (argc == 3 && !strcmp(argv[1], "time") &&
        !strcmp(argv[2], "--network"))
        status = fstest_time_phase(true);
    else if (argc == 2 && !strcmp(argv[1], "math"))
        status = fstest_math_phase();
    else if (argc >= 2 && !strncmp(argv[1], "volume-safe-", 12)) {
        if (atexit(runtime_exit_witness) != 0) return 2;
        status = fstest_volume_phase(argc, argv);
    } else if (argc >= 2 && !strncmp(argv[1], "volume-", 7))
        status = fstest_volume_phase(argc, argv);
    else if (argc >= 2 && !strcmp(argv[1], "stdio")) {
        const char *target = argc == 4 ? argv[3] : "/temp";
        if ((argc != 3 && argc != 4) || !valid_run(argv[2]) ||
            !fstest_target_valid(target)) return 2;
        status = fstest_stdio_phase(argv[2], target);
    } else if ((argc != 3 && argc != 4) ||
        (strcmp(argv[1], "create") && strcmp(argv[1], "verify")) ||
        !valid_run(argv[2])) return 2;
    else {
        const char *target = argc == 4 ? argv[3] : "/temp";
        if (!fstest_target_valid(target)) return 2;
        status = fstest_file_phase(argv[1], argv[2], target);
    }
#ifdef HEAP_PROFILE_GUEST
    if (!heap_profile_save()) return 2;
#endif
    return status;
}
