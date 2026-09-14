// <netinet/in.h> for the NetSurf PAPP: the address types NetSurf's URL
// database uses to recognise numeric hosts (IPv4 only: NO_IPV6).
#ifndef PAPP_NETINET_IN_H
#define PAPP_NETINET_IN_H

#include <stdint.h>
#include <sys/socket.h>

typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;

struct in_addr {
    in_addr_t s_addr;
};

struct in6_addr {
    uint8_t s6_addr[16];
};

struct sockaddr_in {
    uint8_t sin_len;
    sa_family_t sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    char sin_zero[8];
};

#define INADDR_ANY ((in_addr_t)0)
#define INET_ADDRSTRLEN 16
#define INET6_ADDRSTRLEN 46
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17

// The ESP32-P4 is little-endian.
#ifndef htons
#define htons(x) ((uint16_t)__builtin_bswap16((uint16_t)(x)))
#define ntohs(x) htons(x)
#define htonl(x) ((uint32_t)__builtin_bswap32((uint32_t)(x)))
#define ntohl(x) htonl(x)
#endif

#endif
