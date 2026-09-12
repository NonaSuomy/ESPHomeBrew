// Interface list for the Red Alert PAPP's socket layer: the device's one IPv4
// interface, with its broadcast address.
#pragma once

#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ifaddrs
{
    struct ifaddrs* ifa_next;
    char* ifa_name;
    unsigned int ifa_flags;
    struct sockaddr* ifa_addr;
    struct sockaddr* ifa_netmask;
    struct sockaddr* ifa_broadaddr;
    void* ifa_data;
};

int getifaddrs(struct ifaddrs** ifap);
void freeifaddrs(struct ifaddrs* ifa);

#ifdef __cplusplus
}
#endif
