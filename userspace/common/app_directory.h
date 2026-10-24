/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/filesystem.h>

typedef enum mg_app_directory_kind {
    MG_APP_DIRECTORY_CONFIG = 0,
    MG_APP_DIRECTORY_DATA,
    MG_APP_DIRECTORY_STATE,
    MG_APP_DIRECTORY_CACHE,
} mg_app_directory_kind_t;

/* Resolve the authenticated process user's application directory. This is a
 * read-only lookup; it never creates any part of the path. */
mg_result_t app_directory_path(mg_app_directory_kind_t kind,
                               const char *application, char *path,
                               usize capacity);

/* Explicitly ensure the selected root and application directory exist. */
mg_result_t app_directory_ensure(mg_app_directory_kind_t kind,
                                 const char *application, char *path,
                                 usize capacity);
