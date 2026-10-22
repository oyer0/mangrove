/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <mg/error.h>
#include <mg/filesystem.h>
#include <mg/object.h>
#include <mg/log_service.h>
#include <mg/pass.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern long write(int, const void *, unsigned long);

extern int probe_snprintf(char *, size_t, const char *, ...);
extern int probe_printf(const char *, ...);
extern void probe_perror(const char *);
extern char *probe_strerror(int);
extern double probe_strtod(const char *, char **);
extern float probe_strtof(const char *, char **);
extern long double probe_strtold(const char *, char **);
extern void __stdio_exit_flush(void);
extern int *probe_errno_location(void);
extern void test_write_mode(int);
extern void test_read_error(int);
extern void test_read_limit(size_t);
extern void test_fail_allocation_after(size_t);
extern void test_clear_allocation_failure(void);
extern const char *test_console_output(size_t *);
extern void test_console_input_reset(const void *, size_t);
extern size_t test_console_input_boundary_count(void);
extern size_t test_console_input_boundary_output_length(size_t);
extern bool test_console_input_boundary_secure(size_t);
extern bool test_console_secure_input(void);

static unsigned int login_attempts;
static u64 login_test_time;
static unsigned int login_autologin_calls;
static unsigned int login_session_creates;
static unsigned int login_shell_launches;
static unsigned int login_shell_waits;
static unsigned int login_session_ends;
static char login_session_username[MG_IDENTITY_USERNAME_CAPACITY];
static jmp_buf login_loop_escape;

#define main unused_logind_application_main
#define printf probe_printf
#define snprintf probe_snprintf
#include "../../userspace/logind/main.c"
#undef snprintf
#undef printf
#undef main

typedef int (*host_snprintf_t)(char *, size_t, const char *, ...);
typedef int (*host_sscanf_t)(const char *, const char *, ...);
typedef double (*host_strtod_t)(const char *, char **);
typedef float (*host_strtof_t)(const char *, char **);
typedef long double (*host_strtold_t)(const char *, char **);
static host_snprintf_t host_snprintf;
static host_sscanf_t host_sscanf;
static host_strtod_t host_strtod;
static host_strtof_t host_strtof;
static host_strtold_t host_strtold;

static void check(bool ok, const char *label)
{
    if (!ok) {
        (void)write(2, "FAIL: ", 6);
        (void)write(2, label, strlen(label));
        (void)write(2, "\n", 1);
        abort();
    }
}

mg_result_t pass_authenticate_account(const char *username,
                                      const char *password,
                                      mg_identity_t *identity)
{
    login_attempts++;
    if (login_attempts < 3U) return MG_ERR_AUTH_FAILED;
    if (!username || !password || strcmp(username, "valid") != 0 ||
        strcmp(password, "validpass") != 0 || !identity)
        return MG_ERR_AUTH_FAILED;
    identity->uid = 7U;
    strncpy(identity->username, username, sizeof(identity->username) - 1U);
    strncpy(identity->home, "/home/valid", sizeof(identity->home) - 1U);
    return MG_OK;
}

mg_result_t mg_log_submit(mg_log_severity_t severity, const char *message)
{
    (void)severity;
    return message ? MG_OK : MG_ERR_BAD_ARGUMENT;
}

u64 uptime_ms(void)
{
    login_test_time += 500U;
    return login_test_time;
}

mg_result_t process_yield(void) { return MG_OK; }

mg_result_t session_autologin_identity(mg_identity_t *identity)
{
    (void)identity;
    login_autologin_calls++;
    if (login_autologin_calls == 1U) return MG_ERR_NOT_FOUND;
    longjmp(login_loop_escape, 1);
}

mg_result_t service_lookup(const char *name, mg_handle_t *out_endpoint)
{
    if (!name || strcmp(name, "session") != 0 || !out_endpoint)
        return MG_ERR_BAD_ARGUMENT;
    *out_endpoint = 42U;
    return MG_OK;
}

