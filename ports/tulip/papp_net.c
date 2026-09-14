// The _pappnet module: the loader's network services (psram_app.h net_*),
// for the port's socket, tls/ssl and network modules (ports/tulip/py).
//
// The device's firmware owns the network (Ethernet or Wi-Fi); Tulip uses it
// through the loader. Everything here returns at once; socket.py does the
// blocking, timeouts and stream buffering on top.
//
//   has(name)               is a group of services there: "ipv4", "resolve",
//                           "udp", "tcp", "tcp_server", "tls". Older loaders
//                           leave newer services NULL.
//   ipv4()                  (ip, netmask) as dotted strings, or None while the
//                           network is down.
//   resolve(host)           the IPv4 address of a name (a dotted string), or
//                           None. May wait for DNS.
//   udp_open(port, bcast)   } a Conn, or None when the loader refuses
//   tcp_connect(ip, port)   } (no free socket, bad address). tls_connect
//   tcp_listen(port)        } checks the server's certificate against the
//   tls_connect(host, port) } loader's CA bundle; it cannot be turned off.
//
// A Conn is one loader handle. Its methods return the loader's own numbers
// (see psram_app.h); socket.py turns them into Python semantics:
//   send(buf)        bytes taken, 0 would block, -1 error
//   recv(buf)        TCP: bytes, 0 closed, -2 nothing, -1 error
//                    TLS: bytes, 0 nothing yet, -1 closed or failed
//   sendto(buf, ip, port), recvfrom(buf) -> (n, ip, port)   UDP
//   accept()         None (nothing waiting), False (error) or (Conn, ip, port)
//   poll()           net_poll bits: 1 readable, 2 writable, 4 failed
//   status()         TLS: 1 ready, 0 connecting, -1 failed; else 1
//   close()          also run by the GC for a Conn nobody holds any more
#include "papp_port.h"

#include <stdio.h>
#include <string.h>

#include "py/mperrno.h"
#include "py/runtime.h"

static void no_service(void)
{
    mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("this PAPP loader has no such network service; update the loader"));
}

static bool has_tcp(void)
{
    return papp_svc->net_tcp_connect && papp_svc->net_tcp_send && papp_svc->net_tcp_recv &&
           papp_svc->net_poll && papp_svc->net_udp_close;
}

static bool has_tls(void)
{
    return papp_svc->net_tls_connect && papp_svc->net_tls_status && papp_svc->net_tls_send &&
           papp_svc->net_tls_recv && papp_svc->net_tls_close;
}

static bool has_udp(void)
{
    return papp_svc->net_udp_open && papp_svc->net_udp_send && papp_svc->net_udp_recv && papp_svc->net_udp_close;
}

static mp_obj_t ip_str(uint32_t ip)
{
    char text[16];
    int n = snprintf(text, sizeof(text), "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 0xff),
                     (unsigned)((ip >> 8) & 0xff), (unsigned)(ip & 0xff));
    return mp_obj_new_str(text, (size_t)n);
}

// A dotted quad in host byte order (psram_app.h's convention).
static uint32_t ip_parse(mp_obj_t text_in)
{
    const char *s = mp_obj_str_get_str(text_in);
    uint32_t ip = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') {
            mp_raise_ValueError(MP_ERROR_TEXT("expected an IPv4 address"));
        }
        unsigned value = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 4) {
            value = value * 10 + (unsigned)(*s++ - '0');
            digits++;
        }
        if (value > 255 || (part < 3 && *s++ != '.')) {
            mp_raise_ValueError(MP_ERROR_TEXT("expected an IPv4 address"));
        }
        ip = (ip << 8) | value;
    }
    if (*s != '\0') {
        mp_raise_ValueError(MP_ERROR_TEXT("expected an IPv4 address"));
    }
    return ip;
}

static uint16_t port_of(mp_obj_t port_in)
{
    mp_int_t port = mp_obj_get_int(port_in);
    if (port < 0 || port > 65535) {
        mp_raise_ValueError(MP_ERROR_TEXT("port out of range"));
    }
    return (uint16_t)port;
}

