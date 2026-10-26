/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <stdio.h>
#include <string.h>

static int check_file(const char *path, bool check_certificates,
                      u32 *certificate_count)
{
    mg_path_info_t info;
    mg_result_t result;
    FILE *stream;
    u32 count = 0;
    unsigned char window[27] = {0};
    usize window_length = 0;
    int byte;
    int read_error;
    int close_error;

    result = path_info(path, &info);
    if (result != MG_OK || info.type != MG_PATH_TYPE_FILE ||
        info.owner_uid != 0 ||
        (info.permissions & MG_PERMISSION_OTHER_READ) == 0 ||
        (info.permissions & MG_PERMISSION_OTHER_WRITE) != 0 || info.size == 0) {
        printf("CASTORE1 metadata path=%s result=%d\n", path, result);
        return 0;
    }

    result = file_open(path, MG_OPEN_WRITE);
    if (result != MG_ERR_ACCESS_DENIED) {
        if (result > 0) (void)handle_close((mg_handle_t)result);
        printf("CASTORE1 writable path=%s result=%d\n", path, result);
        return 0;
    }

    stream = fopen(path, "rb");
    if (!stream) {
        printf("CASTORE1 unreadable path=%s\n", path);
        return 0;
    }
    while ((byte = fgetc(stream)) != EOF) {
        if (check_certificates) {
            if (window_length < sizeof(window)) {
                window[window_length++] = (unsigned char)byte;
            } else {
                memmove(window, window + 1, sizeof(window) - 1U);
                window[sizeof(window) - 1U] = (unsigned char)byte;
            }
            if (window_length == sizeof(window) &&
                memcmp(window, "-----BEGIN CERTIFICATE-----", sizeof(window)) == 0)
                count++;
        }
    }
    read_error = ferror(stream);
    close_error = fclose(stream);
    if (read_error || close_error != 0) {
        printf("CASTORE1 read-error path=%s\n", path);
        return 0;
    }
    if (check_certificates && count != 118U) {
        printf("CASTORE1 certificate-count=%u\n", count);
        return 0;
    }
    if (certificate_count) *certificate_count = count;
    return 1;
}

int main(void)
{
    static const char *const paths[] = {
        "/share/ca-certificates/ca-bundle.pem",
        "/share/ca-certificates/manifest.json",
        "/share/ca-certificates/MPL-2.0.txt",
    };
    mg_path_info_t info;
    u32 certificates = 0;

    for (usize index = 0; index < sizeof(paths) / sizeof(paths[0]); index++) {
        if (!check_file(paths[index], index == 0,
                        index == 0 ? &certificates : (u32 *)0))
            return 1;
    }
    if (file_create("/share/ca-certificates/unapproved.pem") !=
            MG_ERR_ACCESS_DENIED ||
        path_info("/share/ca-certificates/unapproved.pem", &info) !=
            MG_ERR_NOT_FOUND) {
        puts("CASTORE1 directory-writable");
        return 1;
    }

    printf("CASTORE1 PASS certificates=%u\n", certificates);
    (void)fflush(stdout);
    return 0;
}