mg_result_t ipc_request(mg_handle_t endpoint,
                        const mg_ipc_message_t *request,
                        mg_ipc_message_t *reply)
{
    mg_session_request_t session_request;
    mg_session_response_t response = {0};
    if (endpoint != 42U || !request || !reply ||
        request->version != MG_IPC_PROTOCOL_VERSION ||
        request->type != MG_SESSION_REQUEST ||
        request->payload_length != sizeof(session_request))
        return MG_ERR_PROTOCOL;
    memcpy(&session_request, request->payload, sizeof(session_request));
    if (session_request.operation == MG_SESSION_OP_CREATE) {
        login_session_creates++;
        strncpy(login_session_username, session_request.username,
                sizeof(login_session_username) - 1U);
        response.count = 1U;
        response.sessions[0].id = 99U;
    } else if (session_request.operation == MG_SESSION_OP_END) {
        if (session_request.session_id != 99U) return MG_ERR_PROTOCOL;
        login_session_ends++;
    } else {
        return MG_ERR_PROTOCOL;
    }
    response.result = MG_OK;
    memset(reply, 0, sizeof(*reply));
    reply->version = MG_IPC_PROTOCOL_VERSION;
    reply->type = MG_SESSION_RESPONSE;
    reply->payload_length = sizeof(response);
    memcpy(reply->payload, &response, sizeof(response));
    return MG_OK;
}

mg_result_t session_launch_shell(mg_session_id_t session_id,
                                 mg_handle_t *out_shell)
{
    if (session_id != 99U || !out_shell) return MG_ERR_BAD_ARGUMENT;
    login_shell_launches++;
    *out_shell = 7U;
    return MG_OK;
}

mg_result_t process_wait(mg_handle_t handle, i32 *status)
{
    if (handle != 7U || !status) return MG_ERR_BAD_ARGUMENT;
    login_shell_waits++;
    *status = 0;
    return MG_OK;
}

static void login_prompt_tests(void)
{
    static const char failed_flush_input[] = "secret\n";
    static const char input[] =
        "baduser\nbadpass1\nbaduser2\nbadpass2\nvalid\nvalidpass\n";
    static const char expected_prompts[6][64] = {
        "Username: ",
        "Username: baduser\nPassword: ",
        "Authentication failed.\nUsername: ",
        "Username: baduser2\nPassword: ",
        "Authentication failed.\nUsername: ",
        "Username: valid\nPassword: ",
    };
    size_t output_length = 0;
    const char *output;
    volatile size_t mask_count = 0;

    login_attempts = 0;
    login_test_time = 0;
    login_autologin_calls = login_session_creates = 0;
    login_shell_launches = login_shell_waits = login_session_ends = 0;
    memset(login_session_username, 0, sizeof(login_session_username));
    test_console_input_reset(failed_flush_input,
                             sizeof(failed_flush_input) - 1U);
    test_write_mode(-1);
    check(probe_printf("Password: ") == 10,
          "password prompt is accepted into the line buffer");
    char rejected_password[32] = {0};
    check(read_hidden_line(rejected_password, sizeof(rejected_password)) ==
              MG_ERR_IO && test_console_input_boundary_count() == 0U &&
          !test_console_secure_input(),
          "failed prompt flush prevents input and clears secure mode");
    test_write_mode(1);
    clearerr(stdout);
    check(fflush(stdout) == 0, "recover buffered prompt after output failure");

    test_console_input_reset(input, sizeof(input) - 1U);
    if (setjmp(login_loop_escape) == 0) run_login_loop();
    check(login_attempts == 3U &&
          !strcmp(login_session_username, "valid"),
          "login retries preserve identity through session creation");
    check(login_autologin_calls == 2U && login_session_creates == 1U &&
          login_shell_launches == 1U && login_shell_waits == 1U &&
          login_session_ends == 1U,
          "successful authentication launches and cleans up one shell session");
    check(test_console_input_boundary_count() == 6U,
          "login reaches six input boundaries for three attempts");

    output = test_console_output(&output_length);
    for (size_t i = 0; i < 6U; i++) {
        size_t visible = test_console_input_boundary_output_length(i);
        size_t expected_length = strlen(expected_prompts[i]);
        check(visible >= expected_length &&
              !memcmp(output + visible - expected_length,
                      expected_prompts[i], expected_length),
              "login prompt is visible before each input read");
        check(test_console_input_boundary_secure(i) == ((i & 1U) != 0U),
              "password reads alone enable secure terminal input");
    }
    check(output_length > 0U && !strstr(output, "badpass1") &&
          !strstr(output, "badpass2") && !strstr(output, "validpass"),
          "passwords never appear in terminal output");
    for (size_t i = 0; i < output_length; i++)
        if (output[i] == '*') mask_count++;
    check(mask_count == 25U,
          "password input is represented only by matching mask characters");
    check(strstr(output, "Authentication failed.\nUsername: baduser2\n") != NULL &&
          strstr(output, "Authentication failed.\nUsername: valid\n") != NULL,
          "failed-authentication message is separated from the next prompt");
}

