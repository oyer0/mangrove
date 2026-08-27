#include <config_parser.h>
#include <string.h>

static bool is_space(char value)
{
    return value == ' ' || value == '\t' || value == '\r';
}

static bool is_key_char(char value)
{
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') ||
           value == '_' || value == '-' || value == '.';
}

static bool starts_comment(const char *text, usize position, usize length)
{
    return position + 1U < length && text[position] == '/' &&
           text[position + 1U] == '/';
}

static bool append_char(char *output, usize capacity, usize *length, char value)
{
    if (*length + 1U >= capacity) return false;
    output[(*length)++] = value;
    output[*length] = '\0';
    return true;
}

bool kernel_config_parse(const char *text, usize length,
                         kernel_config_document_t *document,
                         u32 *error_line)
{
    usize position = 0;
    u32 line = 1;

    if (error_line) *error_line = 0;
    if (!text || !document) return false;
    *document = (kernel_config_document_t){0};

    while (position < length) {
        usize key_length = 0;
        usize value_length = 0;
        char key[KERNEL_CONFIG_KEY_MAX] = {0};
        char value[KERNEL_CONFIG_VALUE_MAX] = {0};
        bool terminated = false;

        while (position < length && is_space(text[position])) position++;
        if (position >= length) break;
        if (text[position] == '\n') {
            position++;
            line++;
            continue;
        }
        if (starts_comment(text, position, length)) {
            while (position < length && text[position] != '\n') position++;
            continue;
        }

        while (position < length && is_key_char(text[position])) {
            if (key_length + 1U >= sizeof(key)) goto malformed;
            key[key_length++] = text[position++];
        }
        key[key_length] = '\0';
        while (position < length && is_space(text[position])) position++;
        if (key_length == 0 || position >= length || text[position++] != '=')
            goto malformed;
        while (position < length && is_space(text[position])) position++;

        if (position < length && text[position] == '"') {
            position++;
            while (position < length) {
                char current = text[position++];
                if (current == '"') {
                    terminated = true;
                    break;
                }
                if (current == '\\') {
                    if (position >= length) goto malformed;
                    current = text[position++];
                    if (current != '"' && current != '\\') goto malformed;
                }
                if (current == '\n') goto malformed;
                if (!append_char(value, sizeof(value), &value_length, current))
                    goto malformed;
            }
            if (!terminated) goto malformed;
            while (position < length && is_space(text[position])) position++;
            if (position < length && !starts_comment(text, position, length) &&
                text[position] != '\n') goto malformed;
        } else {
            usize start = position;
            while (position < length && text[position] != '\n' &&
                   !starts_comment(text, position, length)) position++;
            while (position > start && is_space(text[position - 1U])) position--;
            if (position - start >= sizeof(value)) goto malformed;
            for (usize index = start; index < position; index++)
                value[value_length++] = text[index];
            value[value_length] = '\0';
        }

        if (document->count >= KERNEL_CONFIG_MAX_ENTRIES) goto malformed;
        strcpy(document->entries[document->count].key, key);
        strcpy(document->entries[document->count].value, value);
        document->count++;

        while (position < length && text[position] != '\n') position++;
        if (position < length) {
            position++;
            line++;
        }
        continue;

malformed:
        if (error_line) *error_line = line;
        return false;
    }
    return true;
}

const char *kernel_config_find(const kernel_config_document_t *document,
                               const char *key)
{
    if (!document || !key) return NULL;
    for (u32 index = document->count; index > 0; index--) {
        if (strcmp(document->entries[index - 1U].key, key) == 0)
            return document->entries[index - 1U].value;
    }
    return NULL;
}
