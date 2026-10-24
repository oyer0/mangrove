/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production Reed load/save functions with controlled native file calls. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../userspace/reed/editor.c"

typedef struct {
    char data[MG_FILE_REPLACE_MAX_BYTES + 1U];
    usize length;
    u64 generation;
} test_file_t;

static test_file_t first, second, created;
static test_file_t *path_target;
static bool created_visible;
static char created_path[REED_MAX_PATH_BYTES];
static test_file_t *handles[32];
static u64 expectations[32];
static unsigned next_handle = 3, opens, closes, snapshots, replacements;
static unsigned creations, created_handles;
static mg_result_t snapshot_failure, replacement_failure, creation_failure;
static bool report_short_size;
static int fail_allocation_after = -1;
static u64 reported_size_override = (u64)-1;
static const char *prompt_text;
static const char *prompt_next_text;
static usize prompt_offset;
static reed_editor_t *active_editor;
static unsigned conflict_prompts, snapshots_at_choice;
static unsigned required_snapshots_at_choice;
static bool mutate_on_overwrite_choice;
static char conflict_prompt_status[REED_STATUS_CAPACITY];

bool result_is_error(mg_result_t result)
{
    return result < 0;
}

bool command_expand_home_path(const char *input, bool allow_home,
                              char *output, usize capacity)
{
    (void)allow_home;
    if (!input || strlen(input) >= capacity) return false;
    strcpy(output, input);
    return true;
}

bool command_resolve_path(const char *input, char *output, usize capacity)
{
    if (!input || !*input || strlen(input) >= capacity) return false;
    strcpy(output, input);
    return true;
}

mg_result_t terminal_get_size(mg_terminal_size_t *size)
{
    size->rows = 3;
    size->columns = 80;
    return MG_OK;
}
mg_result_t terminal_update_begin(void) { return MG_OK; }
mg_result_t terminal_update_end(void) { return MG_OK; }
mg_result_t terminal_clear_screen(void) { return MG_OK; }
mg_result_t terminal_cursor_move(u32 row, u32 column)
{
    (void)row; (void)column; return MG_OK;
}
mg_result_t terminal_write_styled(const void *data, usize length,
                                   mg_terminal_color_t foreground,
                                   mg_terminal_color_t background)
{
    (void)data; (void)length; (void)foreground; (void)background;
    return (mg_result_t)length;
}
mg_result_t terminal_read_key_event(u32 timeout, mg_terminal_key_event_t *event)
{
    (void)timeout;
    if (!prompt_text || !event) return MG_ERR_IO;
    if (active_editor && active_editor->prompt == REED_PROMPT_CONFLICT) {
        conflict_prompts++;
        snapshots_at_choice = snapshots;
        assert(snapshots_at_choice >= required_snapshots_at_choice);
        strcpy(conflict_prompt_status, active_editor->status);
    }
    memset(event, 0, sizeof(*event));
    if (prompt_text[prompt_offset]) {
        event->codepoint = (u32)(u8)prompt_text[prompt_offset++];
        if (event->codepoint == 27U) event->keycode = HID_KEY_ESCAPE;
        if ((event->codepoint == 's' ||
             (event->codepoint == 'a' && active_editor &&
              active_editor->prompt == REED_PROMPT_CONFLICT)) &&
            prompt_next_text) {
            prompt_text = prompt_next_text;
            prompt_next_text = NULL;
            prompt_offset = 0;
        }
        if (event->codepoint == 'o' && mutate_on_overwrite_choice) {
            assert(active_editor && active_editor->prompt == REED_PROMPT_CONFLICT);
            handles[active_editor->file_handle]->generation++;
            handles[active_editor->file_handle]->data[0] = 'Z';
            mutate_on_overwrite_choice = false;
        }
    }
    else {
        event->keycode = HID_KEY_ENTER;
        prompt_text = NULL;
    }
    return MG_OK;
}

void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size)
{
    if (fail_allocation_after == 0) {
        fail_allocation_after = -1;
        return NULL;
    }
    if (fail_allocation_after > 0) fail_allocation_after--;
    return __real_malloc(size);
}