#define REF_FORMAT(fmt, ...) do { \
    char expected[512], actual[512]; \
    int a = host_snprintf(expected, sizeof(expected), fmt, __VA_ARGS__); \
    int b = probe_snprintf(actual, sizeof(actual), fmt, __VA_ARGS__); \
    check(a == b && !memcmp(expected, actual, (size_t)a + 1U), fmt); \
} while (0)

static int call_vsnprintf(char *buffer, size_t size, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int result = vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
    return result;
}

static int call_vsprintf(char *buffer, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int result = vsprintf(buffer, format, arguments);
    va_end(arguments);
    return result;
}

static int call_vsscanf(const char *input, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int result = vsscanf(input, format, arguments);
    va_end(arguments);
    return result;
}

static int call_vfprintf(FILE *stream, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int result = vfprintf(stream, format, arguments);
    va_end(arguments);
    return result;
}

static int call_vfscanf(FILE *stream, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int result = vfscanf(stream, format, arguments);
    va_end(arguments);
    return result;
}

static int call_vprintf(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int result = vprintf(format, arguments);
    va_end(arguments);
    return result;
}

static int call_vscanf(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int result = vscanf(format, arguments);
    va_end(arguments);
    return result;
}

static void formatting_tests(void)
{
    char actual[8];
    REF_FORMAT("%hhd|%hd|%ld|%lld|%jd|%zd|%td", -128, -32768,
               LONG_MIN, LLONG_MIN, INTMAX_MIN, (ptrdiff_t)-17, (ptrdiff_t)-19);
    REF_FORMAT("%#o|%#x|%#X|%+08d|% 8d|%-9.5s|%c", 0U, 42U, 42U,
               -42, 7, "alphabet", 'Z');
    REF_FORMAT("%.0f|%#.0f|%+010.3e|%.6g|%a", 0.5, 2.5, -0.000125,
               1234567.0, 0x1.23456789abcdefp+20);
    REF_FORMAT("%F|%G|%A", INFINITY, 0.0000125, DBL_TRUE_MIN);
    REF_FORMAT("%.8Lf|%.6Lg", 1.0L / 7.0L, LDBL_MIN);
    check(call_vsnprintf(actual, sizeof(actual), "%+04d:%s", 17, "x") == 6 &&
          !strcmp(actual, "+017:x"), "vsnprintf variadic forwarding");
    check(call_vsprintf(actual, "%u", 42U) == 2 && !strcmp(actual, "42"),
          "vsprintf variadic forwarding");
    check(snprintf(actual, sizeof(actual), "%c", 'Q') == 1 &&
          !strcmp(actual, "Q"), "snprintf interface");
    check(sprintf(actual, "%d", -8) == 2 && !strcmp(actual, "-8"),
          "sprintf interface");
    int required = probe_snprintf(actual, sizeof(actual), "%s:%d", "abcd", 12);
    check(required == 7 && !memcmp(actual, "abcd:12", 7) && actual[7] == 0,
          "bounded snprintf termination");
    check(probe_snprintf(NULL, 0, "%ld", LONG_MIN) > 0,
          "zero-capacity snprintf");
    *probe_errno_location() = 0;
    check(probe_snprintf(actual, sizeof(actual), "%q", 1) == -1 &&
          *probe_errno_location() == EINVAL, "invalid conversion error");
    test_fail_allocation_after(0);
    *probe_errno_location() = 0;
    check(probe_snprintf(actual, sizeof(actual), "%f", 1.25) == -1 &&
          *probe_errno_location() == ENOMEM,
          "floating formatting allocation failure");
    test_clear_allocation_failure();

    char *end = NULL;
    double parsed = probe_strtod("-0x1.8p+2tail", &end);
    check(parsed == -6.0 && end && !strcmp(end, "tail"), "hex strtod end pointer");
    parsed = probe_strtod("1.7976931348623157e308", &end);
    check(parsed == DBL_MAX && *end == 0, "strtod DBL_MAX");
    parsed = probe_strtod("4.9406564584124654e-324", &end);
    check(parsed == DBL_TRUE_MIN && *end == 0, "strtod smallest subnormal");
    parsed = host_strtod("1.2345678901234567e-90", &end);
    char *ours_end;
    double ours = probe_strtod("1.2345678901234567e-90", &ours_end);
    check(parsed == ours && *end == 0 && *ours_end == 0, "host strtod reference");
    char *host_end, *our_end;
    float host_float = host_strtof("0x1.fffffep+127", &host_end);
    float our_float = probe_strtof("0x1.fffffep+127", &our_end);
    check(host_float == our_float && *host_end == 0 && *our_end == 0,
          "host strtof maximum finite");
    long double host_long = host_strtold("1.0000000000000000001", &host_end);
    long double our_long = probe_strtold("1.0000000000000000001", &our_end);
    check(host_long == our_long && *host_end == 0 && *our_end == 0,
          "host strtold precision");
    test_fail_allocation_after(0);
    *probe_errno_location() = 0;
    ours_end = NULL;
    check(probe_strtod("1.25", &ours_end) == 0.0 &&
          ours_end && ours_end[0] == '1' &&
          *probe_errno_location() == ENOMEM,
          "floating conversion allocation failure");
    test_clear_allocation_failure();
    uint64_t state = UINT64_C(0x7f4a7c159e3779b9);
    const char conversions[] = "fega";
    for (unsigned int i = 0; i < 300U; i++) {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        uint64_t bits = state;
        double value;
        memcpy(&value, &bits, sizeof(value));
        if (value != value) value = NAN;
        for (size_t j = 0; j < sizeof(conversions) - 1U; j++) {
            char format[16], expected[512], actual[512];
            unsigned int precision = (i + (unsigned int)j * 3U) % 10U;
            (void)host_snprintf(format, sizeof(format), "%%.%u%c", precision,
                                conversions[j]);
            int want = host_snprintf(expected, sizeof(expected), format, value);
            int got = probe_snprintf(actual, sizeof(actual), format, value);
            if (!(want == got && !memcmp(expected, actual, (size_t)want + 1U))) {
                char message[1400];
                int n = host_snprintf(message, sizeof(message),
                    "format=%s value=%a host=[%s] mangrove=[%s] counts=%d/%d\n",
                    format, value, expected, actual, want, got);
                (void)write(2, message, (unsigned long)n);
                check(false, "seeded float format reference");
            }
        }
    }
}

