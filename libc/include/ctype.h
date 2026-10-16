/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif
int isalnum(int character);
int isalpha(int character);
int isblank(int character);
int iscntrl(int character);
int isdigit(int character);
int isgraph(int character);
int islower(int character);
int isprint(int character);
int ispunct(int character);
int isspace(int character);
int isupper(int character);
int isxdigit(int character);
int tolower(int character);
int toupper(int character);
#ifdef __cplusplus
}
#endif