// ── Conn ────────────────────────────────────────────────────────────────────

typedef struct {
    mp_obj_base_t base;
    int handle;   // -1 once closed
    bool tls;     // a net_tls_* handle, else UDP/TCP (closed with net_udp_close)
} conn_obj_t;

static const mp_obj_type_t conn_type;

static mp_obj_t conn_new(int handle, bool tls)
{
    if (handle < 0) {
        return mp_const_none;
    }
    conn_obj_t *self = mp_obj_malloc_with_finaliser(conn_obj_t, &conn_type);
    self->handle = handle;
    self->tls = tls;
    return MP_OBJ_FROM_PTR(self);
}

static conn_obj_t *conn_open(mp_obj_t self_in)
{
    conn_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->handle < 0) {
        mp_raise_OSError(MP_EBADF);
    }
    return self;
}

static mp_obj_t conn_close(mp_obj_t self_in)
{
    conn_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->handle >= 0) {
        if (self->tls) {
            if (papp_svc->net_tls_close) {
                papp_svc->net_tls_close(self->handle);
            }
        } else if (papp_svc->net_udp_close) {
            papp_svc->net_udp_close(self->handle);
        }
        self->handle = -1;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(conn_close_obj, conn_close);

static mp_obj_t conn_send(mp_obj_t self_in, mp_obj_t buf_in)
{
    conn_obj_t *self = conn_open(self_in);
    mp_buffer_info_t buf;
    mp_get_buffer_raise(buf_in, &buf, MP_BUFFER_READ);
    int n;
    if (self->tls) {
        n = papp_svc->net_tls_send(self->handle, buf.buf, (int)buf.len);
    } else {
        if (!papp_svc->net_tcp_send) {
            no_service();
        }
        n = papp_svc->net_tcp_send(self->handle, buf.buf, (int)buf.len);
    }
    return MP_OBJ_NEW_SMALL_INT(n);
}
static MP_DEFINE_CONST_FUN_OBJ_2(conn_send_obj, conn_send);

static mp_obj_t conn_recv(mp_obj_t self_in, mp_obj_t buf_in)
{
    conn_obj_t *self = conn_open(self_in);
    mp_buffer_info_t buf;
    mp_get_buffer_raise(buf_in, &buf, MP_BUFFER_WRITE);
    if (buf.len == 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("empty buffer"));
    }
    int n;
    if (self->tls) {
        n = papp_svc->net_tls_recv(self->handle, buf.buf, (int)buf.len);
    } else {
        if (!papp_svc->net_tcp_recv) {
            no_service();
        }
        n = papp_svc->net_tcp_recv(self->handle, buf.buf, (int)buf.len);
    }
    return MP_OBJ_NEW_SMALL_INT(n);
}
static MP_DEFINE_CONST_FUN_OBJ_2(conn_recv_obj, conn_recv);

static mp_obj_t conn_sendto(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    conn_obj_t *self = conn_open(args[0]);
    mp_buffer_info_t buf;
    mp_get_buffer_raise(args[1], &buf, MP_BUFFER_READ);
    if (self->tls) {
        mp_raise_OSError(MP_EOPNOTSUPP);
    }
    uint32_t ip = ip_parse(args[2]);
    uint16_t port = port_of(args[3]);
    return MP_OBJ_NEW_SMALL_INT(papp_svc->net_udp_send(self->handle, buf.buf, (int)buf.len, ip, port));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(conn_sendto_obj, 4, 4, conn_sendto);

static mp_obj_t conn_recvfrom(mp_obj_t self_in, mp_obj_t buf_in)
{
    conn_obj_t *self = conn_open(self_in);
    mp_buffer_info_t buf;
    mp_get_buffer_raise(buf_in, &buf, MP_BUFFER_WRITE);
    if (self->tls) {
        mp_raise_OSError(MP_EOPNOTSUPP);
    }
    if (buf.len == 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("empty buffer"));
    }
    uint32_t ip = 0;
    uint16_t port = 0;
    int n = papp_svc->net_udp_recv(self->handle, buf.buf, (int)buf.len, &ip, &port);
    mp_obj_t items[3] = {MP_OBJ_NEW_SMALL_INT(n), ip_str(ip), MP_OBJ_NEW_SMALL_INT(port)};
    return mp_obj_new_tuple(3, items);
}
static MP_DEFINE_CONST_FUN_OBJ_2(conn_recvfrom_obj, conn_recvfrom);