static void scanning_tests(void)
{
    int a = 0, b = 0;
    double d = 0.0;
    char word[8] = {0}, pair[3] = {0};
    int actual = sscanf("-42 0xff 3.125 alpha XY", "%d %i %lf %4s %2c",
                        &a, &b, &d, word, pair);
    int expected_a = 0, expected_b = 0, expected_d = 0;
    double expected_float = 0.0;
    char expected_word[8] = {0}, expected_pair[3] = {0};
    expected_d = host_sscanf("-42 0xff 3.125 alpha XY", "%d %i %lf %4s %2c",
                             &expected_a, &expected_b, &expected_float,
                             expected_word, expected_pair);
    check(actual == expected_d && a == expected_a && b == expected_b &&
          d == expected_float && !strcmp(word, expected_word) &&
          !memcmp(pair, expected_pair, 2), "integer/float/string scanning");
    unsigned value = 0;
    char letters[8] = {0};
    check(sscanf("0xfeed abc", "%x %[a-z]", &value, letters) == 2 &&
          value == 0xfeedU && !strcmp(letters, "abc"), "hex and scanset");
    int count = -1, parsed = 0;
    check(sscanf(" 17x", " %d%n", &parsed, &count) == 1 &&
          parsed == 17 && count == 3, "%n input count");
    check(sscanf("", "%d", &parsed) == EOF, "input failure before assignment");
    check(sscanf("x", "%d", &parsed) == 0, "matching failure");
    float f = 0.0F;
    check(sscanf("0x.8p+1tail", "%a", &f) == 1 && f == 1.0F,
          "hex float scanning and suffix preservation");
    char trailing = 0;
    check(sscanf("12x", "%d%c", &parsed, &trailing) == 2 && parsed == 12 &&
          trailing == 'x',
          "matching character remains available");
    parsed = 0;
    check(call_vsscanf("29", "%d", &parsed) == 1 && parsed == 29,
          "vsscanf variadic forwarding");
    char console_word[8] = {0};
    check(call_vscanf("%5s", console_word) == 1 &&
          !strcmp(console_word, "input"), "vscanf standard input");
    check(scanf(" %4s", console_word) == 1 &&
          !strcmp(console_word, "line"), "scanf standard input");
    char *long_float = malloc(65538U);
    check(long_float != NULL, "bounded float scan fixture allocation");
    memset(long_float, '1', 65537U);
    long_float[65537] = '\0';
    double ignored = 0.0;
    *probe_errno_location() = 0;
    check(sscanf(long_float, "%lf", &ignored) == 0 &&
          *probe_errno_location() == EOVERFLOW,
          "overlong float input is rejected at the bounded token limit");
    free(long_float);

    char grow_float[80];
    memset(grow_float, '1', sizeof(grow_float) - 1U);
    grow_float[sizeof(grow_float) - 1U] = '\0';
    test_fail_allocation_after(1);
    *probe_errno_location() = 0;
    check(sscanf(grow_float, "%lf", &ignored) == 0 &&
          *probe_errno_location() == ENOMEM,
          "floating scan realloc failure");
    test_clear_allocation_failure();
}