mg_result_t path_info(const char *path, mg_path_info_t *info)
{
    if (!strcmp(path, "/new") && !created_visible) return MG_ERR_NOT_FOUND;
    if (strcmp(path, "/file") && strcmp(path, "/alias") &&
        strcmp(path, "/other")) return MG_ERR_NOT_FOUND;
    if (!info) return MG_ERR_BAD_ARGUMENT;
    memset(info, 0, sizeof(*info));
    info->type = MG_PATH_TYPE_FILE;
    info->size = reported_size_override != (u64)-1
        ? reported_size_override : (report_short_size ? 1U :
        (strcmp(path, "/other") ? path_target->length : second.length));
    return MG_OK;
}

mg_result_t file_open(const char *path, u32 flags)
{
    test_file_t *target = strcmp(path, "/other") ? path_target : &second;
    assert(flags == MG_OPEN_RDWR);
    assert(next_handle < 32);
    handles[next_handle] = target;
    expectations[next_handle] = target->generation;
    opens++;
    return next_handle++;
}

mg_result_t file_create_contents(const char *path, const void *data,
                                 usize length, mg_handle_t *out_handle)
{
    creations++;
    assert(path && out_handle && length <= MG_FILE_REPLACE_MAX_BYTES);
    assert(length == 0 || data);
    if (creation_failure) return creation_failure;
    if (!strcmp(path, "/file") || !strcmp(path, "/other") ||
        (created_visible && !strcmp(path, created_path)))
        return MG_ERR_ALREADY_EXISTS;
    assert(next_handle < 32);
    memcpy(created.data, data, length);
    created.length = length;
    created.generation = 1;
    strncpy(created_path, path, sizeof(created_path) - 1U);
    created_path[sizeof(created_path) - 1U] = '\0';
    created_visible = true;
    handles[next_handle] = &created;
    expectations[next_handle] = created.generation;
    *out_handle = next_handle++;
    created_handles++;
    return MG_OK;
}

mg_result_t file_snapshot(mg_handle_t handle, char *data, usize capacity,
                          mg_file_lines_state_t *state)
{
    test_file_t *target = handles[handle];
    snapshots++;
    assert(target && data && state);
    if (snapshot_failure) return snapshot_failure;
    if (capacity < target->length) return MG_ERR_BUFFER_TOO_SMALL;
    memcpy(data, target->data, target->length);
    state->identifier = (u64)(uintptr_t)target;
    state->revision = target->generation;
    state->length = target->length;
    expectations[handle] = target->generation;
    return MG_OK;
}

mg_result_t file_replace(mg_handle_t handle, const void *data, usize length)
{
    test_file_t *target = handles[handle];
    replacements++;
    assert(target && length <= MG_FILE_REPLACE_MAX_BYTES);
    if (replacement_failure) return replacement_failure;
    if (expectations[handle] != target->generation)
        return MG_ERR_STALE_GENERATION;
    memcpy(target->data, data, length);
    target->length = length;
    target->generation++;
    expectations[handle] = target->generation;
    return MG_OK;
}

mg_result_t handle_close(mg_handle_t handle)
{
    assert(handle < 32 && handles[handle]);
    handles[handle] = NULL;
    closes++;
    return MG_OK;
}

static void init_editor(reed_editor_t *editor)
{
    memset(editor, 0, sizeof(*editor));
    assert(reed_buffer_init(&editor->buffer));
    assert(reed_buffer_mark_saved(&editor->buffer));
    editor->frame = calloc(3U * 80U, sizeof(*editor->frame));
    editor->presented_frame = calloc(3U * 80U,
                                     sizeof(*editor->presented_frame));
    assert(editor->frame && editor->presented_frame);
    editor->running = true;
    active_editor = editor;
}

static void destroy_editor(reed_editor_t *editor)
{
    if (editor->existing_file) assert(handle_close(editor->file_handle) == MG_OK);
    editor_destroy_frames(editor);
    reed_buffer_destroy(&editor->buffer);
    active_editor = NULL;
}

int main(void)
{
    reed_editor_t editor;
    mg_handle_t original;
    unsigned before, closes_before;
    char *saved;
    char edited_before[32], edited_after[32];
    usize edited_length, after_length;

    memcpy(first.data, "old\n", 4);
    first.length = 4;
    first.generation = 7;
    memcpy(second.data, "else\n", 5);
    second.length = 5;
    second.generation = 3;
    path_target = &first;
    init_editor(&editor);
    report_short_size = true;
    assert(editor_open_path(&editor, "/file") == MG_OK);
    assert(snapshots >= 2 && editor.existing_file);
    assert(!reed_buffer_is_modified(&editor.buffer));
    assert(reed_buffer_length(&editor.buffer) == 3);
    original = editor.file_handle;
    report_short_size = false;

    assert(reed_buffer_record_edit(&editor.buffer));
    assert(reed_buffer_insert(&editor.buffer, 3, " one", 4));
    path_target = &second; /* Original pathname is reused after rename. */
    before = snapshots;
    assert(save_action(&editor));
    assert(snapshots == before); /* Ordinary save needs no extra snapshot. */
    assert(first.length == 8 && !memcmp(first.data, "old one\n", 8));
    assert(second.length == 5 && !reed_buffer_is_modified(&editor.buffer));
    assert(editor.file_handle == original);
    assert(reed_buffer_insert(&editor.buffer, 7, "!", 1));
    assert(save_action(&editor));
    assert(first.generation == 9 && first.length == 9);

    saved = editor.buffer.saved_text;
    assert(reed_buffer_insert(&editor.buffer, 8, "x", 1));
    assert(reed_buffer_copy(&editor.buffer, edited_before,
                            sizeof(edited_before), &edited_length));
    first.generation++;
    before = replacements;
    prompt_text = "c";
    prompt_offset = 0;
    unsigned snapshots_before_conflict = snapshots;
    required_snapshots_at_choice = snapshots + 1;
    assert(!save_action(&editor) && replacements == before + 1);
    assert(snapshots == snapshots_before_conflict + 1 &&
           snapshots_at_choice == snapshots && conflict_prompts &&
           editor.save_conflict);
    assert(!strcmp(editor.status,
                   "Save cancelled; external change unresolved; edits kept."));
    required_snapshots_at_choice = 0;
    assert(editor.buffer.saved_text == saved && reed_buffer_is_modified(&editor.buffer));
    assert(editor.buffer.undo.count == 1 && editor.file_handle == original);
    assert(reed_buffer_copy(&editor.buffer, edited_after,
                            sizeof(edited_after), &after_length) &&
           after_length == edited_length &&
           !memcmp(edited_before, edited_after, edited_length));
    before = replacements;
    prompt_text = "o";
    prompt_offset = 0;
    mutate_on_overwrite_choice = true;
    assert(!save_action(&editor) && replacements == before + 1);
    assert(editor.save_conflict && !mutate_on_overwrite_choice &&
           !strcmp(editor.status,
                   "Save refused: file changed externally; edits kept."));
    assert(first.data[0] == 'Z');
    assert(editor.buffer.saved_text == saved && editor.buffer.undo.count == 1);
    assert(reed_buffer_copy(&editor.buffer, edited_after,
                            sizeof(edited_after), &after_length) &&
           after_length == edited_length &&
           !memcmp(edited_before, edited_after, edited_length));
    snapshot_failure = MG_ERR_ACCESS_DENIED;
    before = replacements;
    prompt_text = "o";
    prompt_offset = 0;
    assert(!save_action(&editor) && replacements == before);
    assert(!strcmp(conflict_prompt_status,
                   "Changed; read denied: [c] Cancel [a] Save As: ") &&
           editor.save_conflict);
    snapshot_failure = MG_ERR_BUFFER_TOO_SMALL;
    prompt_text = "o";
    prompt_offset = 0;
    assert(!save_action(&editor) && replacements == before);
    assert(!strcmp(conflict_prompt_status,
                   "Changed; file exceeds 2 MiB: [c] Cancel [a] Save As: "));
    snapshot_failure = 0;
    fail_allocation_after = 0;
    prompt_text = "o";
    prompt_offset = 0;
    assert(!save_action(&editor) && replacements == before &&
           editor.save_conflict);
    assert(!strcmp(conflict_prompt_status,
                   "Changed; no snapshot memory: [c] Cancel [a] Save As: "));
    prompt_text = "a";
    prompt_next_text = "/file";
    prompt_offset = 0;
    before = closes;
    assert(!save_action(&editor) && editor.save_conflict && closes == before);
    assert(!strcmp(editor.status, "Save refused: destination exists; edits kept."));
    prompt_text = "o";
    prompt_offset = 0;
    assert(save_action(&editor) && !editor.save_conflict);
    assert(!reed_buffer_is_modified(&editor.buffer) && editor.file_handle == original);
    saved = editor.buffer.saved_text;
    assert(reed_buffer_insert(&editor.buffer,
                              reed_buffer_length(&editor.buffer), "?", 1));
    snapshot_failure = MG_ERR_IO;
    before = closes;
    assert(editor_open_path(&editor, "/other") == MG_ERR_IO);
    assert(editor.file_handle == original && closes == before + 1);
    assert(editor.buffer.saved_text == saved && reed_buffer_is_modified(&editor.buffer));
    assert(editor.buffer.undo.count == 1);
    snapshot_failure = MG_ERR_DEVICE_GONE;
    assert(editor_open_path(&editor, "/other") == MG_ERR_DEVICE_GONE);
    assert(editor.file_handle == original && editor.buffer.undo.count == 1);
    snapshot_failure = 0;
    snapshot_failure = MG_ERR_UNSUPPORTED;
    assert(editor_open_path(&editor, "/other") == MG_ERR_UNSUPPORTED);
    assert(editor.file_handle == original && reed_buffer_is_modified(&editor.buffer));

    replacement_failure = MG_ERR_ACCESS_DENIED;
    before = replacements;
    assert(!save_action(&editor) && replacements == before + 1);
    assert(!strcmp(editor.status, "Save denied: access changed; edits kept."));
    assert(editor.buffer.saved_text == saved && editor.buffer.undo.count == 1);
    replacement_failure = MG_ERR_DEVICE_GONE;
    assert(!save_action(&editor));
    assert(!strcmp(editor.status, "Save failed: device removed; edits kept."));
    replacement_failure = 0;
    snapshot_failure = 0;
    second.data[0] = '\0';
    assert(editor_open_path(&editor, "/other") == MG_ERR_BAD_ARGUMENT);
    second.data[0] = (char)0xFF;
    assert(editor_open_path(&editor, "/other") == MG_ERR_BAD_ARGUMENT);
    second.data[0] = 'e';
    fail_allocation_after = 0;
    assert(editor_open_path(&editor, "/other") == MG_ERR_NO_MEMORY);
    assert(editor.file_handle == original && reed_buffer_is_modified(&editor.buffer));
    fail_allocation_after = 5;
    assert(editor_open_path(&editor, "/other") == MG_ERR_NO_MEMORY);
    assert(editor.file_handle == original && editor.buffer.undo.count == 1);
    reported_size_override = REED_MAX_BUFFER_BYTES + 1U;
    before = snapshots;
    assert(editor_open_path(&editor, "/other") == MG_ERR_BUFFER_TOO_SMALL);
    assert(snapshots == before && editor.file_handle == original);
    reported_size_override = (u64)-1;

    before = replacements;
    fail_allocation_after = 1; /* Serialized file allocates; baseline does not. */
    assert(!save_action(&editor) && replacements == before);
    assert(editor.buffer.saved_text == saved && reed_buffer_is_modified(&editor.buffer));

    /* The exact replacement limit includes the optional final newline. */
    char *large = (char *)malloc(MG_FILE_REPLACE_MAX_BYTES + 1U);
    assert(large);
    memset(large, 'x', MG_FILE_REPLACE_MAX_BYTES + 1U);
    assert(reed_buffer_replace(&editor.buffer, large,
                               MG_FILE_REPLACE_MAX_BYTES));
    assert(reed_buffer_set_final_newline(&editor.buffer, false));
    first.generation = expectations[original];
    execute_save_command(&editor, true);
    assert(!editor.running && !editor.save_uncertain);
    editor.running = true;
    assert(first.length == MG_FILE_REPLACE_MAX_BYTES);
    assert(!reed_buffer_is_modified(&editor.buffer));
    saved = editor.buffer.saved_text;
    assert(reed_buffer_replace(&editor.buffer, large,
                               MG_FILE_REPLACE_MAX_BYTES));
    assert(reed_buffer_set_final_newline(&editor.buffer, true));
    before = replacements;
    assert(!save_action(&editor) && replacements == before);
    assert(reed_buffer_is_modified(&editor.buffer));
    assert(reed_buffer_replace(&editor.buffer, "again", 5));
    replacement_failure = MG_ERR_UNCERTAIN;
    before = snapshots;
    execute_save_command(&editor, true); /* /sq must stay open on failure. */
    assert(editor.running && editor.save_uncertain);
    assert(!editor.save_conflict);
    assert(snapshots == before && editor.prompt == REED_PROMPT_NONE);
    assert(editor.buffer.saved_text == saved &&
           reed_buffer_is_modified(&editor.buffer));
    assert(!strcmp(editor.status,
                   "Save outcome uncertain; edits kept. Do not retry."));
    replacement_failure = 0;
    before = replacements;
    assert(!save_action(&editor) && replacements == before);
    assert(editor.save_uncertain);
    assert(!strcmp(editor.status,
                   "Save blocked after uncertain outcome; use /saveas PATH or discard."));
    free(large);

    second.length = 0;
    assert(editor_open_path(&editor, "/other") == MG_OK);
    assert(reed_buffer_length(&editor.buffer) == 0 &&
           !reed_buffer_has_final_newline(&editor.buffer));
    assert(save_action(&editor) && second.length == 0);

    /* Save As binds the returned inode handle, never a reopened pathname. */
    assert(reed_buffer_record_edit(&editor.buffer));
    assert(reed_buffer_insert(&editor.buffer, 0, "copy", 4));
    original = editor.file_handle;
    saved = editor.buffer.saved_text;
    before = closes;
    assert(!save_as_action(&editor, "/file"));
    assert(!strcmp(editor.status,
                   "Save refused: destination exists; edits kept."));
    assert(editor.file_handle == original && closes == before &&
           editor.buffer.saved_text == saved && editor.buffer.undo.count == 1);
    assert(!strcmp(reed_buffer_path(&editor.buffer), "/other") &&
           reed_buffer_is_modified(&editor.buffer));
    creation_failure = MG_ERR_UNCERTAIN;
    assert(!save_as_action(&editor, "/maybe") && editor.save_uncertain);
    assert(editor.file_handle == original && closes == before &&
           editor.buffer.saved_text == saved && editor.buffer.undo.count == 1 &&
           !strcmp(reed_buffer_path(&editor.buffer), "/other"));
    creation_failure = 0;
    assert(save_as_action(&editor, "/saveas"));
    assert(editor.file_handle != original && closes == before + 1 &&
           !strcmp(reed_buffer_path(&editor.buffer), "/saveas") &&
           !editor.save_uncertain);
    assert(created.length == 4 && !memcmp(created.data, "copy", 4));
    assert(!reed_buffer_is_modified(&editor.buffer) &&
           editor.buffer.undo.count == 1);
    assert(reed_buffer_insert(&editor.buffer, 4, "!", 1));
    before = replacements;
    assert(save_action(&editor) && replacements == before + 1);
    assert(created.length == 5 && !memcmp(created.data, "copy!", 5));

    assert(editor_open_path(&editor, "/new") == MG_OK);
    assert(!editor.existing_file);
    assert(reed_buffer_insert(&editor.buffer, 0, "new", 3));
    saved = editor.buffer.saved_text;
    creation_failure = MG_ERR_NO_SPACE;
    before = creations;
    execute_save_command(&editor, true);
    assert(editor.running && creations == before + 1);
    assert(!editor.existing_file && editor.buffer.saved_text == saved &&
           !strcmp(reed_buffer_path(&editor.buffer), "/new") &&
           editor.buffer.undo.count == 0);
    creation_failure = MG_ERR_UNCERTAIN;
    assert(!save_action(&editor) && editor.save_uncertain);
    creation_failure = 0;
    before = creations;
    assert(!save_action(&editor) && creations == before);
    assert(save_as_action(&editor, "/alternate"));
    assert(!editor.save_uncertain && editor.existing_file &&
           !strcmp(reed_buffer_path(&editor.buffer), "/alternate"));
    assert(created.length == 4 && !memcmp(created.data, "new\n", 4));
    assert(!reed_buffer_is_modified(&editor.buffer));
    destroy_editor(&editor);
    assert(opens + created_handles == closes);

    /* An unnamed /s prompts, then binds the returned handle. */
    init_editor(&editor);
    assert(reed_buffer_insert(&editor.buffer, 0, "draft", 5));
    prompt_text = "";
    prompt_offset = 0; /* New empty input starts at its actual beginning. */
    before = creations;
    assert(!save_action(&editor) && creations == before &&
           !editor.existing_file && reed_buffer_is_modified(&editor.buffer));
    prompt_text = "/unnamed";
    prompt_offset = 0;
    assert(save_action(&editor));
    assert(editor.existing_file &&
           !strcmp(reed_buffer_path(&editor.buffer), "/unnamed"));
    assert(created.length == 6 && !memcmp(created.data, "draft\n", 6));
    assert(!reed_buffer_is_modified(&editor.buffer));
    assert(reed_buffer_insert(&editor.buffer, 5, "!", 1));
    before = replacements;
    execute_save_command(&editor, true);
    assert(!editor.running && replacements == before + 1);
    destroy_editor(&editor);
    assert(opens + created_handles == closes);

    /* A named new buffer saves through exclusive create after all allocation
     * and destination failures, and /sq quits only on confirmed success. */
    init_editor(&editor);
    assert(editor_open_path(&editor, "/new") == MG_OK);
    assert(reed_buffer_insert(&editor.buffer, 0, "named", 5));
    saved = editor.buffer.saved_text;
    before = creations;
    strcpy(editor.input, "saveas /file");
    editor.input_length = strlen(editor.input);
    editor.input_cursor = editor.input_length;
    execute_command_line(&editor);
    assert(creations == before + 1 && !editor.existing_file &&
           !strcmp(editor.status,
                   "Save refused: destination exists; edits kept."));
    before = creations;
    for (int budget = 0; budget < 3; budget++) {
        fail_allocation_after = budget;
        assert(!save_action(&editor) && creations == before);
        assert(editor.buffer.saved_text == saved && !editor.existing_file &&
               reed_buffer_is_modified(&editor.buffer));
    }
    creation_failure = MG_ERR_ACCESS_DENIED;
    assert(!save_action(&editor) && !editor.existing_file);
    creation_failure = MG_ERR_DEVICE_GONE;
    assert(!save_action(&editor) && !editor.existing_file);
    creation_failure = MG_ERR_ALREADY_EXISTS;
    execute_save_command(&editor, true);
    assert(editor.running && !editor.existing_file &&
           !strcmp(reed_buffer_path(&editor.buffer), "/new"));
    creation_failure = 0;
    execute_save_command(&editor, true);
    assert(!editor.running && editor.existing_file &&
           created.length == 6 && !memcmp(created.data, "named\n", 6));
    destroy_editor(&editor);

    init_editor(&editor);
    assert(reed_buffer_set_final_newline(&editor.buffer, false));
    assert(save_as_action(&editor, "/empty"));
    assert(created.length == 0 && !reed_buffer_is_modified(&editor.buffer));
    destroy_editor(&editor);

    init_editor(&editor);
    large = malloc(512U * 1024U + 1U);
    assert(large);
    memset(large, 'x', 512U * 1024U + 1U);
    assert(reed_buffer_replace(&editor.buffer, large, 512U * 1024U + 1U));
    assert(reed_buffer_set_final_newline(&editor.buffer, false));
    assert(save_as_action(&editor, "/over-old-limit"));
    assert(created.length == 512U * 1024U + 1U);
    free(large);
    destroy_editor(&editor);

    init_editor(&editor);
    large = malloc(MG_FILE_REPLACE_MAX_BYTES - 1U);
    assert(large);
    memset(large, 'x', MG_FILE_REPLACE_MAX_BYTES - 1U);
    assert(reed_buffer_replace(&editor.buffer, large,
                               MG_FILE_REPLACE_MAX_BYTES - 1U));
    assert(reed_buffer_set_final_newline(&editor.buffer, true));
    assert(save_as_action(&editor, "/maximum-newline"));
    assert(created.length == MG_FILE_REPLACE_MAX_BYTES &&
           created.data[MG_FILE_REPLACE_MAX_BYTES - 1U] == '\n');
    free(large);
    destroy_editor(&editor);

    init_editor(&editor);
    large = malloc(MG_FILE_REPLACE_MAX_BYTES);
    assert(large);
    memset(large, 'x', MG_FILE_REPLACE_MAX_BYTES);
    assert(reed_buffer_replace(&editor.buffer, large,
                               MG_FILE_REPLACE_MAX_BYTES));
    assert(reed_buffer_set_final_newline(&editor.buffer, false));
    assert(save_as_action(&editor, "/maximum"));
    assert(created.length == MG_FILE_REPLACE_MAX_BYTES &&
           created.data[MG_FILE_REPLACE_MAX_BYTES - 1U] == 'x');
    free(large);
    destroy_editor(&editor);

    init_editor(&editor);
    large = malloc(MG_FILE_REPLACE_MAX_BYTES + 1U);
    assert(large);
    memset(large, 'x', MG_FILE_REPLACE_MAX_BYTES + 1U);
    assert(reed_buffer_replace(&editor.buffer, large,
                               MG_FILE_REPLACE_MAX_BYTES));
    assert(reed_buffer_set_final_newline(&editor.buffer, true));
    before = creations;
    assert(!save_as_action(&editor, "/too-large") && creations == before);
    assert(reed_buffer_is_modified(&editor.buffer));
    free(large);
    destroy_editor(&editor);

    init_editor(&editor);
    assert(reed_buffer_insert(&editor.buffer, 0, "before open", 11));
    prompt_text = "s";
    prompt_next_text = "/saved-before-open";
    prompt_offset = 0;
    assert(open_action(&editor, "/other"));
    assert(created.length == 12 &&
           !memcmp(created.data, "before open\n", 12));
    assert(!strcmp(reed_buffer_path(&editor.buffer), "/other") &&
           editor.existing_file);
    destroy_editor(&editor);

    init_editor(&editor);
    assert(reed_buffer_insert(&editor.buffer, 0, "quit", 4));
    prompt_text = "s";
    prompt_next_text = "/quit-save";
    prompt_offset = 0;
    assert(quit_action(&editor) && !editor.running);
    assert(created.length == 5 && !memcmp(created.data, "quit\n", 5));
    destroy_editor(&editor);

    init_editor(&editor);
    assert(reed_buffer_insert(&editor.buffer, 0, "discard", 7));
    prompt_text = "d";
    prompt_offset = 0;
    assert(quit_action(&editor) && !editor.running);
    assert(reed_buffer_is_modified(&editor.buffer));
    destroy_editor(&editor);

    /* A stale /sq cannot quit. A later explicit Save As can bind a new
     * handle even when the conflicting file cannot be snapshotted. */
    init_editor(&editor);
    path_target = &first;
    assert(editor_open_path(&editor, "/other") == MG_OK);
    original = editor.file_handle;
    assert(reed_buffer_insert(&editor.buffer, 0, "x", 1));
    saved = editor.buffer.saved_text;
    second.generation++;
    prompt_text = "c";
    prompt_offset = 0;
    execute_save_command(&editor, true);
    assert(editor.running && editor.save_conflict &&
           editor.file_handle == original && editor.buffer.saved_text == saved &&
           reed_buffer_is_modified(&editor.buffer));
    snapshot_failure = MG_ERR_ACCESS_DENIED;
    prompt_text = "a";
    prompt_next_text = "/conflict-copy";
    prompt_offset = 0;
    execute_save_command(&editor, true);
    assert(!editor.running && !editor.save_conflict &&
           editor.file_handle != original &&
           !strcmp(reed_buffer_path(&editor.buffer), "/conflict-copy") &&
           !reed_buffer_is_modified(&editor.buffer));
    snapshot_failure = 0;
    destroy_editor(&editor);

    /* Save-before-open must keep the old document on Cancel, Enter, and an
     * unrecognized conflict key; intentional discard may then switch it. */
    init_editor(&editor);
    assert(editor_open_path(&editor, "/other") == MG_OK);
    original = editor.file_handle;
    assert(reed_buffer_insert(&editor.buffer, 0, "y", 1));
    second.generation++;
    prompt_text = "s";
    prompt_next_text = "c";
    prompt_offset = 0;
    closes_before = closes;
    assert(!open_action(&editor, "/file"));
    assert(editor.save_conflict && editor.file_handle == original &&
           closes == closes_before && reed_buffer_is_modified(&editor.buffer));
    before = replacements;
    prompt_text = "";
    prompt_offset = 0;
    assert(!save_action(&editor) && replacements == before &&
           editor.save_conflict);
    prompt_text = "x";
    prompt_offset = 0;
    assert(!save_action(&editor) && replacements == before &&
           editor.save_conflict);
    prompt_text = "\x1b";
    prompt_offset = 0;
    assert(!save_action(&editor) && replacements == before &&
           editor.save_conflict);
    prompt_text = "d";
    prompt_offset = 0;
    assert(open_action(&editor, "/file") && closes == closes_before + 1 &&
           !editor.save_conflict && editor.file_handle != original);
    destroy_editor(&editor);

    /* An exact-limit file with a trailing newline loads and saves without
     * losing the final byte or exceeding the serialized-content limit. */
    next_handle = 3;
    memset(first.data, 'x', MG_FILE_CONTENT_MAX_BYTES);
    first.data[MG_FILE_CONTENT_MAX_BYTES - 1U] = '\n';
    first.length = MG_FILE_CONTENT_MAX_BYTES;
    first.generation++;
    path_target = &first;
    init_editor(&editor);
    assert(editor_open_path(&editor, "/file") == MG_OK);
    assert(reed_buffer_length(&editor.buffer) ==
           MG_FILE_CONTENT_MAX_BYTES - 1U);
    assert(reed_buffer_has_final_newline(&editor.buffer));
    assert(save_action(&editor));
    assert(first.length == MG_FILE_CONTENT_MAX_BYTES &&
           first.data[MG_FILE_CONTENT_MAX_BYTES - 1U] == '\n');
    destroy_editor(&editor);
    first.data[MG_FILE_CONTENT_MAX_BYTES - 1U] = 'x';
    first.generation++;
    init_editor(&editor);
    assert(editor_open_path(&editor, "/file") == MG_OK);
    assert(reed_buffer_length(&editor.buffer) == MG_FILE_CONTENT_MAX_BYTES &&
           !reed_buffer_has_final_newline(&editor.buffer));
    assert(save_action(&editor));
    assert(first.length == MG_FILE_CONTENT_MAX_BYTES &&
           first.data[MG_FILE_CONTENT_MAX_BYTES - 1U] == 'x');
    destroy_editor(&editor);
    assert(opens + created_handles == closes);

    /* Relative Save As reaches the native anchored create unchanged. The
     * controlled syscall fixture tests editor state, not path resolution. */
    next_handle=3;
    init_editor(&editor);
    assert(reed_buffer_insert(&editor.buffer,0,"relative",8));
    assert(save_created_file(&editor,"../relative-copy")==MG_OK);
    assert(!strcmp(created_path,"../relative-copy"));
    assert(!strcmp(reed_buffer_path(&editor.buffer),"../relative-copy"));
    destroy_editor(&editor);
    assert(opens+created_handles==closes);

    /* Opening a line remains an in-memory edit when the backing store denies
     * replacement; the failed save must preserve the complete draft. */
    next_handle = 3;
    memcpy(first.data, "readonly", 8U);
    first.length = 8U;
    first.generation++;
    path_target = &first;
    replacement_failure = 0;
    init_editor(&editor);
    assert(editor_open_path(&editor, "/file") == MG_OK);
    assert(reed_buffer_cursor_set(&editor.buffer,
                                 reed_buffer_length(&editor.buffer)));
    assert(insert_newline(&editor, false));
    finish_insert(&editor);
    replacement_failure = MG_ERR_ACCESS_DENIED;
    assert(!save_action(&editor));
    assert(!strcmp(editor.status,
                   "Save denied: access changed; edits kept."));
    {
        char text[32];
        assert(reed_buffer_copy(&editor.buffer, text, sizeof(text), NULL));
        assert(!strcmp(text, "readonly\n"));
    }
    assert(reed_buffer_is_modified(&editor.buffer));
    replacement_failure = 0;
    destroy_editor(&editor);
    assert(opens + created_handles == closes);

    puts("Reed safe-save tests passed");
    return 0;
}
