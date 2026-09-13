// TCP options for the Red Alert PAPP's socket layer (see sys/socket.h).
// The loader sets TCP_NODELAY on every TCP socket; setsockopt accepts it.
#pragma once

#include <netinet/in.h>

#define TCP_NODELAY 1
