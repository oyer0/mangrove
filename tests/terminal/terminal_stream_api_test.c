/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <mg/terminal.h>
#include <mg/line_editor.h>
#include <mangrove.h>
#include <stdio.h>
#include <string.h>

static char control_bytes[1024];
static usize control_length;
static char literal_bytes[1024];
static usize literal_length;
static bool interactive_output = true;
static bool fail_literal_write;
static u32 syscall_operations[32];
static usize syscall_count;
static bool editor_probe;
static bool raw_input;
static unsigned key_reads;
static unsigned editor_update_depth;

static void reset_capture(void)
{
    control_length = 0U;
    literal_length = 0U;
    syscall_count = 0U;
    fail_literal_write = false;
}

static void append_bytes(char *destination, usize *destination_length,
                         usize capacity, const void *bytes, usize length)
{
    assert(length <= capacity - *destination_length);
    memcpy(destination + *destination_length, bytes, length);
    *destination_length += length;
}

mg_result_t console_write(const void *buffer, usize length)
{
    append_bytes(control_bytes, &control_length, sizeof(control_bytes),
                 buffer, length);
    return (mg_result_t)length;
}

mg_result_t console_write_literal(const void *buffer, usize length)
{
    if (fail_literal_write) return MG_ERR_IO;
    append_bytes(literal_bytes, &literal_length, sizeof(literal_bytes),
                 buffer, length);
    return (mg_result_t)length;
}

mg_result_t object_write_all(mg_handle_t handle, const void *buffer, usize length)
{
    assert(editor_probe && handle == MG_STDOUT_HANDLE);
    return console_write_literal(buffer, length) < 0 ? MG_ERR_IO : MG_OK;
}

long mg_syscall(unsigned long number, unsigned long argument0,
                unsigned long argument1, unsigned long argument2)
{
    assert(number == 65UL);
    assert(syscall_count < sizeof(syscall_operations) /
                           sizeof(syscall_operations[0]));
    u32 operation = (u32)argument0;
    syscall_operations[syscall_count++] = operation;
    if (editor_probe) {
        switch (operation) {
            case MG_TERMINAL_OP_INPUT_MODE: {
                const mg_terminal_input_mode_request_t *request =
                    (const void *)(uintptr_t)argument1;
                raw_input = request->enabled != 0U;
                return MG_OK;
            }
            case MG_TERMINAL_OP_PREPARE_INPUT_LINE:
            case MG_TERMINAL_OP_INPUT_CURSOR:
                return MG_OK;
            case MG_TERMINAL_OP_UPDATE_BEGIN:
                editor_update_depth++;
                return MG_OK;
            case MG_TERMINAL_OP_UPDATE_END:
                assert(editor_update_depth > 0U);
                editor_update_depth--;
                return MG_OK;
            case MG_TERMINAL_OP_READ_KEY_EVENT: {
                mg_terminal_key_event_t *event = (void *)(uintptr_t)argument2;
                assert(raw_input);
                *event = (mg_terminal_key_event_t){
                    .version = MG_TERMINAL_API_VERSION,
                    .keycode = MG_TERMINAL_KEY_ENTER,
                };
                key_reads++;
                return MG_OK;
            }
        }
    }
    if (operation == MG_TERMINAL_OP_CAPABILITIES) {
        mg_terminal_capabilities_t *capabilities =
            (mg_terminal_capabilities_t *)(uintptr_t)argument2;
        assert(!argument1 && capabilities);
        capabilities->version = MG_TERMINAL_API_VERSION;
        capabilities->capabilities = interactive_output ?
            MG_TERMINAL_CAP_INTERACTIVE_OUTPUT |
            MG_TERMINAL_CAP_STYLED_OUTPUT : 0U;
        return MG_OK;
    }
    if (operation == MG_TERMINAL_OP_SIZE) {
        mg_terminal_size_t *size =
            (mg_terminal_size_t *)(uintptr_t)argument2;
        assert(!argument1 && size);
        size->version = MG_TERMINAL_API_VERSION;
        size->rows = 25U;
        size->columns = 80U;
        return MG_OK;
    }
    assert(0 && "output helper bypassed ESC stream through a terminal syscall");
    return MG_ERR_UNSUPPORTED;
}

static void expect_control(const char *expected)
{
    usize length = strlen(expected);
    assert(control_length == length);
    assert(!memcmp(control_bytes, expected, length));
}

static void expect_operation_set(void)
{
    for (usize index = 0U; index < syscall_count; index++)
        assert(syscall_operations[index] == MG_TERMINAL_OP_SIZE ||
               syscall_operations[index] == MG_TERMINAL_OP_CAPABILITIES);
}

static void test_control_helpers(void)
{
    reset_capture();
    assert(terminal_alternate_enter() == MG_OK);
    expect_control("\x1b{alt}");
    assert(terminal_alternate_leave() == MG_OK);
    expect_control("\x1b{alt}\x1b{main}");

    assert(terminal_cursor_move(4U, 9U) == MG_OK);
    expect_control("\x1b{alt}\x1b{main}\x1b{at:5,10}");
    usize previous_length = control_length;
    assert(terminal_cursor_move(25U, 0U) == MG_ERR_BAD_ARGUMENT);
    assert(terminal_cursor_move(0U, 80U) == MG_ERR_BAD_ARGUMENT);
    assert(control_length == previous_length);

    assert(terminal_cursor_set_visible(false) == MG_OK);
    assert(terminal_cursor_set_visible(true) == MG_OK);
    assert(terminal_clear_line() == MG_OK);
    assert(terminal_clear_to_end() == MG_OK);
    assert(terminal_clear_screen() == MG_OK);
    expect_control("\x1b{alt}\x1b{main}\x1b{at:5,10}"
                   "\x1b{hide}\x1b{show}\x1b{cll}\x1b{cleol}"
                   "\x1b{cls}\x1b{at:1,1}");
    expect_operation_set();
}

