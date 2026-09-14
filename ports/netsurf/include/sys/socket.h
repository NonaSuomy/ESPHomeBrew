// <sys/socket.h> for the NetSurf PAPP. NetSurf only needs the types for
// its headers (utils/inet.h); all networking goes through the loader's
// net_* services (papp_http.c).
#ifndef PAPP_SYS_SOCKET_H
#define PAPP_SYS_SOCKET_H

#include <stdint.h>
#include <sys/types.h>

typedef uint32_t socklen_t;
typedef uint8_t sa_family_t;

struct sockaddr {
    uint8_t sa_len;
    sa_family_t sa_family;
    char sa_data[14];
};

#define AF_UNSPEC 0
#define AF_INET 2
#define AF_INET6 10
#define PF_INET AF_INET
#define PF_INET6 AF_INET6

#define SOCK_STREAM 1
#define SOCK_DGRAM 2

// NetSurf's default socket_open hook (unused: no libcurl); always fails.
int socket(int domain, int type, int protocol);

#endif
