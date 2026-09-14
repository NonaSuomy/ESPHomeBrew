# tls for Tulip on the ESP32-P4 PAPP loader (MicroPython's TLS module;
# ssl.py builds on it).
#
# The loader makes the TLS connection (esp-tls, net_tls_* in psram_app.h):
# wrapping a connected socket hands its host and port to the loader, which
# connects again, checks the server's certificate against the firmware's CA
# bundle (the name must match server_hostname, which is also sent as SNI) and
# does the handshake; the plain TCP connection is dropped. The result is a
# stream like any socket (read, readline, write, ...).
#
# Certificates are always checked. verify_mode is accepted for compatibility
# (tuliprequests sets CERT_NONE), but cannot turn the check off, and the loader
# has no place for custom CAs or client certificates. Only clients.
import errno

import _pappnet
import socket as _socket

PROTOCOL_TLS_CLIENT = 0
PROTOCOL_TLS_SERVER = 1
PROTOCOL_DTLS_CLIENT = 2
PROTOCOL_DTLS_SERVER = 3
CERT_NONE = 0
CERT_OPTIONAL = 1
CERT_REQUIRED = 2
MBEDTLS_VERSION = "esp-tls (the PAPP loader)"


def _failed(host, port):
    return OSError(errno.ECONNABORTED,
                   "TLS connection to %s:%d failed (name, network, certificate or handshake; "
                   "the loader's log says which)" % (host, port))


class SSLSocket(_socket.socket):
    def __init__(self, conn, host, peer, timeout):
        self._setup(_socket.SOCK_STREAM)
        self._conn = conn
        self._host = host
        self._peer = peer
        self._timeout = timeout

    def __repr__(self):
        return "<SSLSocket %s:%d>" % (self._host, self._peer[1])

    def do_handshake(self):
        # Wait for the loader's handshake. Non-blocking sockets return at once
        # and reads/writes give EAGAIN / None until it is done.
        conn = self._live()
        wait = None
        while True:
            status = conn.status()
            if status == 1:
                return
            if status < 0:
                self.close()
                raise _failed(self._host, self._peer[1])
            if self._timeout == 0:
                return
            if wait is None:
                wait = _socket._Wait(self._timeout)
            try:
                wait()
            except OSError:
                self.close()
                raise

    def _raw_recv(self, buf):
        conn = self._live()
        n = conn.recv(buf)
        if n > 0:
            return n
        if n == 0:
            return None  # nothing yet (or still connecting)
        if conn.status() == 1:
            return 0  # the server closed the connection
        raise _failed(self._host, self._peer[1])

    def _raw_send(self, data):
        conn = self._live()
        n = conn.send(data)
        if n > 0:
            return n
        if n == 0:
            return None  # would block (or still connecting): send the same bytes again
        if conn.status() == 1:
            raise OSError(errno.ECONNRESET, "the TLS connection was closed")
        raise _failed(self._host, self._peer[1])

    def connect(self, address):
        raise OSError(errno.EALREADY)

    def getpeercert(self, binary_form=False):
        return None  # verified by the loader, which does not keep it

    def cipher(self):
        return None


def _wrap(sock, server_hostname, do_handshake):
    _socket._need("tls", "TLS (HTTPS)")
    if not isinstance(sock, _socket.socket) or sock.type != _socket.SOCK_STREAM:
        raise OSError(errno.EOPNOTSUPP, "TLS needs a TCP socket from this socket module")
    if sock._peer is None:
        raise OSError(errno.ENOTCONN, "connect the socket before wrapping it")
    host = server_hostname or sock._host or sock._peer[0]
    if isinstance(host, bytes):
        host = host.decode()
    port = sock._peer[1]
    timeout = sock._timeout
    peer = sock._peer
    # The loader opens its own connection to the same server.
    sock.close()
    conn = _socket._open(_pappnet.tls_connect, host, port)
    if conn is None:
        raise OSError(errno.ENOBUFS,
                      "the loader could not start a TLS connection "
                      "(at most 4 at once, or its firmware has no CA bundle)")
    s = SSLSocket(conn, host, peer, timeout)
    if do_handshake:
        s.do_handshake()
    return s


class SSLContext:
    def __init__(self, protocol=PROTOCOL_TLS_CLIENT):
        if protocol != PROTOCOL_TLS_CLIENT:
            raise OSError(errno.EOPNOTSUPP, "only TLS clients (PROTOCOL_TLS_CLIENT)")
        self.protocol = protocol
        self.verify_mode = CERT_REQUIRED  # the loader always verifies
        self.check_hostname = True

    def load_verify_locations(self, cafile=None, cadata=None):
        raise OSError(errno.EOPNOTSUPP, "the loader checks certificates against its own CA bundle")

    def load_cert_chain(self, certfile, keyfile=None):
        raise OSError(errno.EOPNOTSUPP, "client certificates are not supported")

    def load_default_certs(self, purpose=None):
        pass

    def set_ciphers(self, ciphers):
        pass

    def wrap_socket(self, sock, server_side=False, do_handshake_on_connect=True, server_hostname=None,
                    do_handshake=None):
        if server_side:
            raise OSError(errno.EOPNOTSUPP, "only TLS clients")
        if do_handshake is None:
            do_handshake = do_handshake_on_connect
        return _wrap(sock, server_hostname, do_handshake)
