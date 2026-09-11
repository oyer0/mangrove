/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <mangrove_version.h>
#include <stdio.h>
#include <string.h>
#include "../common/help.h"

#define INFO_LOGO_LINES 14U
#define INFO_COLUMN     42U
#define INFO_LABEL_WIDTH 8U

typedef struct {
    const char *label;
    const char *value;
} info_field_t;

static const char *const info_logo[INFO_LOGO_LINES] = {
    "                      yyg@$,",
    "                   yg@@@@@@$",
    "                 y@@@@@$9@@@L",
    "                g@@@@@@$$@@@$",
    " .              $@@@@@@g@@@@E",
    "  7@gy_,        4@@@@@g@@@@@",
    "   `@@@@@$gy_    @@@Fa@@@@~",
    "     R@@@@@@@g   `@Fa@P~~            y",
    "      ~@@@$@@@y   ``    yyagggggggg$@F",
    "        7R@@@@@_     _a@@@@@@@@@@@@@F",
    "           `~~`?_   yPE@g$@@@@@@@@P`",
    "                \"  g~fR@@@@@@@@F~",
    "                   @    `````",
    "                   @",
};

static usize text_width(const char *text)
{
    usize width = 0;

    if (!text) return 0;
    for (usize index = 0; text[index]; index++)
        if (((u8)text[index] & 0xC0U) != 0x80U) width++;
    return width;
}

static bool write_semantic(const char *text, mg_terminal_style_role_t style)
{
    usize length;

    if (!text) return false;
    length = strlen(text);
    return terminal_write_semantic(text, length, style) == (mg_result_t)length;
}

static bool write_default(const char *text)
{
    return write_semantic(text, MG_TERMINAL_STYLE_DEFAULT);
}

static bool write_spaces(usize count)
{
    static const char spaces[] =
        "                                                                ";

    while (count) {
        usize chunk = count < sizeof(spaces) - 1U
            ? count : sizeof(spaces) - 1U;
        if (chunk == sizeof(spaces) - 1U) {
            if (!write_default(spaces)) return false;
        } else {
            char tail[sizeof(spaces)];
            memcpy(tail, spaces, chunk);
            tail[chunk] = '\0';
            if (!write_default(tail)) return false;
        }
        count -= chunk;
    }
    return true;
}

static bool format_bytes(u64 bytes, char *output, usize capacity)
{
    const char *unit = "B";
    u64 divisor = 1;
    int length;

    if (!output || !capacity) return false;
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        unit = "GiB";
        divisor = 1024ULL * 1024ULL * 1024ULL;
    } else if (bytes >= 1024ULL * 1024ULL) {
        unit = "MiB";
        divisor = 1024ULL * 1024ULL;
    } else if (bytes >= 1024ULL) {
        unit = "KiB";
        divisor = 1024ULL;
    }
    length = snprintf(output, capacity, "%llu %s", bytes / divisor, unit);
    return length >= 0 && (usize)length < capacity;
}

static bool format_uptime(u64 milliseconds, char *output, usize capacity)
{
    u64 seconds = milliseconds / 1000ULL;
    int length;

    if (!output || !capacity) return false;
    length = snprintf(output, capacity, "%02llu:%02llu:%02llu",
                      seconds / 3600ULL, (seconds / 60ULL) % 60ULL,
                      seconds % 60ULL);
    return length >= 0 && (usize)length < capacity;
}

static bool add_field(info_field_t *fields, u32 *count,
                      const char *label, const char *value)
{
    if (!fields || !count || !label || !value || *count >= INFO_LOGO_LINES)
        return false;
    fields[*count].label = label;
    fields[*count].value = value;
    (*count)++;
    return true;
}

static bool write_field(const info_field_t *field)
{
    usize label_width;

    if (!field || !field->label || !field->value) return false;
    label_width = text_width(field->label);
    return write_semantic(field->label, MG_TERMINAL_STYLE_HEADING) &&
           write_spaces(label_width < INFO_LABEL_WIDTH
                        ? INFO_LABEL_WIDTH - label_width : 1U) &&
           write_default(": ") && write_default(field->value);
}

static bool write_screen(const info_field_t *fields, u32 field_count)
{
    if (!fields || field_count > INFO_LOGO_LINES) return false;
    if (terminal_update_begin() != MG_OK) return false;
    for (u32 line = 0; line < INFO_LOGO_LINES; line++) {
        usize logo_width = text_width(info_logo[line]);
        if (!write_semantic(info_logo[line], MG_TERMINAL_STYLE_SUCCESS) ||
            !write_spaces(logo_width < INFO_COLUMN
                          ? INFO_COLUMN - logo_width : 1U)) {
            (void)terminal_update_end();
            return false;
        }
        if (line < field_count && !write_field(&fields[line])) {
            (void)terminal_update_end();
            return false;
        }
        if (!write_default("\n")) {
            (void)terminal_update_end();
            return false;
        }
    }
    return terminal_update_end() == MG_OK;
}

