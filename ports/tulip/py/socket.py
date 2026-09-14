# socket for Tulip on the ESP32-P4 PAPP loader.
#
# TCP and UDP over the device's own network (Ethernet or Wi-Fi, set up by its
# firmware), through the loader's net_* services (_pappnet, papp_net.c). IPv4
# only; addresses are (host, port) tuples and host may be a name.
#
# Sockets block by default. settimeout()/setblocking() work as usual: waits
# poll the loader with short sleeps, so Ctrl-C and Tulip's callbacks still
# run. read()/readinto()/readline()/write() make a socket a stream (what
# tuliprequests uses); the class is an io.IOBase, so select.poll and asyncio
# can wait on it too. tls.py adds TLS (HTTPS) on top.
#
# On a loader without these services every call raises OSError saying so.
import errno
import gc
import time

import _pappnet

try:
    from io import IOBase as _IOBase
except ImportError:
    _IOBase = object

AF_INET = 2
AF_INET6 = 10
SOCK_STREAM = 1
SOCK_DGRAM = 2
SOCK_RAW = 3
IPPROTO_IP = 0
IPPROTO_TCP = 6
IPPROTO_UDP = 17
SOL_SOCKET = 0xFFF
SO_REUSEADDR = 0x0004
SO_KEEPALIVE = 0x0008
SO_BROADCAST = 0x0020
TCP_NODELAY = 1

error = OSError

_CHUNK = 4096
# MicroPython stream ioctl requests and poll flags (py/stream.h).
_IOCTL_POLL = 3
_IOCTL_CLOSE = 4
_POLL_RD = 0x0001
_POLL_WR = 0x0004
_POLL_ERR = 0x0008
_POLL_HUP = 0x0010


def _need(service, what):
    if not _pappnet.has(service):
        raise OSError(errno.ENODEV, "%s needs a newer PAPP loader (it has no %s service)" % (what, service))


def _network_up():
    return not _pappnet.has("ipv4") or _pappnet.ipv4() is not None


def _is_ip(host):
    parts = host.split(".")
    if len(parts) != 4:
        return False
    for p in parts:
        if not p or len(p) > 3 or not p.isdigit() or int(p) > 255:
            return False
    return True


def _resolve(host):
    if isinstance(host, bytes):
        host = host.decode()
    if host in ("", "0.0.0.0"):
        return "0.0.0.0"
    if host == "localhost":
        return "127.0.0.1"
    if _is_ip(host):
        return host
    _need("resolve", "Looking up a host name")
    if not _network_up():
        raise OSError(errno.EHOSTUNREACH, "no network: the device is not connected")
    ip = _pappnet.resolve(host)
    if ip is None:
        raise OSError(errno.EHOSTUNREACH, "host not found: %s" % host)
    return ip


def _open(fn, *args):
    # A connection nobody holds any more still has its loader handle until
    # the GC finalises it: collect once before giving up on a free handle.
    conn = fn(*args)
    if conn is None:
        gc.collect()
        conn = fn(*args)
    return conn


def getaddrinfo(host, port, af=0, type=0, proto=0, flags=0):
    return [(AF_INET, type or SOCK_STREAM, proto, "", (_resolve(host), int(port)))]


def gethostbyname(host):
    return _resolve(host)


def _again(e):
    # EAGAIN: a non-blocking socket had nothing to do (MicroPython's e.errno is args[0]).
    return bool(e.args) and e.args[0] == errno.EAGAIN


def _data(data):
    if isinstance(data, str):
        data = data.encode()
    return memoryview(data)


class _Wait:
    # One wait for the loader: raises EAGAIN on a non-blocking socket and
    # ETIMEDOUT after the socket's timeout, else sleeps a little (yields at
    # first, then 10 ms steps).
    def __init__(self, timeout):
        self.timeout = timeout
        self.start = time.ticks_ms()

    def __call__(self):
        if self.timeout == 0:
            raise OSError(errno.EAGAIN)
        waited = time.ticks_diff(time.ticks_ms(), self.start)
        if self.timeout is not None and waited >= self.timeout * 1000:
            raise OSError(errno.ETIMEDOUT)
        time.sleep_ms(1 if waited < 20 else 10)