static void stream_tests(void)
{
    static const char payload[] = "stream-data-0123456789";
    const int known_errors[] = {
        EPERM, ENOENT, ESRCH, EINTR, EIO, ENXIO, E2BIG, ENOEXEC, EBADF,
        ECHILD, EAGAIN, ENOMEM, EACCES, EFAULT, EBUSY, EEXIST, EXDEV,
        ENODEV, ENOTDIR, EISDIR, EINVAL, ENFILE, EMFILE, ENOTTY, ETXTBSY,
        EFBIG, ENOSPC, ESPIPE, EROFS, EMLINK, EPIPE, EDOM, ERANGE, ENOSYS,
        ENAMETOOLONG, ENOLCK, ENOTEMPTY, ELOOP, ENOMSG, EIDRM, EOVERFLOW,
        EILSEQ, EPROTO, ETIMEDOUT, ECONNRESET, ECONNREFUSED, EADDRINUSE,
        ENETUNREACH, ENETDOWN, ENOTCONN, ECANCELED, ENODATA, ENOTSUP
    };
    for (size_t i = 0; i < sizeof(known_errors) / sizeof(known_errors[0]); i++)
        check(strcmp(probe_strerror(known_errors[i]), "Unknown error") != 0,
              "strerror describes every published errno");
    FILE *stream = fopen("/temp/stdio-stream.bin", "w+");
    check(stream != NULL, "fopen w+");
    check(fwrite(payload, 1, sizeof(payload) - 1U, stream) == sizeof(payload) - 1U,
          "buffered fwrite count");
    check(fflush(stream) == 0 && ftell(stream) == (long)sizeof(payload) - 1L,
          "flush and position");
    check(fseek(stream, 0, SEEK_SET) == 0, "seek start");
    char readback[sizeof(payload)] = {0};
    check(fread(readback, 2, (sizeof(payload) - 1U) / 2U, stream) ==
          (sizeof(payload) - 1U) / 2U, "fread complete elements");
    check(!memcmp(readback, payload, sizeof(payload) - 1U), "fread contents");
    check(fseek(stream, 0, SEEK_SET) == 0 &&
          call_vfscanf(stream, "%6[stream]", readback) == 1 &&
          !strcmp(readback, "stream"), "vfscanf stream conversion");
    check(fseek(stream, 0, SEEK_SET) == 0 &&
          call_vfprintf(stream, "%s", "stream-data-0123456789") ==
              (int)(sizeof(payload) - 1U), "vfprintf stream output");
    check(fflush(stream) == 0 && fseek(stream, 0, SEEK_SET) == 0,
          "vfprintf flush and rewind");
    memset(readback, 0, sizeof(readback));
    check(fread(readback, 1, sizeof(payload) - 1U, stream) ==
              sizeof(payload) - 1U &&
          !memcmp(readback, payload, sizeof(payload) - 1U),
          "vfprintf preserved file contents");
    check(fgetc(stream) == EOF && feof(stream), "genuine EOF state");
    check(ungetc('!', stream) == '!' && !feof(stream) && fgetc(stream) == '!',
          "ungetc clears EOF");
    clearerr(stream);
    check(fclose(stream) == 0, "fclose");

    test_fail_allocation_after(0);
    *probe_errno_location() = 0;
    check(fopen("/temp/stdio-allocation-failure.bin", "w") == NULL &&
          *probe_errno_location() == ENOMEM,
          "fopen buffer allocation failure");
    test_clear_allocation_failure();

    stream = fopen("/temp/stdio-stream.bin", "r");
    check(stream != NULL, "open setvbuf allocation fixture");
    test_fail_allocation_after(0);
    *probe_errno_location() = 0;
    check(setvbuf(stream, NULL, _IOFBF, 128U) == -1 &&
          *probe_errno_location() == ENOMEM,
          "setvbuf allocation failure");
    test_clear_allocation_failure();
    check(fgetc(stream) == 's' && fclose(stream) == 0,
          "setvbuf allocation failure preserves existing buffer");

    stream = fopen("/temp/stdio-stream.bin", "a");
    check(stream != NULL, "fopen append");
    check(fwrite("+tail", 1, 5, stream) == 5 && fflush(stream) == 0,
          "append write");
    check(fseek(stream, 0, SEEK_SET) == 0, "append stream seek");
    check(fwrite("!", 1, 1, stream) == 1 && fflush(stream) == 0,
          "append ignores seek for write");
    check(fclose(stream) == 0, "append close");
    stream = fopen("/temp/stdio-stream.bin", "r");
    check(stream != NULL, "reopen");
    char direct_scan[16] = {0};
    check(fscanf(stream, "%6s", direct_scan) == 1 &&
          !strcmp(direct_scan, "stream"), "fscanf stream conversion");
    check(fseek(stream, 0, SEEK_SET) == 0, "rewind direct fscanf");
    char all[64] = {0};
    size_t length = fread(all, 1, sizeof(all), stream);
    check(length == sizeof(payload) - 1U + 6U &&
          !memcmp(all, payload, sizeof(payload) - 1U) &&
          !memcmp(all + sizeof(payload) - 1U, "+tail!", 6),
          "append contents");
    check(fclose(stream) == 0, "read close");

    stream = fopen("/temp/stdio-stream.bin", "r");
    check(stream != NULL, "fopen for position tests");
    char user_buffer[32];
    check(setvbuf(stream, user_buffer, _IOFBF, sizeof(user_buffer)) == 0,
          "setvbuf caller storage");
    fpos_t position;
    check(fgetpos(stream, &position) == 0 && position == 0,
          "fgetpos at start");
    char line[16] = {0};
    check(fgets(line, sizeof(line), stream) != NULL && line[0] == 's',
          "fgets");
    check(fsetpos(stream, &position) == 0, "fsetpos");
    check(fgetc(stream) == 's', "fsetpos content");
    while (fgetc(stream) != EOF) { }
    check(feof(stream), "EOF before rewind");
    rewind(stream);
    check(!feof(stream) && ftell(stream) == 0, "rewind clears EOF and position");
    test_read_limit(3);
    memset(readback, 0, sizeof(readback));
    check(fread(readback, 1, sizeof(payload) - 1U, stream) == sizeof(payload) - 1U &&
          !memcmp(readback, payload, sizeof(payload) - 1U),
          "confirmed partial native reads");
    test_read_limit(0);
    check(fclose(stream) == 0, "position stream close");

    stream = fopen("/temp/stdio-stream.bin", "r+");
    check(stream != NULL, "open update stream for read-position regression");
    check(fgetc(stream) == 's', "update stream first read");
    fpos_t update_position = -1;
    check(fgetpos(stream, &update_position) == 0 && update_position == 1,
          "fgetpos accounts for buffered input");
    check(fseek(stream, 0, SEEK_CUR) == 0 && ftell(stream) == 1,
          "fseek current preserves buffered input position");
    check(fgetc(stream) == 't', "update stream read after fseek");
    check(fclose(stream) == 0, "fclose does not flush input buffer");
    stream = fopen("/temp/stdio-stream.bin", "r");
    char after_update[64] = {0};
    check(stream && fread(after_update, 1, length, stream) == length &&
          !memcmp(after_update, all, length),
          "read-only update operations leave file bytes unchanged");
    if (stream) check(fclose(stream) == 0, "close update verification stream");

    stream = fopen("/temp/stdio-stream.bin", "r");
    check(stream != NULL, "fopen error fixture");
    test_read_error(MG_ERR_DEVICE_GONE);
    *probe_errno_location() = 0;
    check(fgetc(stream) == EOF && ferror(stream) && !feof(stream) &&
          *probe_errno_location() == ENODEV,
          "device loss is not EOF");
    test_read_error(0);
    clearerr(stream);
    check(fseek(stream, 0, SEEK_SET) == 0 && fgetc(stream) == 's',
          "clear error then seek and read");
    stream = freopen("/temp/stdio-stream.bin", "r", stream);
    check(stream != NULL, "freopen returned NULL");
    check(fgetc(stream) == 's', "freopen first byte");
    check(fclose(stream) == 0, "freopen close");

    stream = fopen("/temp/stdio-stream.bin", "a");
    check(stream != NULL && fprintf(stream, "%s:%04d", "v", 7) == 6,
          "fprintf");
    __stdio_exit_flush();
    check(fclose(stream) == 0, "exit flush then close");

    stream = fopen("/temp/stdio-fault.bin", "w");
    check(stream != NULL && fwrite("abcdef", 1, 6, stream) == 6,
          "fault fixture buffered write");
    test_write_mode(2);
    check(fflush(stream) == EOF && ferror(stream),
          "partial write then uncertain result");
    check(fclose(stream) == EOF, "close preserves unresolved write error");
    test_write_mode(1);

    FILE *open_streams[FOPEN_MAX] = {0};
    size_t opened = 0;
    while (opened < FOPEN_MAX &&
           (open_streams[opened] = fopen("/temp/stdio-stream.bin", "r")) != NULL)
        opened++;
    if (!(opened == FOPEN_MAX - 3U && *probe_errno_location() == EMFILE)) {
        char message[128];
        int n = host_snprintf(message, sizeof(message),
            "streams=%zu expected=%d errno=%d EMFILE=%d\n", opened,
            FOPEN_MAX - 3, *probe_errno_location(), EMFILE);
        (void)write(2, message, (unsigned long)n);
        check(false, "bounded stream handle table");
    }
    while (opened) check(fclose(open_streams[--opened]) == 0,
                         "close bounded stream slot");
    check(tmpfile() == NULL && *probe_errno_location() == ENOSYS,
          "tmpfile unsupported explicitly");
    check(tmpnam(NULL) == NULL && *probe_errno_location() == ENOSYS,
          "tmpnam unsupported explicitly");
    check(rename("/temp/stdio-stream.bin", "/temp/stdio-fault.bin") == -1 &&
          *probe_errno_location() == EEXIST,
          "rename preserves existing destination");
    check(remove("/temp/stdio-fault.bin") == 0, "remove");

    check(probe_printf("console:%d\n", 17) == 11,
          "line buffered native stdout");
    check(call_vprintf("v:%d\n", 8) == 4, "vprintf variadic forwarding");
    size_t console_length = 0;
    const char *console = test_console_output(&console_length);
    check(console_length >= 15 && !memcmp(console + console_length - 15,
          "console:17\nv:8\n", 15), "console output bytes");
    *probe_errno_location() = EACCES;
    probe_perror("open");
    console = test_console_output(&console_length);
    static const char perror_expected[] = "open: Permission denied\n";
    check(console_length >= sizeof(perror_expected) - 1U &&
          !memcmp(console + console_length - (sizeof(perror_expected) - 1U),
                  perror_expected, sizeof(perror_expected) - 1U),
          "perror prefix and saved errno text");
    *probe_errno_location() = EIO;
    probe_perror(NULL);
    console = test_console_output(&console_length);
    static const char perror_empty_expected[] = "Input/output error\n";
    check(console_length >= sizeof(perror_empty_expected) - 1U &&
          !memcmp(console + console_length - (sizeof(perror_empty_expected) - 1U),
                  perror_empty_expected, sizeof(perror_empty_expected) - 1U),
          "perror null prefix");
    static const char stdio_input[] = "x\n";
    test_console_input_reset(stdio_input, sizeof(stdio_input) - 1U);
    check(probe_printf("stdio prompt: ") == 14,
          "line-buffered input prompt queued");
    check(fgetc(stdin) == 'x' &&
          test_console_input_boundary_count() == 1U,
          "standard input consumes test byte");
    size_t input_boundary = test_console_input_boundary_output_length(0);
    console = test_console_output(&console_length);
    check(input_boundary >= 14U &&
          !memcmp(console + input_boundary - 14U, "stdio prompt: ", 14U),
          "standard input flushes the line-buffered prompt before reading");

    test_write_mode(-1);
    check(probe_printf("fail\n") == EOF && *probe_errno_location() == EIO,
          "formatted output reports line-flush failure");
    test_write_mode(1);
    clearerr(stdout);
    check(fflush(stdout) == 0, "flush after explicit error recovery");

    login_prompt_tests();
}

int main(void)
{
    void *libc = dlopen("libc.so.6", 2);
    check(libc != NULL, "load independent host libc");
    host_snprintf = (host_snprintf_t)dlsym(libc, "snprintf");
    host_sscanf = (host_sscanf_t)dlsym(libc, "sscanf");
    host_strtod = (host_strtod_t)dlsym(libc, "strtod");
    host_strtof = (host_strtof_t)dlsym(libc, "strtof");
    host_strtold = (host_strtold_t)dlsym(libc, "strtold");
    check(host_snprintf && host_sscanf && host_strtod && host_strtof && host_strtold,
          "host references");
    formatting_tests();
    scanning_tests();
    stream_tests();
    const char *pass = "C formatted I/O and conversion checks PASS\n";
    (void)write(1, pass, strlen(pass));
    return 0;
}
