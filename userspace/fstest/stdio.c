/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <mg/filesystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STDIO_PAYLOAD_SIZE 4097U

static unsigned operation;
static unsigned char expected[STDIO_PAYLOAD_SIZE];
static unsigned char observed[STDIO_PAYLOAD_SIZE];
static char text_expected[96];
static char text_observed[96];

static bool record(const char *name, long long expected_value,
                   long long observed_value)
{
    operation++;
    if (printf("OP %u %s expected=%lld observed=%lld\n", operation, name,
               expected_value, observed_value) < 0) return false;
    if (expected_value != observed_value) {
        (void)printf("FAIL %s errno=%d\n", name, errno);
        return false;
    }
    return true;
}

static void exit_witness(void)
{
    /* Intentionally omit a newline. Normal exit must flush this buffered text. */
    (void)fputs("EXIT-FLUSH", stdout);
}

static void make_paths(const char *target, const char *run,
                       char *binary, char *text, char *temporary)
{
    (void)snprintf(binary, 256, "%s/stdio-%s.bin", target, run);
    (void)snprintf(text, 256, "%s/stdio-%s.txt", target, run);
    (void)snprintf(temporary, 256, "%s/stdio-%s.tmp", target, run);
}

int fstest_stdio_phase(const char *run, const char *target)
{
    char binary_path[256], text_path[256], temporary_path[256];
    const char *prefix = "MTS-";
    const char *suffix = "|-17|3.25\n";
    usize run_length = strlen(run), suffix_length = strlen(suffix);
    usize text_length = 0;
    FILE *stream = NULL;
    FILE *temporary_stream = NULL;
    int status = 43;
    operation = 0;
    make_paths(target, run, binary_path, text_path, temporary_path);
    for (usize i = 0; i < STDIO_PAYLOAD_SIZE; i++)
        expected[i] = (unsigned char)(i * 73U + (i >> 4) * 19U + 0x5bU);
    if (run_length != 16U || run_length + strlen(prefix) + suffix_length >= sizeof(text_expected))
        return 2;
    memcpy(text_expected + text_length, prefix, strlen(prefix));
    text_length += strlen(prefix);
    memcpy(text_expected + text_length, run, run_length);
    text_length += run_length;
    memcpy(text_expected + text_length, suffix, suffix_length);
    text_length += suffix_length;

    if (atexit(exit_witness) != 0) return 2;
    if (printf("GUESTIO1 phase=stdio run=%s bytes=%u\n", run,
               STDIO_PAYLOAD_SIZE) < 0) return status;

    stream = fopen(binary_path, "w+b");
    if (!record("binary-open", 1, stream != NULL)) goto done;
    if (!record("binary-write", STDIO_PAYLOAD_SIZE,
                (long long)fwrite(expected, 1, sizeof(expected), stream))) goto done;
    if (!record("binary-flush", 0, fflush(stream))) goto done;
    if (!record("binary-seek", 0, fseek(stream, 0, SEEK_SET))) goto done;
    memset(observed, 0, sizeof(observed));
    if (!record("binary-read", STDIO_PAYLOAD_SIZE,
                (long long)fread(observed, 1, sizeof(observed), stream))) goto done;
    bool equal = memcmp(expected, observed, sizeof(expected)) == 0;
    if (!record("binary-content", 1, equal)) goto done;
    if (!record("binary-eof", 1, fgetc(stream) == EOF && feof(stream))) goto done;
    rewind(stream);
    if (!record("rewind", 0, ftell(stream))) goto done;
    mg_result_t close_result = fclose(stream);
    stream = NULL;
    if (!record("binary-close", 0, close_result)) goto done;

    stream = fopen(text_path, "wx+");
    if (!record("text-exclusive-create", 1, stream != NULL)) goto done;
    if (!record("text-write", (long long)text_length,
                fprintf(stream, "MTS-%s|%d|%.2f\n", run, -17, 3.25))) goto done;
    if (fseek(stream, 0, SEEK_SET) != 0) goto done;
    memset(text_observed, 0, sizeof(text_observed));
    if (!record("text-line-read", (long long)text_length,
                (long long)(fgets(text_observed, sizeof(text_observed), stream)
                            ? strlen(text_observed) : 0))) goto done;
    if (!record("text-content", 1,
                memcmp(text_observed, text_expected, text_length) == 0)) goto done;
    if (fseek(stream, 0, SEEK_SET) != 0) goto done;
    char scanned_run[17] = {0};
    int scanned_number = 0;
    float scanned_value = 0.0F;
    int assigned = fscanf(stream, "MTS-%16[0123456789abcdef]|%d|%f",
                          scanned_run, &scanned_number, &scanned_value);
    if (!record("formatted-scan", 3, assigned) ||
        !record("scan-values", 1,
                !strcmp(scanned_run, run) && scanned_number == -17 &&
                scanned_value == 3.25F)) goto done;
    fpos_t position;
    if (!record("fgetpos", 0, fgetpos(stream, &position))) goto done;
    if (!record("fsetpos", 0, fsetpos(stream, &position))) goto done;
    close_result = fclose(stream);
    stream = NULL;
    if (!record("text-close", 0, close_result)) goto done;

    stream = fopen(text_path, "r");
    if (!record("text-reopen", 1, stream != NULL)) goto done;
    if (!record("text-reopened-read", (long long)text_length,
                (long long)(fgets(text_observed, sizeof(text_observed), stream)
                            ? strlen(text_observed) : 0))) goto done;
    if (!record("text-reopened-content", 1,
                memcmp(text_observed, text_expected, text_length) == 0)) goto done;
    close_result = fclose(stream);
    stream = NULL;
    if (!record("text-reclose", 0, close_result)) goto done;

    FILE *collision = fopen(text_path, "wx");
    if (!record("exclusive-collision", 1,
                collision == NULL && errno == EEXIST)) {
        if (collision) (void)fclose(collision);
        goto done;
    }
    temporary_stream = fopen(temporary_path, "wx");
    if (!record("temporary-create", 1, temporary_stream != NULL)) goto done;
    close_result = fclose(temporary_stream);
    temporary_stream = NULL;
    if (!record("temporary-close", 0, close_result)) goto done;
    if (!record("rename-existing", EEXIST,
                rename(text_path, binary_path) == -1 ? errno : 0)) goto done;
    if (!record("remove", 0, remove(temporary_path))) goto done;

    mg_path_info_t info;
    if (!record("binary-stat", 0, path_info(binary_path, &info))) goto done;
    if (!record("binary-length", STDIO_PAYLOAD_SIZE, (long long)info.size)) goto done;
    if (!record("text-stat", 0, path_info(text_path, &info))) goto done;
    if (!record("text-length", (long long)text_length, (long long)info.size)) goto done;
    if (printf("GUESTIO1 complete phase=stdio run=%s ops=%u bytes=%u\n",
               run, operation, STDIO_PAYLOAD_SIZE) < 0) goto done;
    status = 0;
done:
    if (stream) (void)fclose(stream);
    if (temporary_stream) (void)fclose(temporary_stream);
    return status;
}
