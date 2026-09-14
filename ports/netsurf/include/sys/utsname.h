// <sys/utsname.h> for the NetSurf PAPP: uname() (papp_syscalls.c) names
// the platform in NetSurf's user agent string.
#ifndef PAPP_SYS_UTSNAME_H
#define PAPP_SYS_UTSNAME_H

#ifdef __cplusplus
extern "C" {
#endif

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

int uname(struct utsname *buf);

#ifdef __cplusplus
}
#endif

#endif