static char cpu_lower(char character)
{
    if (character >= 'A' && character <= 'Z')
        return (char)(character - 'A' + 'a');
    return character;
}

static bool cpu_is_space(char character)
{
    return character == ' ' || character == '\t' ||
           character == '\r' || character == '\n';
}

static bool cpu_token_equals(const char *token, usize length,
                             const char *expected)
{
    usize expected_length;

    if (!token || !expected) return false;
    expected_length = strlen(expected);
    if (length != expected_length) return false;
    for (usize index = 0; index < length; index++)
        if (cpu_lower(token[index]) != cpu_lower(expected[index]))
            return false;
    return true;
}

static bool cpu_suffix_equals(const char *text, usize length,
                              const char *suffix)
{
    usize suffix_length;

    if (!text || !suffix) return false;
    suffix_length = strlen(suffix);
    return length >= suffix_length &&
           cpu_token_equals(text + length - suffix_length,
                            suffix_length, suffix);
}

static char *cpu_find_ci(char *text, const char *needle)
{
    usize needle_length;

    if (!text || !needle) return NULL;
    needle_length = strlen(needle);
    if (!needle_length) return text;
    for (usize offset = 0; text[offset]; offset++) {
        usize index;
        for (index = 0; index < needle_length; index++) {
            if (!text[offset + index] ||
                cpu_lower(text[offset + index]) !=
                cpu_lower(needle[index])) break;
        }
        if (index == needle_length) return text + offset;
    }
    return NULL;
}

static void cpu_trim_trailing(char *text)
{
    usize length;

    if (!text) return;
    length = strlen(text);
    while (length && text[length - 1U] == ' ')
        text[--length] = '\0';
}

static void cpu_remove_noise_words(char *text)
{
    usize read = 0;
    usize write = 0;

    if (!text) return;
    while (text[read]) {
        usize start;
        usize length;

        while (text[read] == ' ') read++;
        if (!text[read]) break;
        start = read;
        while (text[read] && text[read] != ' ') read++;
        length = read - start;
        if (cpu_token_equals(text + start, length, "CPU") ||
            cpu_token_equals(text + start, length, "Processor"))
            continue;
        if (write) text[write++] = ' ';
        for (usize index = 0; index < length; index++)
            text[write++] = text[start + index];
    }
    text[write] = '\0';
}

static bool cpu_is_number(const char *text, usize length)
{
    if (!text || !length) return false;
    for (usize index = 0; index < length; index++)
        if (text[index] < '0' || text[index] > '9') return false;
    return true;
}

static bool cpu_is_number_core(const char *text, usize length)
{
    usize index = 0;

    if (!text || !length) return false;
    while (index < length && text[index] >= '0' && text[index] <= '9')
        index++;
    if (!index || index == length) return false;
    if (text[index] == '-') index++;
    return cpu_suffix_equals(text, length, "core") ||
           cpu_suffix_equals(text, length, "cores");
}

static void cpu_trim_core_descriptor(char *text)
{
    usize length;
    usize last_start;

    if (!text) return;
    cpu_trim_trailing(text);
    length = strlen(text);
    last_start = length;
    while (last_start && text[last_start - 1U] != ' ') last_start--;
    if (cpu_is_number_core(text + last_start, length - last_start)) {
        text[last_start] = '\0';
        cpu_trim_trailing(text);
        return;
    }
    if (!cpu_token_equals(text + last_start, length - last_start, "core") &&
        !cpu_token_equals(text + last_start, length - last_start, "cores"))
        return;

    if (last_start) last_start--;
    while (last_start && text[last_start - 1U] == ' ') last_start--;
    {
        usize previous_start = last_start;
        while (previous_start && text[previous_start - 1U] != ' ')
            previous_start--;
        if (cpu_is_number(text + previous_start,
                          last_start - previous_start)) {
            text[previous_start] = '\0';
            cpu_trim_trailing(text);
        }
    }
}

