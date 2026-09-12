// BSD sockets for the Red Alert PAPP, just enough for Vanilla Conquer's LAN
// play (common/wspudp.cpp: IPX packets over UDP broadcast). Each socket is a
// loader UDP handle (net_udp_*); descriptors 40..47 keep them apart from files
// and inside newlib's FD_SETSIZE for select().
#include "papp_port.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>

enum
{
    SOCKET_FD_BASE = PAPP_SOCKET_FD_BASE,
    SOCKET_COUNT = PAPP_SOCKET_FD_COUNT,
    DATAGRAM_MAX = 1500,
};

struct Socket
{
    bool used;
    int handle;        // loader handle, -1 until bound
    bool broadcast;
    bool pending;      // a datagram taken by select() and not yet read
    int pending_len;
    uint32_t pending_ip;
    uint16_t pending_port;
    unsigned char pending_buf[DATAGRAM_MAX];
};

static Socket s_sockets[SOCKET_COUNT];

// The one interface getifaddrs() reports; ifa first so the list pointer is
// the allocation.
struct IfEntry
{
    ifaddrs ifa;
    sockaddr_in addr, netmask, broadcast;
    char name[4];
};

static Socket* socket_of(int fd)
{
    if (fd < SOCKET_FD_BASE || fd >= SOCKET_FD_BASE + SOCKET_COUNT || !s_sockets[fd - SOCKET_FD_BASE].used) {
        return nullptr;
    }
    return &s_sockets[fd - SOCKET_FD_BASE];
}

static bool net_available()
{
    return papp_svc->net_udp_open != nullptr; // an older loader has no network
}

// Bind on first use: VC binds explicitly, but a send before bind must work too.
static bool ensure_open(Socket* s, uint16_t port)
{
    if (s->handle < 0) {
        s->handle = papp_svc->net_udp_open(port, 1);
    }
    return s->handle >= 0;
}

// Take one waiting datagram into the socket's lookahead, if there is one.
static bool poll_pending(Socket* s)
{
    if (s->pending || s->handle < 0) {
        return s->pending;
    }
    const int n = papp_svc->net_udp_recv(s->handle, s->pending_buf, DATAGRAM_MAX, &s->pending_ip, &s->pending_port);
    if (n > 0) {
        s->pending = true;
        s->pending_len = n;
    }
    return s->pending;
}

extern "C" {

int socket(int domain, int type, int protocol)
{
    (void)protocol;
    if (!net_available() || domain != AF_INET || type != SOCK_DGRAM) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    for (int i = 0; i < SOCKET_COUNT; i++) {
        if (!s_sockets[i].used) {
            memset(&s_sockets[i], 0, sizeof(s_sockets[i]));
            s_sockets[i].used = true;
            s_sockets[i].handle = -1;
            return SOCKET_FD_BASE + i;
        }
    }
    errno = EMFILE;
    return -1;
}

int bind(int fd, const struct sockaddr* addr, socklen_t len)
{
    Socket* s = socket_of(fd);
    if (s == nullptr || addr == nullptr || len < sizeof(sockaddr_in)) {
        errno = EBADF;
        return -1;
    }
    const sockaddr_in* in = reinterpret_cast<const sockaddr_in*>(addr);
    if (s->handle >= 0 || !ensure_open(s, ntohs(in->sin_port))) {
        errno = EADDRINUSE;
        return -1;
    }
    return 0;
}

int setsockopt(int fd, int level, int name, const void* value, socklen_t len)
{
    Socket* s = socket_of(fd);
    if (s == nullptr) {
        errno = EBADF;
        return -1;
    }
    if (level == SOL_SOCKET && name == SO_BROADCAST && value != nullptr && len >= sizeof(int)) {
        s->broadcast = *static_cast<const int*>(value) != 0; // loader sockets always allow it
    }
    return 0; // buffer sizes and linger: the loader's defaults do
}

int getsockopt(int fd, int level, int name, void* value, socklen_t* len)
{
    if (socket_of(fd) == nullptr) {
        errno = EBADF;
        return -1;
    }
    if (level == SOL_SOCKET && name == SO_ERROR && value != nullptr && len != nullptr && *len >= sizeof(int)) {
        *static_cast<int*>(value) = 0;
        *len = sizeof(int);
    }
    return 0;
}

int ioctl(int fd, unsigned long request, ...)
{
    if (socket_of(fd) == nullptr) {
        errno = EBADF;
        return -1;
    }
    (void)request; // FIONBIO: loader sockets are always non-blocking
    return 0;
}

ssize_t sendto(int fd, const void* buf, size_t len, int flags, const struct sockaddr* to, socklen_t to_len)
{
    (void)flags;
    Socket* s = socket_of(fd);
    if (s == nullptr || to == nullptr || to_len < sizeof(sockaddr_in)) {
        errno = EBADF;
        return -1;
    }
    if (!ensure_open(s, 0)) {
        errno = ENETDOWN;
        return -1;
    }
    const sockaddr_in* in = reinterpret_cast<const sockaddr_in*>(to);
    const int sent = papp_svc->net_udp_send(s->handle, buf, (int)len, ntohl(in->sin_addr.s_addr), ntohs(in->sin_port));
    if (sent == 0) {
        errno = EWOULDBLOCK;
        return -1;
    }
    if (sent < 0) {
        errno = EIO;
        return -1;
    }
    return sent;
}

ssize_t recvfrom(int fd, void* buf, size_t len, int flags, struct sockaddr* from, socklen_t* from_len)
{
    (void)flags;
    Socket* s = socket_of(fd);
    if (s == nullptr) {
        errno = EBADF;
        return -1;
    }
    if (!poll_pending(s)) {
        errno = EWOULDBLOCK;
        return -1;
    }
    const size_t n = (size_t)s->pending_len < len ? (size_t)s->pending_len : len;
    memcpy(buf, s->pending_buf, n);
    if (from != nullptr && from_len != nullptr && *from_len >= sizeof(sockaddr_in)) {
        sockaddr_in* in = reinterpret_cast<sockaddr_in*>(from);
        memset(in, 0, sizeof(*in));
        in->sin_family = AF_INET;
        in->sin_port = htons(s->pending_port);
        in->sin_addr.s_addr = htonl(s->pending_ip);
        *from_len = sizeof(sockaddr_in);
    }
    s->pending = false;
    return (ssize_t)n;
}

// Readable: a datagram is waiting. Writable: always. Never waits: the game
// polls every frame.
int select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds, struct timeval* timeout)
{
    (void)timeout;
    int ready = 0;
    for (int fd = 0; fd < nfds && fd < FD_SETSIZE; fd++) {
        Socket* s = socket_of(fd);
        if (readfds != nullptr && FD_ISSET(fd, readfds)) {
            if (s != nullptr && poll_pending(s)) {
                ready++;
            } else {
                FD_CLR(fd, readfds);
            }
        }
        if (writefds != nullptr && FD_ISSET(fd, writefds)) {
            if (s != nullptr && s->handle >= 0) {
                ready++;
            } else {
                FD_CLR(fd, writefds);
            }
        }
        if (exceptfds != nullptr) {
            FD_CLR(fd, exceptfds);
        }
    }
    return ready;
}

