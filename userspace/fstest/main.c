/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <mg/error.h>
#include <stdio.h>
#include <string.h>

#define PAYLOAD_SIZE 8193U
static unsigned char expected[PAYLOAD_SIZE];
static unsigned char observed[PAYLOAD_SIZE];
static unsigned operation;

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
    char name[64], path[256];
    snprintf(name, sizeof(name), "mgfs-%s.bin", run);
    snprintf(path, sizeof(path), "%s/%s", target, name);
    for (usize i = 0; i < PAYLOAD_SIZE; i++)
        expected[i] = (unsigned char)(i * 37U + (i >> 8) * 11U +
                                      (unsigned char)run[i % 16U]);
    if (printf("GUESTFS1 phase=%s run=%s bytes=%u\n", phase, run,
               PAYLOAD_SIZE) < 0) return 43;

    mg_path_info_t info;
    mg_handle_t file = 0, directory = 0;
    mg_result_t result;
    int status = 43;
    if (!check("deviced-running",1,deviced_running())) goto done;
    if (create) {
        if (!check("absent", MG_ERR_NOT_FOUND, path_info(path, &info)) ||
            !check("create", MG_OK, file_create(path))) goto done;
    }
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
    return status;
}

int fstest_volume_phase(int argc, char **argv);

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

int main(int argc, char **argv)
{
    if (argc >= 2 && !strncmp(argv[1], "volume-", 7))
        return fstest_volume_phase(argc, argv);
    if ((argc != 3 && argc != 4) ||
        (strcmp(argv[1], "create") && strcmp(argv[1], "verify")) ||
        strlen(argv[2]) != 16) return 2;
    for (usize i = 0; i < 16; i++) {
        char c = argv[2][i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 2;
    }
    const char *target = argc == 4 ? argv[3] : "/temp";
    if (!fstest_target_valid(target)) return 2;
    return fstest_file_phase(argv[1], argv[2], target);
}
