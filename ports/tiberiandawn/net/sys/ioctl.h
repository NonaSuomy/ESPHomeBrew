// ioctl for the Tiberian Dawn PAPP's sockets: only FIONBIO, and they are always
// non-blocking.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define FIONBIO 0x5421

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif
