#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct git_file git_file;

git_file *git_fs_open(const char *path, const char *mode);
size_t git_fs_read(void *buffer, size_t size, size_t count, git_file *file);
size_t git_fs_write(const void *buffer, size_t size, size_t count, git_file *file);
int git_fs_getc(git_file *file);
int git_fs_seek(git_file *file, long offset, int origin);
long git_fs_tell(git_file *file);
long git_fs_size(git_file *file);
int git_fs_close(git_file *file);

bool git_fs_exists(const char *path);
bool git_fs_mkdir(const char *path);
bool git_fs_remove(const char *path);
bool git_fs_rename(const char *from, const char *to);
bool git_fs_read_all(const char *path, char **data, size_t *length);
bool git_fs_write_all(const char *path, const void *data, size_t length);

#ifdef __cplusplus
}
#endif
