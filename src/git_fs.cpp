#include "git_fs.h"

#include <SDCardManager.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

struct git_file {
    FsFile file;
};

extern "C" git_file *git_fs_open(const char *path, const char *mode)
{
    if (!path || !mode) return nullptr;

    const bool writing = strchr(mode, 'w') != nullptr;
    git_file *out = new (std::nothrow) git_file;
    if (!out) return nullptr;

    if (writing) {
        out->file = SdMan.open(path, O_WRONLY | O_CREAT | O_TRUNC);
    } else {
        out->file = SdMan.open(path, O_RDONLY);
    }
    if (!out->file) {
        delete out;
        return nullptr;
    }
    return out;
}

extern "C" size_t git_fs_read(void *buffer, size_t size, size_t count, git_file *file)
{
    if (!file || !buffer || size == 0 || count == 0 || count > SIZE_MAX / size) return 0;
    const size_t wanted = size * count;
    const int read = file->file.read(buffer, wanted);
    return read > 0 ? static_cast<size_t>(read) / size : 0;
}

extern "C" size_t git_fs_write(const void *buffer, size_t size, size_t count, git_file *file)
{
    if (!file || !buffer || size == 0 || count == 0 || count > SIZE_MAX / size) return 0;
    const size_t wanted = size * count;
    return file->file.write(static_cast<const uint8_t *>(buffer), wanted) / size;
}

extern "C" int git_fs_getc(git_file *file)
{
    return file ? file->file.read() : -1;
}

extern "C" int git_fs_seek(git_file *file, long offset, int origin)
{
    if (!file) return -1;
    int64_t base = 0;
    if (origin == SEEK_SET) {
        base = 0;
    } else if (origin == SEEK_CUR) {
        base = file->file.curPosition();
    } else if (origin == SEEK_END) {
        base = file->file.size();
    } else {
        return -1;
    }
    const int64_t position = base + offset;
    if (position < 0 || position > UINT32_MAX) return -1;
    return file->file.seekSet(static_cast<uint32_t>(position)) ? 0 : -1;
}

extern "C" long git_fs_tell(git_file *file)
{
    return file ? static_cast<long>(file->file.curPosition()) : -1;
}

extern "C" long git_fs_size(git_file *file)
{
    return file ? static_cast<long>(file->file.size()) : -1;
}

extern "C" int git_fs_close(git_file *file)
{
    if (!file) return -1;
    file->file.close();
    delete file;
    return 0;
}

extern "C" bool git_fs_exists(const char *path)
{
    return path && SdMan.exists(path);
}

extern "C" bool git_fs_mkdir(const char *path)
{
    if (!path || !path[0]) return false;
    if (SdMan.exists(path)) return true;
    return SdMan.mkdir(path, true);
}

extern "C" bool git_fs_remove(const char *path)
{
    return path && (!SdMan.exists(path) || SdMan.remove(path));
}

extern "C" bool git_fs_rename(const char *from, const char *to)
{
    if (!from || !to) return false;
    if (SdMan.exists(to) && !SdMan.remove(to)) return false;
    return SdMan.rename(from, to);
}

extern "C" bool git_fs_read_all(const char *path, char **data, size_t *length)
{
    if (!path || !data || !length) return false;
    *data = nullptr;
    *length = 0;

    git_file *file = git_fs_open(path, "rb");
    if (!file) return false;

    const long size = git_fs_size(file);
    if (size < 0) {
        git_fs_close(file);
        return false;
    }
    const size_t file_size = static_cast<size_t>(size);
    char *buffer = static_cast<char *>(malloc(file_size + 1));
    if (!buffer) {
        git_fs_close(file);
        return false;
    }
    size_t total = 0;
    while (total < file_size) {
        const size_t read = git_fs_read(buffer + total, 1, file_size - total, file);
        if (read == 0) break;
        total += read;
    }
    const bool ok = total == file_size;
    git_fs_close(file);
    if (!ok) {
        free(buffer);
        return false;
    }
    buffer[file_size] = '\0';
    *data = buffer;
    *length = file_size;
    return true;
}

extern "C" bool git_fs_write_all(const char *path, const void *data, size_t length)
{
    if (!path || (!data && length)) return false;
    git_file *file = git_fs_open(path, "wb");
    if (!file) return false;
    const bool ok = length == 0 || git_fs_write(data, 1, length, file) == length;
    const bool closed = git_fs_close(file) == 0;
    return ok && closed;
}