static void test_styled_helper_palette(void)
{
    static const char *const colors[MG_TERMINAL_COLOR_COUNT] = {
        "black", "red", "green", "yellow", "blue", "magenta", "cyan",
        "l_gray", "d_gray", "b_red", "b_green", "b_yellow", "b_blue",
        "b_magenta", "b_cyan", "white"
    };
    static const char literal[] = "safe \x1b{cls} payload";
    char expected[128];

    for (u32 color = 0U; color < MG_TERMINAL_COLOR_COUNT; color++) {
        reset_capture();
        assert(terminal_write_styled(literal, sizeof(literal) - 1U,
            (mg_terminal_color_t)color,
            (mg_terminal_color_t)((color + 1U) % MG_TERMINAL_COLOR_COUNT)) ==
            (mg_result_t)(sizeof(literal) - 1U));
        int length = snprintf(expected, sizeof(expected),
            "\x1b{fg:%s}\x1b{bg:%s}\x1b{reset}",
            colors[color], colors[(color + 1U) % MG_TERMINAL_COLOR_COUNT]);
        assert(length > 0 && (usize)length < sizeof(expected));
        expect_control(expected);
        assert(literal_length == sizeof(literal) - 1U);
        assert(!memcmp(literal_bytes, literal, sizeof(literal) - 1U));
        expect_operation_set();
    }

    reset_capture();
    fail_literal_write = true;
    assert(terminal_write_styled("text", 4U, MG_TERMINAL_COLOR_GREEN,
        MG_TERMINAL_COLOR_BLACK) == MG_ERR_IO);
    expect_control("\x1b{fg:green}\x1b{bg:black}\x1b{reset}");
}

static void test_semantic_helper_styles(void)
{
    static const char *const commands[MG_TERMINAL_STYLE_COUNT] = {
        "\x1b{reset}", "\x1b{fg:b_blue}\x1b{bg:black}",
        "\x1b{err}", "\x1b{warn}", "\x1b{ok}", "\x1b{head}"
    };
    static const char reset[] = "\x1b{reset}";
    char expected[96];

    for (u32 role = 0U; role < MG_TERMINAL_STYLE_COUNT; role++) {
        reset_capture();
        assert(terminal_write_semantic("heading", 7U,
            (mg_terminal_style_role_t)role) == 7);
        int length = snprintf(expected, sizeof(expected), "%s%s",
                              commands[role], reset);
        assert(length > 0 && (usize)length < sizeof(expected));
        expect_control(expected);
        assert(literal_length == 7U && !memcmp(literal_bytes, "heading", 7U));
    }
}

static void test_redirected_output_has_no_controls(void)
{
    static const char text[] = "literal \x1b{cls}";

    reset_capture();
    interactive_output = false;
    assert(terminal_clear_screen() == MG_ERR_UNSUPPORTED);
    assert(control_length == 0U && literal_length == 0U);
    assert(terminal_write_styled(text, sizeof(text) - 1U,
        MG_TERMINAL_COLOR_RED, MG_TERMINAL_COLOR_BLACK) ==
        (mg_result_t)(sizeof(text) - 1U));
    assert(control_length == 0U);
    assert(literal_length == sizeof(text) - 1U);
    assert(!memcmp(literal_bytes, text, sizeof(text) - 1U));

    reset_capture();
    assert(terminal_write_semantic(text, sizeof(text) - 1U,
        MG_TERMINAL_STYLE_WARNING) == (mg_result_t)(sizeof(text) - 1U));
    assert(control_length == 0U);
    assert(literal_length == sizeof(text) - 1U);
    assert(!memcmp(literal_bytes, text, sizeof(text) - 1U));
    expect_operation_set();
    interactive_output = true;
}

static void test_line_editor_output_destination(void)
{
    char buffer[32];
    mg_line_editor_t editor;
    editor_probe = true;
    for (unsigned interactive = 0; interactive < 2U; interactive++) {
        reset_capture();
        key_reads = 0U;
        interactive_output = interactive != 0U;
        line_editor_init(&editor, buffer, sizeof(buffer), "developer ~: ");
        line_editor_set_prompt_style(&editor, MG_TERMINAL_COLOR_GREEN,
                                     MG_TERMINAL_COLOR_BLACK);
        line_editor_set_key_event_input(&editor, true);
        assert(line_editor_read_line(&editor) ==
               (interactive ? 0 : MG_ERR_UNSUPPORTED));
        assert(!raw_input && !editor_update_depth && key_reads == interactive);
        if (!interactive) {
            /* A redirected prompt cannot become an invisible input consumer.
             * The caller must explicitly give an interactive child a console. */
            assert(control_length == 0U);
            assert(literal_length == strlen("developer ~: "));
        } else {
            static const char show[] = "\x1b{show}";
            assert(control_length >= sizeof(show) - 1U);
            assert(!memcmp(control_bytes + control_length - (sizeof(show) - 1U),
                           show, sizeof(show) - 1U));
        }
    }
    editor_probe = false;
}

int main(void)
{
    test_control_helpers();
    test_styled_helper_palette();
    test_semantic_helper_styles();
    test_redirected_output_has_no_controls();
    test_line_editor_output_destination();
    return 0;
}
