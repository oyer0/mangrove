#pragma once

#include <types.h>

const char *kmon_get_cwd(void);
void kmon_set_cwd(const char *path);

void kmon_pwd(int argc, char **argv);
void kmon_cd(int argc, char **argv);
void kmon_ls(int argc, char **argv);
void kmon_cat(int argc, char **argv);
void kmon_touch(int argc, char **argv);
void kmon_mkdir(int argc, char **argv);
void kmon_rm(int argc, char **argv);
void kmon_rmdir(int argc, char **argv);
