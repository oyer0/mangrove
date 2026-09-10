/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/line_editor.h>
#include "builtin.h"

mg_result_t shell_complete_line(mg_line_editor_t *editor, void *context);
