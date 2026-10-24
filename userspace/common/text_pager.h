/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/error.h>
#include <mg/types.h>

#define TEXT_PAGER_MAX_DOCUMENT_BYTES (4U * 1024U * 1024U)
#define TEXT_PAGER_MAX_VISUAL_ROWS 262144U
#define TEXT_PAGER_MAX_QUERY_BYTES 256U

typedef struct text_pager_visual_row {
    u32 byte_offset;
    u32 tab_remaining;
    u32 logical_column;
    u32 cells;
} text_pager_visual_row_t;

typedef struct text_pager_document {
    const u8 *text;
    usize length;
    u32 columns;
    u32 tab_width;
    text_pager_visual_row_t *rows;
    usize row_count;
} text_pager_document_t;

/* The caller owns the input bytes for the lifetime of the document. */
mg_result_t text_pager_document_init(text_pager_document_t *document,
                                     const void *text, usize length,
                                     u32 columns, u32 tab_width);
mg_result_t text_pager_document_relayout(text_pager_document_t *document,
                                         u32 columns);
void text_pager_document_destroy(text_pager_document_t *document);

/* Reads a bounded regular file into a new allocation. */
mg_result_t text_pager_read_file(const char *path, char **out_text,
                                 usize *out_length);

/* Displays an already loaded, read-only document and restores the console. */
mg_result_t text_pager_run(const char *title, const char *path,
                           const void *text, usize length);
