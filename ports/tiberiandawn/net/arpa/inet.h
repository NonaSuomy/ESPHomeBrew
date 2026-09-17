// Address text conversion for the Tiberian Dawn PAPP's socket layer.
#pragma once

#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INET_ADDRSTRLEN 16

const char* inet_ntop(int af, const void* src, char* dst, socklen_t size);
int inet_pton(int af, const char* src, void* dst);
in_addr_t inet_addr(const char* cp);
char* inet_ntoa(struct in_addr in);

#ifdef __cplusplus
}
#endif