static mp_obj_t conn_accept(mp_obj_t self_in)
{
    conn_obj_t *self = conn_open(self_in);
    if (self->tls || !papp_svc->net_tcp_accept) {
        mp_raise_OSError(MP_EOPNOTSUPP);
    }
    uint32_t ip = 0;
    uint16_t port = 0;
    int handle = papp_svc->net_tcp_accept(self->handle, &ip, &port);
    if (handle == -2) {
        return mp_const_none;
    }
    if (handle < 0) {
        return mp_const_false;
    }
    mp_obj_t items[3] = {conn_new(handle, false), ip_str(ip), MP_OBJ_NEW_SMALL_INT(port)};
    return mp_obj_new_tuple(3, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(conn_accept_obj, conn_accept);

static mp_obj_t conn_poll(mp_obj_t self_in)
{
    conn_obj_t *self = conn_open(self_in);
    if (!papp_svc->net_poll) {
        no_service();
    }
    return MP_OBJ_NEW_SMALL_INT(papp_svc->net_poll(self->handle));
}
static MP_DEFINE_CONST_FUN_OBJ_1(conn_poll_obj, conn_poll);

static mp_obj_t conn_status(mp_obj_t self_in)
{
    conn_obj_t *self = conn_open(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->tls ? papp_svc->net_tls_status(self->handle) : 1);
}
static MP_DEFINE_CONST_FUN_OBJ_1(conn_status_obj, conn_status);

static mp_obj_t conn_fileno(mp_obj_t self_in)
{
    conn_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->handle);
}
static MP_DEFINE_CONST_FUN_OBJ_1(conn_fileno_obj, conn_fileno);

static void conn_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind)
{
    (void)kind;
    conn_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "<Conn %s %d>", self->tls ? "tls" : "net", self->handle);
}

static const mp_rom_map_elem_t conn_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&conn_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_recv), MP_ROM_PTR(&conn_recv_obj) },
    { MP_ROM_QSTR(MP_QSTR_sendto), MP_ROM_PTR(&conn_sendto_obj) },
    { MP_ROM_QSTR(MP_QSTR_recvfrom), MP_ROM_PTR(&conn_recvfrom_obj) },
    { MP_ROM_QSTR(MP_QSTR_accept), MP_ROM_PTR(&conn_accept_obj) },
    { MP_ROM_QSTR(MP_QSTR_poll), MP_ROM_PTR(&conn_poll_obj) },
    { MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&conn_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_fileno), MP_ROM_PTR(&conn_fileno_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&conn_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&conn_close_obj) },
};
static MP_DEFINE_CONST_DICT(conn_locals_dict, conn_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    conn_type,
    MP_QSTR_Conn,
    MP_TYPE_FLAG_NONE,
    print, conn_print,
    locals_dict, &conn_locals_dict
    );

// ── Module ──────────────────────────────────────────────────────────────────

static mp_obj_t pappnet_has(mp_obj_t name_in)
{
    const char *name = mp_obj_str_get_str(name_in);
    bool ok = false;
    if (strcmp(name, "ipv4") == 0) {
        ok = papp_svc->net_ipv4 != NULL;
    } else if (strcmp(name, "resolve") == 0) {
        ok = papp_svc->net_resolve != NULL;
    } else if (strcmp(name, "udp") == 0) {
        ok = has_udp();
    } else if (strcmp(name, "tcp") == 0) {
        ok = has_tcp();
    } else if (strcmp(name, "tcp_server") == 0) {
        ok = has_tcp() && papp_svc->net_tcp_listen && papp_svc->net_tcp_accept;
    } else if (strcmp(name, "tls") == 0) {
        ok = has_tls();
    }
    return mp_obj_new_bool(ok);
}
static MP_DEFINE_CONST_FUN_OBJ_1(pappnet_has_obj, pappnet_has);

