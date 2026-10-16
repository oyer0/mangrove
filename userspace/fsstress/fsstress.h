/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FSSTRESS_H
#define FSSTRESS_H
#include <stdint.h>
#include <stddef.h>
#define FS_FILES 8U
#define FS_DIRS 4U
#define FS_BYTES 65536U
#define FS_OPS 1024U
#define FS_TRACE_BYTES (48U+32U*FS_OPS)
#define FS_OK 0
#define FS_MISSING (-2)
#define FS_EXISTS (-9)
#define FS_UNSUPPORTED (-7)
#define FS_NOTEMPTY (-16)
#define FS_IO (-12)
#define FS_UNCERTAIN (-33)
#define FS_STALE (-34)
#define FS_SAFE 1U
#define FS_GAPS 2U
#define FS_ASCII_CI 4U
#define FS_RESIZE 8U
/* IDs, not paths from the filesystem, define every model name. */
enum fs_action { ROOT, MKDIR, CREATE, WRITE, READ, REOPEN, TRUNCATE, MOVE,
    UNLINK, RMDIR, SNAPSHOT, REPLACE, STALE, CONTENTS, CASE, RESIZE, DMOVE, FS_ACTIONS };
typedef struct {uint32_t action,id,arg,offset,length;int32_t expected;uint64_t data;} fs_op;
typedef struct {uint64_t seed;uint32_t profile,caps,count,control;fs_op ops[FS_OPS];} fs_plan;
typedef struct {uint32_t live,parent,variant,size;} fs_file;
typedef struct {uint32_t live,parent;} fs_dir;
typedef struct {fs_file files[FS_FILES];fs_dir dirs[FS_DIRS];uint32_t root;} fs_shape;
typedef struct {uint32_t type;uint64_t size,identity;} fs_info;
typedef struct {int status;uint32_t count;} fs_result;
typedef struct {char name[32];uint32_t type;} fs_entry;
typedef struct {uint64_t identity,revision,length;} fs_version;
uint64_t fs_random(uint64_t *state);
uint32_t fs_pick(uint64_t *state,uint32_t bound);
void fs_payload(uint64_t seed,unsigned char *out,uint32_t count);
uint32_t fs_caps(uint32_t profile);
int fs_generate(fs_plan *plan,uint32_t profile,uint64_t seed,uint32_t count,uint32_t control);
int fs_validate(const fs_plan *plan);
int fs_predict(const fs_shape *shape,const fs_op *op,uint32_t caps);
void fs_transition(fs_shape *shape,const fs_op *op,int status);
int fs_path(const fs_shape *shape,uint32_t directory,uint32_t id,uint32_t variant,char *out,size_t cap);
size_t fs_encode(const fs_plan *plan,unsigned char *out);
int fs_decode(fs_plan *plan,const unsigned char *data,size_t length);
int fs_execute(const fs_plan *plan,const char *root,int verify);
int fs_main(int argc,char **argv);
/* Small platform boundary. No filesystem or allocation implementation here. */
int io_native(void);
int io_enter(const char *root);
int io_open(const char *path,int64_t *handle);
int io_close(int64_t handle);
int io_create(const char *path);
int io_mkdir(const char *path);
int io_remove(const char *path,int directory);
int io_move(const char *source,const char *dest);
int io_stat(const char *path,fs_info *info);
int io_seek(int64_t handle,uint32_t offset);
fs_result io_read(int64_t handle,void *data,uint32_t count);
fs_result io_write(int64_t handle,const void *data,uint32_t count);
int io_resize(int64_t handle,uint32_t size);
int io_list(const char *path,fs_entry *entries,uint32_t capacity,uint32_t *count);
int io_snapshot(int64_t handle,unsigned char *data,uint32_t capacity,fs_version *version);
int io_replace(int64_t handle,const unsigned char *data,uint32_t count);
int io_contents(const char *path,const unsigned char *data,uint32_t count,int64_t *handle);
int io_trace_save(const char *path,const unsigned char *data,uint32_t count);
int io_trace_load(const char *path,unsigned char *data,uint32_t capacity,uint32_t *count);
uint64_t io_milliseconds(void);
#endif