class socket(_IOBase):
    def __init__(self, af=AF_INET, type=SOCK_STREAM, proto=0, fileno=None):
        self._setup(type)
        if af != AF_INET:
            raise OSError(errno.EOPNOTSUPP, "only AF_INET (IPv4)")
        if type == SOCK_STREAM:
            _need("tcp", "A TCP socket")
        elif type == SOCK_DGRAM:
            _need("udp", "A UDP socket")
        else:
            raise OSError(errno.EOPNOTSUPP, "only SOCK_STREAM and SOCK_DGRAM")

    def _setup(self, type):
        self.type = type
        self._conn = None       # the _pappnet.Conn
        self._timeout = None    # None: blocking, 0: non-blocking, else seconds
        self._rbuf = b""        # read ahead by readline()
        self._eof = False
        self._closed = False
        self._scratch = None
        self._host = None       # what connect() was given, for TLS
        self._peer = None       # (ip, port)
        self._port = 0          # bind()
        self._broadcast = False

    def __repr__(self):
        return "<socket %s %s>" % ("tcp" if self.type == SOCK_STREAM else "udp", self._peer)

    # ── Options ──

    def settimeout(self, value):
        self._timeout = None if value is None else float(value)

    def gettimeout(self):
        return self._timeout

    def setblocking(self, flag):
        self._timeout = None if flag else 0

    def setsockopt(self, level, option, value):
        # Only SO_BROADCAST means anything here (for UDP, before bind or the
        # first sendto); the loader sets TCP_NODELAY and SO_REUSEADDR itself.
        if option == SO_BROADCAST:
            self._broadcast = bool(value)

    def fileno(self):
        return self._conn.fileno() if self._conn is not None else -1

    def makefile(self, mode="rb", buffering=0):
        return self

    def getpeername(self):
        if self._peer is None:
            raise OSError(errno.ENOTCONN)
        return self._peer

    def getsockname(self):
        ip = "0.0.0.0"
        if _pappnet.has("ipv4"):
            up = _pappnet.ipv4()
            if up is not None:
                ip = up[0]
        return (ip, self._port)

    # ── Connecting ──

    def _live(self):
        if self._closed:
            raise OSError(errno.EBADF)
        if self._conn is None:
            raise OSError(errno.ENOTCONN)
        return self._conn

    def connect(self, address):
        if self._closed:
            raise OSError(errno.EBADF)
        host, port = address[0], int(address[1])
        ip = _resolve(host)
        if self.type == SOCK_DGRAM:
            self._peer = (ip, port)
            return
        if self._conn is not None:
            raise OSError(errno.EALREADY)
        if not _network_up():
            raise OSError(errno.EHOSTUNREACH, "no network: the device is not connected")
        conn = _open(_pappnet.tcp_connect, ip, port)
        if conn is None:
            raise OSError(errno.ENOBUFS, "the loader could not open a connection (too many open?)")
        self._conn = conn
        self._host = host if isinstance(host, str) else host.decode()
        self._peer = (ip, port)
        if self._timeout == 0:
            raise OSError(errno.EINPROGRESS)
        wait = _Wait(self._timeout)
        while True:
            bits = conn.poll()
            if bits < 0 or bits & 4:
                self._drop()
                raise OSError(errno.ECONNREFUSED, "cannot connect to %s:%d" % (ip, port))
            if bits & 2:
                return
            try:
                wait()
            except OSError:
                self._drop()
                raise

    def bind(self, address):
        port = int(address[1])
        if self.type == SOCK_DGRAM:
            if self._conn is not None:
                raise OSError(errno.EINVAL, "already bound")
            self._conn = _open(_pappnet.udp_open, port, self._broadcast)
            if self._conn is None:
                raise OSError(errno.EADDRINUSE, "cannot open UDP port %d" % port)
        self._port = port

    def listen(self, backlog=None):
        if self.type != SOCK_STREAM:
            raise OSError(errno.EOPNOTSUPP)
        _need("tcp_server", "A listening socket")
        self._conn = _open(_pappnet.tcp_listen, self._port)
        if self._conn is None:
            raise OSError(errno.EADDRINUSE, "cannot listen on port %d" % self._port)

    def accept(self):
        conn = self._live()
        wait = None
        while True:
            got = conn.accept()
            if got is False:
                raise OSError(errno.EIO)
            if got is not None:
                s = socket(AF_INET, SOCK_STREAM)
                s._conn = got[0]
                s._peer = (got[1], got[2])
                s._timeout = self._timeout
                return s, s._peer
            if wait is None:
                wait = _Wait(self._timeout)
            wait()

    def _drop(self):
        if self._conn is not None:
            self._conn.close()
            self._conn = None

    def close(self):
        self._drop()
        self._closed = True
        self._rbuf = b""
        self._scratch = None

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    # ── The loader's numbers as Python results (tls.SSLSocket has its own) ──

    def _raw_recv(self, buf):
        # > 0 bytes, 0 at the end of the stream, None when nothing is waiting
        n = self._live().recv(buf)
        if n > 0:
            return n
        if n == 0:
            return 0
        if n == -2:
            return None
        raise OSError(errno.ECONNRESET)

    def _raw_send(self, data):
        # > 0 bytes taken, None when the loader would block
        n = self._live().send(data)
        if n > 0:
            return n
        if n == 0:
            return None
        raise OSError(errno.ECONNRESET)

    # ── Reading ──

    def _chunk(self, n):
        # Up to n new bytes (at least one), b"" at the end of the stream.
        if self._eof:
            return b""
        if self._scratch is None:
            self._scratch = bytearray(_CHUNK)
        buf = memoryview(self._scratch)[:min(n, _CHUNK)]
        wait = None
        while True:
            got = self._raw_recv(buf)
            if got is None:
                if wait is None:
                    wait = _Wait(self._timeout)
                wait()
            elif got == 0:
                self._eof = True
                return b""
            else:
                return bytes(buf[:got])

    def recv(self, bufsize, flags=0):
        if self.type == SOCK_DGRAM:
            return self.recvfrom(bufsize)[0]
        if bufsize <= 0:
            return b""
        if self._rbuf:
            data, self._rbuf = self._rbuf[:bufsize], self._rbuf[bufsize:]
            return data
        return self._chunk(bufsize)

    def read(self, size=-1):
        # A stream read: size bytes (fewer only at the end of the stream), or
        # everything up to the end for size < 0. A non-blocking socket gives
        # what is there, or None.
        if self.type == SOCK_DGRAM:
            return self.recv(_CHUNK if size is None or size < 0 else size)
        parts = []
        got = 0
        if self._rbuf:
            parts.append(self._rbuf)
            got = len(self._rbuf)
            self._rbuf = b""
        try:
            while size is None or size < 0 or got < size:
                want = _CHUNK if size is None or size < 0 else size - got
                data = self._chunk(want)
                if not data:
                    break
                parts.append(data)
                got += len(data)
        except OSError as e:
            if not _again(e):
                raise
            if not parts:
                return None
        data = b"".join(parts)
        if size is not None and 0 <= size < len(data):
            data, self._rbuf = data[:size], data[size:]
        return data

    def readinto(self, buf, nbytes=None):
        mv = memoryview(buf)
        if nbytes is not None:
            mv = mv[:nbytes]
        data = self.read(len(mv))
        if data is None:
            return None
        mv[:len(data)] = data
        return len(data)

    def recv_into(self, buf, nbytes=0, flags=0):
        mv = memoryview(buf)
        data = self.recv(nbytes or len(mv))
        mv[:len(data)] = data
        return len(data)

    def readline(self, limit=-1):
        while True:
            i = self._rbuf.find(b"\n")
            if i >= 0 or (0 <= limit <= len(self._rbuf)):
                end = i + 1 if i >= 0 else limit
                if 0 <= limit < end:
                    end = limit
                line, self._rbuf = self._rbuf[:end], self._rbuf[end:]
                return line
            try:
                data = self._chunk(_CHUNK)
            except OSError as e:
                if not _again(e):
                    raise
                if not self._rbuf:
                    return None  # a non-blocking socket with nothing there yet
                data = b""  # ... or the part of a line there is
            if not data:
                line, self._rbuf = self._rbuf, b""
                return line
            self._rbuf += data

    def readlines(self):
        lines = []
        while True:
            line = self.readline()
            if not line:
                return lines
            lines.append(line)

    # ── Writing ──

    def _send_some(self, data):
        wait = None
        while True:
            n = self._raw_send(data)
            if n is not None:
                return n
            if wait is None:
                wait = _Wait(self._timeout)
            wait()

    def send(self, data, flags=0):
        if self.type == SOCK_DGRAM:
            if self._peer is None:
                raise OSError(errno.ENOTCONN)
            return self.sendto(data, self._peer)
        data = _data(data)
        if not len(data):
            return 0
        return self._send_some(data)

    def sendall(self, data, flags=0):
        if self.type == SOCK_DGRAM:
            self.send(data)
            return
        data = _data(data)
        sent = 0
        # After a "would block" the same bytes go again (TLS needs that).
        while sent < len(data):
            sent += self._send_some(data[sent:])

    def write(self, data):
        # A stream write: all of it on a blocking socket; on a non-blocking one
        # what fits now, or None.
        data = _data(data)
        if self.type == SOCK_DGRAM:
            return self.send(data)
        if self._timeout == 0:
            try:
                return self._send_some(data) if len(data) else 0
            except OSError as e:
                if _again(e):
                    return None
                raise
        self.sendall(data)
        return len(data)

    def flush(self):
        pass

    # ── UDP ──

    def _udp(self):
        if self._closed:
            raise OSError(errno.EBADF)
        if self._conn is None:
            self._conn = _open(_pappnet.udp_open, self._port, self._broadcast)
            if self._conn is None:
                raise OSError(errno.EADDRINUSE, "cannot open a UDP socket")
        return self._conn

    def sendto(self, data, address):
        if self.type != SOCK_DGRAM:
            return self.send(data)
        ip, port = _resolve(address[0]), int(address[1])
        data = _data(data)
        conn = self._udp()
        wait = None
        while True:
            n = conn.sendto(data, ip, port)
            if n > 0 or (n == 0 and not len(data)):
                return n
            if n < 0:
                raise OSError(errno.EIO, "cannot send to %s:%d" % (ip, port))
            if wait is None:
                wait = _Wait(self._timeout)
            wait()

    def recvfrom(self, bufsize):
        if self.type != SOCK_DGRAM:
            return self.recv(bufsize), self._peer
        conn = self._udp()
        buf = bytearray(bufsize)
        wait = None
        while True:
            n, ip, port = conn.recvfrom(buf)
            if n > 0:
                return bytes(buf[:n]), (ip, port)
            if n < 0:
                raise OSError(errno.EIO)
            if wait is None:
                wait = _Wait(self._timeout)
            wait()

    # ── Stream protocol (io.IOBase): select.poll and asyncio ──

    def ioctl(self, request, arg):
        if request == _IOCTL_CLOSE:
            self.close()
            return 0
        if request != _IOCTL_POLL:
            return -errno.EINVAL
        if self._closed:
            return _POLL_HUP
        ready = _POLL_RD if (self._rbuf or self._eof) else 0
        if self._conn is not None:
            bits = self._conn.poll()
            if bits < 0:
                ready |= _POLL_ERR
            else:
                if bits & 1:
                    ready |= _POLL_RD
                if bits & 2:
                    ready |= _POLL_WR
                if bits & 4:
                    ready |= _POLL_ERR
        return ready & (arg | _POLL_ERR | _POLL_HUP)