static mp_obj_t pappnet_ipv4(void)
{
    if (!papp_svc->net_ipv4) {
        no_service();
    }
    uint32_t ip = 0, mask = 0;
    if (!papp_svc->net_ipv4(&ip, &mask)) {
        return mp_const_none;
    }
    mp_obj_t items[2] = {ip_str(ip), ip_str(mask)};
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(pappnet_ipv4_obj, pappnet_ipv4);

static mp_obj_t pappnet_resolve(mp_obj_t host_in)
{
    if (!papp_svc->net_resolve) {
        no_service();
    }
    const char *host = mp_obj_str_get_str(host_in);
    uint32_t ip = 0;
    if (!papp_svc->net_resolve(host, &ip)) {
        return mp_const_none;
    }
    return ip_str(ip);
}
static MP_DEFINE_CONST_FUN_OBJ_1(pappnet_resolve_obj, pappnet_resolve);

static mp_obj_t pappnet_udp_open(mp_obj_t port_in, mp_obj_t broadcast_in)
{
    if (!has_udp()) {
        no_service();
    }
    return conn_new(papp_svc->net_udp_open(port_of(port_in), mp_obj_is_true(broadcast_in)), false);
}
static MP_DEFINE_CONST_FUN_OBJ_2(pappnet_udp_open_obj, pappnet_udp_open);

static mp_obj_t pappnet_tcp_connect(mp_obj_t ip_in, mp_obj_t port_in)
{
    if (!has_tcp()) {
        no_service();
    }
    return conn_new(papp_svc->net_tcp_connect(ip_parse(ip_in), port_of(port_in)), false);
}
static MP_DEFINE_CONST_FUN_OBJ_2(pappnet_tcp_connect_obj, pappnet_tcp_connect);

static mp_obj_t pappnet_tcp_listen(mp_obj_t port_in)
{
    if (!has_tcp() || !papp_svc->net_tcp_listen || !papp_svc->net_tcp_accept) {
        no_service();
    }
    return conn_new(papp_svc->net_tcp_listen(port_of(port_in)), false);
}
static MP_DEFINE_CONST_FUN_OBJ_1(pappnet_tcp_listen_obj, pappnet_tcp_listen);

static mp_obj_t pappnet_tls_connect(mp_obj_t host_in, mp_obj_t port_in)
{
    if (!has_tls()) {
        no_service();
    }
    return conn_new(papp_svc->net_tls_connect(mp_obj_str_get_str(host_in), port_of(port_in)), true);
}
static MP_DEFINE_CONST_FUN_OBJ_2(pappnet_tls_connect_obj, pappnet_tls_connect);

static const mp_rom_map_elem_t pappnet_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR__pappnet) },
    { MP_ROM_QSTR(MP_QSTR_has), MP_ROM_PTR(&pappnet_has_obj) },
    { MP_ROM_QSTR(MP_QSTR_ipv4), MP_ROM_PTR(&pappnet_ipv4_obj) },
    { MP_ROM_QSTR(MP_QSTR_resolve), MP_ROM_PTR(&pappnet_resolve_obj) },
    { MP_ROM_QSTR(MP_QSTR_udp_open), MP_ROM_PTR(&pappnet_udp_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_tcp_connect), MP_ROM_PTR(&pappnet_tcp_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_tcp_listen), MP_ROM_PTR(&pappnet_tcp_listen_obj) },
    { MP_ROM_QSTR(MP_QSTR_tls_connect), MP_ROM_PTR(&pappnet_tls_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_Conn), MP_ROM_PTR(&conn_type) },
};
static MP_DEFINE_CONST_DICT(pappnet_module_globals, pappnet_module_globals_table);

const mp_obj_module_t mp_module_pappnet = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&pappnet_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR__pappnet, mp_module_pappnet);