static bool format_cpu_model(const char *input, char *output, usize capacity)
{
    usize write = 0;

    if (!input || !input[0] || !output || capacity == 0U) return false;
    for (usize read = 0; input[read] && write + 1U < capacity;) {
        if (input[read] == '(') {
            usize marker_length = 0;
            if (input[read + 1U] == 'R' || input[read + 1U] == 'r')
                marker_length = 1U;
            else if ((input[read + 1U] == 'T' ||
                      input[read + 1U] == 't') &&
                     (input[read + 2U] == 'M' ||
                      input[read + 2U] == 'm'))
                marker_length = 2U;
            if (marker_length && input[read + marker_length + 1U] == ')') {
                read += marker_length + 2U;
                continue;
            }
        }
        if (cpu_is_space(input[read])) {
            if (write && output[write - 1U] != ' ')
                output[write++] = ' ';
        } else {
            output[write++] = input[read];
        }
        read++;
    }
    if (write && output[write - 1U] == ' ') write--;
    output[write] = '\0';
    cpu_remove_noise_words(output);

    {
        char *with = cpu_find_ci(output, " with ");
        if (with) {
            char *graphics = cpu_find_ci(with + 6U, "graphics");
            char *radeon = cpu_find_ci(with + 6U, "radeon");
            char *vega = cpu_find_ci(with + 6U, "vega");
            if (graphics || radeon || vega) *with = '\0';
        }
    }

    {
        char *clock = NULL;
        for (char *cursor = output; *cursor; cursor++)
            if (*cursor == '@') clock = cursor;
        if (clock && (clock == output || clock[-1] == ' ')) {
            if (clock > output && clock[-1] == ' ')
                clock[-1] = '\0';
            else
                *clock = '\0';
        }
    }
    cpu_trim_trailing(output);
    cpu_trim_core_descriptor(output);
    return output[0] != '\0';
}

static bool format_cpu(const mg_system_info_t *system, char *output,
                       usize capacity)
{

    return system && format_cpu_model(system->cpu_model, output, capacity);
}

static bool format_gpu(const mg_system_info_t *system, char *output,
                       usize capacity)
{
    int length;

    if (!system || !system->gpu_count || !system->gpus[0].name[0] ||
        !output || capacity == 0U)
        return false;
    if (system->gpu_total > 1U)
        length = snprintf(output, capacity, "%s (+%u more)",
                          system->gpus[0].name, system->gpu_total - 1U);
    else
        length = snprintf(output, capacity, "%s", system->gpus[0].name);
    return length >= 0 && (usize)length < capacity;
}

int main(int argc, char **argv)
{
    mg_identity_t identity;
    mg_system_memory_info_t memory;
    mg_system_info_t system;
    info_field_t fields[INFO_LOGO_LINES];
    char kernel[48];
    char memory_used[32];
    char memory_total[32];
    char memory_value[72];
    char uptime[32];
    char cpu[128];
    char gpu[MG_INSPECTION_GPU_NAME_MAX + 16U];
    char display[32];
    u32 field_count = 0;
    bool system_available;
    int length;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc != 1) {
        command_usage_error(argv[0], "info", argc > 1 ? argv[1] : NULL);
        return 1;
    }

    memset(&identity, 0, sizeof(identity));
    memset(&system, 0, sizeof(system));
    system_available = system_info(&system) == MG_OK;
    if (!add_field(fields, &field_count, MANGROVE_NAME, MANGROVE_VERSION) ||
        snprintf(kernel, sizeof(kernel), "%s %s", PITH_NAME,
                 MANGROVE_VERSION) < 0 ||
        !add_field(fields, &field_count, "Kernel", kernel))
        return 1;
    if (process_get_identity(&identity) == MG_OK && identity.username[0] &&
        !add_field(fields, &field_count, "User", identity.username))
        return 1;

    if (format_uptime(uptime_ms(), uptime, sizeof(uptime)) &&
        !add_field(fields, &field_count, "Uptime", uptime)) return 1;

    if (system_available && format_cpu(&system, cpu, sizeof(cpu)) &&
        !add_field(fields, &field_count, "CPU", cpu)) return 1;
    if (system_available && format_gpu(&system, gpu, sizeof(gpu)) &&
        !add_field(fields, &field_count, "GPU", gpu)) return 1;

    if (memory_info(&memory) == MG_OK &&
        format_bytes(memory.physical_used_bytes, memory_used,
                     sizeof(memory_used)) &&
        format_bytes(memory.physical_total_bytes, memory_total,
                     sizeof(memory_total)) &&
        snprintf(memory_value, sizeof(memory_value), "%s / %s",
                 memory_used, memory_total) >= 0 &&
        !add_field(fields, &field_count, "Memory", memory_value)) return 1;

    if (system_available && system.display_width && system.display_height &&
        (length = snprintf(display, sizeof(display), "%ux%u",
                           system.display_width, system.display_height)) >= 0 &&
        (usize)length < sizeof(display) &&
        !add_field(fields, &field_count, "Display", display)) return 1;
    if (!add_field(fields, &field_count, "Shell", "Shoot")) return 1;

    if (!write_screen(fields, field_count)) {
        printf("info: unable to write terminal output.\n");
        return 1;
    }
    return 0;
}
