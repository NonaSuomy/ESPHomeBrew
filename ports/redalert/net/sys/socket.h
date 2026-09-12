// Minimal BSD sockets for the Red Alert PAPP. newlib has no network headers;
// these declare just what Vanilla Conquer's UDP code uses. The functions are
// in papp_net.cpp, on top of the loader's net_udp_* services.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t socklen_t;
typedef uint16_t sa_family_t;

struct sockaddr
{
    sa_family_t sa_family;
    char sa_data[14];
};

#define AF_INET  2
#define PF_INET  AF_INET
#define AF_UNSPEC 0

#define SOCK_STREAM 1
#define SOCK_DGRAM  2

#define SOL_SOCKET   0xfff
#define SO_REUSEADDR 0x0004
#define SO_BROADCAST 0x0020
#define SO_LINGER    0x0080
#define SO_SNDBUF    0x1001
#define SO_RCVBUF    0x1002
#define SO_ERROR     0x1007

struct linger
{
    int l_onoff;
    int l_linger;
};

int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr* addr, socklen_t len);
int setsockopt(int fd, int level, int name, const void* value, socklen_t len);
int getsockopt(int fd, int level, int name, void* value, socklen_t* len);
ssize_t sendto(int fd, const void* buf, size_t len, int flags, const struct sockaddr* to, socklen_t to_len);
ssize_t recvfrom(int fd, void* buf, size_t len, int flags, struct sockaddr* from, socklen_t* from_len);

#ifdef __cplusplus
}
#endif
