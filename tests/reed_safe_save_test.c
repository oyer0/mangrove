/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production Reed load/save functions with controlled native file calls. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../userspace/reed/editor.c"

typedef struct {
    char data[MG_FILE_REPLACE_MAX_BYTES + 1U];
    usize length;
    u64 generation;
} test_file_t;

static test_file_t first, second;
static test_file_t *path_target;
static test_file_t *handles[32];
static u64 expectations[32];
static unsigned next_handle = 3, opens, closes, snapshots, replacements;
static mg_result_t snapshot_failure, replacement_failure;
static bool report_short_size;
static int fail_allocation_after = -1;
static u64 reported_size_override = (u64)-1;

bool result_is_error(mg_result_t result)
{
    return result < 0;
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
    if (!strcmp(path, "/new")) return MG_ERR_NOT_FOUND;
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
    editor->running = true;
}

static void destroy_editor(reed_editor_t *editor)
{
    if (editor->existing_file) assert(handle_close(editor->file_handle) == MG_OK);
    reed_buffer_destroy(&editor->buffer);
}

int main(void)
{
    reed_editor_t editor;
    mg_handle_t original;
    unsigned before;
    char *saved;

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
    assert(save_action(&editor));
    assert(first.length == 8 && !memcmp(first.data, "old one\n", 8));
    assert(second.length == 5 && !reed_buffer_is_modified(&editor.buffer));
    assert(editor.file_handle == original);
    assert(reed_buffer_insert(&editor.buffer, 7, "!", 1));
    assert(save_action(&editor));
    assert(first.generation == 9 && first.length == 9);

    saved = editor.buffer.saved_text;
    assert(reed_buffer_insert(&editor.buffer, 8, "x", 1));
    first.generation++;
    before = replacements;
    assert(!save_action(&editor) && replacements == before + 1);
    assert(!strcmp(editor.status,
                   "Save refused: file changed externally; edits kept."));
    assert(editor.buffer.saved_text == saved && reed_buffer_is_modified(&editor.buffer));
    replacement_failure = MG_ERR_UNCERTAIN;
    execute_save_command(&editor, true); /* /sq must stay open on failure. */
    assert(editor.running && editor.save_uncertain);
    assert(!strcmp(editor.status,
                   "Save outcome uncertain; edits kept. Do not retry."));
    assert(editor.buffer.saved_text == saved && reed_buffer_is_modified(&editor.buffer));
    assert(editor.buffer.undo.count == 1);
    replacement_failure = 0;

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
    replacement_failure = MG_ERR_UNCERTAIN;
    assert(!save_action(&editor) && editor.save_uncertain);
    assert(!reed_buffer_is_modified(&editor.buffer));
    replacement_failure = 0;
    assert(reed_buffer_replace(&editor.buffer, large,
                               MG_FILE_REPLACE_MAX_BYTES + 1U));
    before = replacements;
    assert(!save_action(&editor) && replacements == before);
    assert(reed_buffer_is_modified(&editor.buffer));
    free(large);

    second.length = 0;
    assert(editor_open_path(&editor, "/other") == MG_OK);
    assert(reed_buffer_length(&editor.buffer) == 0 &&
           !reed_buffer_has_final_newline(&editor.buffer));
    assert(save_action(&editor) && second.length == 0);

    assert(editor_open_path(&editor, "/new") == MG_OK);
    assert(!editor.existing_file && !handles[original]);
    assert(reed_buffer_insert(&editor.buffer, 0, "new", 3));
    before = replacements;
    assert(!save_action(&editor) && replacements == before);
    assert(!strcmp(editor.status,
                   "Save unavailable: safe creation is not supported yet."));
    assert(reed_buffer_is_modified(&editor.buffer));
    destroy_editor(&editor);
    assert(opens == closes);
    puts("Reed safe-save tests passed");
    return 0;
}
