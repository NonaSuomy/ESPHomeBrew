// <dirent.h> for the NetSurf PAPP. The toolchain's newlib has none, and the
// loader cannot list directories: opendir() always fails (papp_syscalls.c),
// so NetSurf's file: fetcher reports a directory as unreadable.
#ifndef PAPP_DIRENT_H
#define PAPP_DIRENT_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8

struct dirent {
    ino_t d_ino;
    unsigned char d_type;
    char d_name[256];
};

typedef struct papp_dir DIR;

DIR *opendir(const char *name);
struct dirent *readdir(DIR *dir);
int closedir(DIR *dir);
void rewinddir(DIR *dir);

#ifdef __cplusplus
}
#endif

#endif
