#ifndef FILE_ATTRIBUTES
#define FILE_ATTRIBUTES
#include <string>

struct FileAttributes {
        int64_t size;
        int64_t atime;
        int64_t mtime;
        int64_t ctime;
        uint32_t mode;
        uint32_t nlink;
        uint32_t uid;
        uint32_t gid;
    };

#endif