// papp_syscalls.c routes close() of a socket descriptor here.
int papp_socket_close(int fd)
{
    Socket* s = socket_of(fd);
    if (s == nullptr) {
        errno = EBADF;
        return -1;
    }
    if (s->handle >= 0) {
        papp_svc->net_udp_close(s->handle);
    }
    s->used = false;
    return 0;
}

// ── Addresses ────────────────────────────────────────────────────────────────

const char* inet_ntop(int af, const void* src, char* dst, socklen_t size)
{
    if (af != AF_INET || src == nullptr || dst == nullptr) {
        errno = EAFNOSUPPORT;
        return nullptr;
    }
    const unsigned char* b = static_cast<const unsigned char*>(src); // network order
    if (snprintf(dst, size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]) >= (int)size) {
        errno = ENOSPC;
        return nullptr;
    }
    return dst;
}

int inet_pton(int af, const char* src, void* dst)
{
    if (af != AF_INET || src == nullptr || dst == nullptr) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    unsigned a, b, c, d;
    char extra;
    if (sscanf(src, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 || a > 255 || b > 255 || c > 255 || d > 255) {
        return 0;
    }
    unsigned char* out = static_cast<unsigned char*>(dst);
    out[0] = (unsigned char)a;
    out[1] = (unsigned char)b;
    out[2] = (unsigned char)c;
    out[3] = (unsigned char)d;
    return 1;
}

in_addr_t inet_addr(const char* cp)
{
    in_addr_t addr;
    return inet_pton(AF_INET, cp, &addr) == 1 ? addr : INADDR_NONE;
}

char* inet_ntoa(struct in_addr in)
{
    static char text[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &in.s_addr, text, sizeof(text));
    return text;
}

// This device's address, in network order. False when the network is down.
static bool local_ipv4(uint32_t* ip_net, uint32_t* mask_net)
{
    uint32_t ip = 0, mask = 0;
    if (papp_svc->net_ipv4 == nullptr || !papp_svc->net_ipv4(&ip, &mask)) {
        return false;
    }
    *ip_net = htonl(ip);
    *mask_net = htonl(mask);
    return true;
}

struct hostent* gethostbyname(const char* name)
{
    (void)name; // only ever asked for this machine
    static uint32_t addr;
    static char* list[2] = {reinterpret_cast<char*>(&addr), nullptr};
    static char host_name[] = "redalert-p4";
    static struct hostent host = {host_name, nullptr, AF_INET, 4, list};
    uint32_t mask;
    return local_ipv4(&addr, &mask) ? &host : nullptr;
}

int getifaddrs(struct ifaddrs** ifap)
{
    uint32_t ip, mask;
    if (ifap == nullptr || !local_ipv4(&ip, &mask)) {
        errno = ENETDOWN;
        return -1;
    }
    IfEntry* e = new IfEntry();
    strcpy(e->name, "en0");
    e->addr.sin_family = e->netmask.sin_family = e->broadcast.sin_family = AF_INET;
    e->addr.sin_addr.s_addr = ip;
    e->netmask.sin_addr.s_addr = mask;
    e->broadcast.sin_addr.s_addr = ip | ~mask;
    e->ifa.ifa_name = e->name;
    e->ifa.ifa_addr = reinterpret_cast<sockaddr*>(&e->addr);
    e->ifa.ifa_netmask = reinterpret_cast<sockaddr*>(&e->netmask);
    e->ifa.ifa_broadaddr = reinterpret_cast<sockaddr*>(&e->broadcast);
    *ifap = &e->ifa; // ifa is the first member: freeifaddrs deletes the IfEntry
    return 0;
}

void freeifaddrs(struct ifaddrs* ifa)
{
    delete reinterpret_cast<IfEntry*>(ifa);
}

} // extern "C"
