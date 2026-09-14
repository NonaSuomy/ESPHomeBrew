"""Tests for the Tulip port's network modules (ports/tulip/py: socket, tls,
ssl, network) under CPython, with a fake _pappnet (the loader's services,
papp_net.c) and a fake MicroPython clock.

Run: python3 -m unittest discover -s tests
"""

import errno
import importlib.util
import io
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path

PY = Path(__file__).resolve().parent.parent / "ports" / "tulip" / "py"
PORT_MODULES = ("socket", "tls", "ssl", "network")


class FakeTime:
    """time.ticks_ms / ticks_diff / ticks_add / sleep_ms; sleeping moves the clock."""

    def __init__(self):
        self.now = 0
        self.sleeps = 0

    def ticks_ms(self):
        return self.now

    def ticks_diff(self, a, b):
        return a - b

    def ticks_add(self, a, b):
        return a + b

    def sleep_ms(self, ms):
        self.now += ms
        self.sleeps += 1


class FakeConn:
    """One loader handle. recv/send/poll/status follow scripts of the loader's
    return values; bytes in the recv script are delivered (possibly split)."""

    def __init__(self, kind, target):
        self.kind = kind          # "tcp", "tls", "udp", "listen"
        self.target = target
        self.recv_script = []
        self.send_script = []     # 0: would block, k > 0: take at most k, -1: error
        self.poll_script = [2]
        self.status_script = [1]
        self.attempts = []        # the bytes offered to each send
        self.sent = bytearray()
        self.closed = False

    @staticmethod
    def _next(script):
        return script.pop(0) if len(script) > 1 else script[0]

    def recv(self, buf):
        assert not self.closed
        if not self.recv_script:
            return 0 if self.kind == "tls" else -2
        item = self.recv_script[0]
        if isinstance(item, bytes):
            n = min(len(buf), len(item))
            buf[:n] = item[:n]
            if n < len(item):
                self.recv_script[0] = item[n:]
            else:
                self.recv_script.pop(0)
            return n
        self.recv_script.pop(0)
        return item

    def send(self, data):
        assert not self.closed
        data = bytes(data)
        self.attempts.append(data)
        limit = self.send_script.pop(0) if self.send_script else len(data)
        if limit <= 0:
            return limit
        taken = data[:limit]
        self.sent += taken
        return len(taken)

    def poll(self):
        return self._next(self.poll_script)

    def status(self):
        return self._next(self.status_script) if self.kind == "tls" else 1

    def fileno(self):
        return 7

    def close(self):
        self.closed = True


class FakeNet:
    """_pappnet: which services exist, the network state and the connections made."""

    def __init__(self):
        self.services = {"ipv4", "resolve", "udp", "tcp", "tcp_server", "tls"}
        self.address = ("192.168.1.50", "255.255.255.0")
        self.names = {"example.com": "93.184.216.34", "tulipcc-production.up.railway.app": "66.33.22.1"}
        self.conns = []
        self.next_tls = None      # a FakeConn to hand out on tls_connect
        self.refuse = 0           # how many opens return None

    def has(self, name):
        return name in self.services

    def ipv4(self):
        return self.address

    def resolve(self, host):
        return self.names.get(host)

    def _conn(self, kind, target):
        if self.refuse:
            self.refuse -= 1
            return None
        conn = FakeConn(kind, target)
        self.conns.append(conn)
        return conn

    def tcp_connect(self, ip, port):
        return self._conn("tcp", (ip, port))

    def tcp_listen(self, port):
        return self._conn("listen", port)

    def udp_open(self, port, broadcast):
        return self._conn("udp", (port, broadcast))

    def tls_connect(self, host, port):
        if self.refuse:
            self.refuse -= 1
            return None
        conn = self.next_tls or FakeConn("tls", (host, port))
        conn.kind, conn.target = "tls", (host, port)
        self.conns.append(conn)
        return conn


def code(error):
    """An OSError's errno: MicroPython sets it from OSError(n) too, CPython only
    with a message."""
    return error.errno if error.errno is not None else error.args[0]


def load_port(net, clock):
    """Import the port's modules with _pappnet and time faked, then put the
    real (CPython) modules back in sys.modules."""
    names = PORT_MODULES + ("_pappnet", "time")
    saved = {name: sys.modules.get(name) for name in names}
    loaded = {}
    try:
        sys.modules["_pappnet"] = net
        sys.modules["time"] = clock
        for name in PORT_MODULES:
            sys.modules.pop(name, None)
        for name in PORT_MODULES:
            spec = importlib.util.spec_from_file_location(name, PY / f"{name}.py")
            module = importlib.util.module_from_spec(spec)
            sys.modules[name] = module
            spec.loader.exec_module(module)
            loaded[name] = module
    finally:
        for name, module in saved.items():
            if module is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = module
    return loaded


class PortTest(unittest.TestCase):
    def setUp(self):
        self.net = FakeNet()
        self.clock = FakeTime()
        mods = load_port(self.net, self.clock)
        self.socket, self.tls, self.ssl, self.network = (mods[n] for n in PORT_MODULES)

    def tcp(self, host="example.com", port=80):
        s = self.socket.socket(self.socket.AF_INET, self.socket.SOCK_STREAM)
        s.connect(self.socket.getaddrinfo(host, port)[0][-1])
        return s, self.net.conns[-1]


class AddressTest(PortTest):
    def test_getaddrinfo_resolves_names_and_keeps_dotted_quads(self):
        self.assertEqual(self.socket.getaddrinfo("example.com", 443, 0, self.socket.SOCK_STREAM),
                         [(2, 1, 0, "", ("93.184.216.34", 443))])
        self.assertEqual(self.socket.getaddrinfo("10.0.0.2", "80")[0][-1], ("10.0.0.2", 80))

    def test_unknown_host_and_no_network_say_so(self):
        with self.assertRaises(OSError) as e:
            self.socket.getaddrinfo("nowhere.invalid", 80)
        self.assertIn("host not found", str(e.exception))
        self.net.address = None
        with self.assertRaises(OSError) as e:
            self.socket.getaddrinfo("example.com", 80)
        self.assertIn("no network", str(e.exception))

    def test_an_older_loader_raises_clear_errors(self):
        self.net.services = set()
        for call in (lambda: self.socket.socket(), lambda: self.socket.getaddrinfo("example.com", 80),
                     lambda: self.socket.socket(self.socket.AF_INET, self.socket.SOCK_DGRAM)):
            with self.assertRaises(OSError) as e:
                call()
            self.assertEqual(code(e.exception), errno.ENODEV)
            self.assertIn("newer PAPP loader", str(e.exception))


class TcpTest(PortTest):
    def test_connect_waits_until_writable(self):
        self.net.tcp_connect = self._slow_connect
        s, conn = self.tcp()
        self.assertEqual(conn.target, ("93.184.216.34", 80))
        self.assertGreaterEqual(self.clock.sleeps, 2)
        self.assertEqual(s.getpeername(), ("93.184.216.34", 80))

    def _slow_connect(self, ip, port):
        conn = FakeNet._conn(self.net, "tcp", (ip, port))
        conn.poll_script = [0, 0, 2]
        return conn

    def test_connect_refused_and_timeout(self):
        s = self.socket.socket()
        orig = self.net.tcp_connect

        def refused(ip, port):
            conn = orig(ip, port)
            conn.poll_script = [0, 4]
            return conn
        self.net.tcp_connect = refused
        with self.assertRaises(OSError) as e:
            s.connect(("example.com", 80))
        self.assertEqual(code(e.exception), errno.ECONNREFUSED)
        self.assertTrue(self.net.conns[-1].closed)

        def silent(ip, port):
            conn = orig(ip, port)
            conn.poll_script = [0]
            return conn
        self.net.tcp_connect = silent
        s = self.socket.socket()
        s.settimeout(0.5)
        with self.assertRaises(OSError) as e:
            s.connect(("example.com", 80))
        self.assertEqual(code(e.exception), errno.ETIMEDOUT)
        self.assertTrue(self.net.conns[-1].closed)
        self.assertGreaterEqual(self.clock.now, 500)

    def test_non_blocking_connect_is_in_progress(self):
        s = self.socket.socket()
        s.setblocking(False)
        with self.assertRaises(OSError) as e:
            s.connect(("example.com", 80))
        self.assertEqual(code(e.exception), errno.EINPROGRESS)

    def test_stream_reads(self):
        s, conn = self.tcp()
        conn.recv_script = [-2, b"HTTP/1.0 200 OK\r\nA: b", -2, b"\r\n\r\nbody-", b"rest", 0]
        self.assertEqual(s.readline(), b"HTTP/1.0 200 OK\r\n")
        self.assertEqual(s.readline(), b"A: b\r\n")
        self.assertEqual(s.readline(), b"\r\n")
        self.assertEqual(s.read(3), b"bod")
        self.assertEqual(s.read(), b"y-rest")
        self.assertEqual(s.read(10), b"")
        self.assertEqual(s.recv(10), b"")

    def test_read_n_waits_for_all_of_it_and_recv_does_not(self):
        s, conn = self.tcp()
        conn.recv_script = [b"ab", -2, b"cd", b"ef"]
        self.assertEqual(s.recv(10), b"ab")
        self.assertEqual(s.read(4), b"cdef")

    def test_readinto(self):
        s, conn = self.tcp()
        conn.recv_script = [b"12345", 0]
        buf = bytearray(3)
        self.assertEqual(s.readinto(buf), 3)
        self.assertEqual(buf, b"123")
        self.assertEqual(s.readinto(buf), 2)
        self.assertEqual(buf[:2], b"45")

    def test_non_blocking_reads(self):
        s, conn = self.tcp()
        s.setblocking(False)
        conn.recv_script = [-2, b"x", -2]
        with self.assertRaises(OSError) as e:
            s.recv(5)
        self.assertEqual(code(e.exception), errno.EAGAIN)
        self.assertEqual(s.read(5), b"x")
        self.assertIsNone(s.read(5))
        self.assertIsNone(s.readline())

    def test_read_timeout(self):
        s, conn = self.tcp()
        s.settimeout(0.2)
        with self.assertRaises(OSError) as e:
            s.recv(5)
        self.assertEqual(code(e.exception), errno.ETIMEDOUT)

    def test_reset_is_an_error(self):
        s, conn = self.tcp()
        conn.recv_script = [-1]
        with self.assertRaises(OSError) as e:
            s.recv(5)
        self.assertEqual(code(e.exception), errno.ECONNRESET)

    def test_sendall_retries_the_same_bytes(self):
        s, conn = self.tcp()
        conn.send_script = [3, 0, 0, 4]
        s.sendall(b"0123456789")
        self.assertEqual(bytes(conn.sent), b"0123456789")
        self.assertEqual(conn.attempts[:4], [b"0123456789", b"3456789", b"3456789", b"3456789"])

    def test_write_takes_str_and_bytes(self):
        s, conn = self.tcp()
        self.assertEqual(s.write("Host: x\r\n"), 9)
        self.assertEqual(s.write(b"%s /%s HTTP/1.0\r\n" % (b"GET", b"a")), 17)
        self.assertEqual(bytes(conn.sent), b"Host: x\r\nGET /a HTTP/1.0\r\n")

    def test_close_releases_the_handle(self):
        s, conn = self.tcp()
        with s:
            pass
        self.assertTrue(conn.closed)
        with self.assertRaises(OSError):
            s.recv(1)

    def test_a_refused_open_collects_garbage_and_tries_again(self):
        self.net.refuse = 1
        s, conn = self.tcp()
        self.assertEqual(conn.target, ("93.184.216.34", 80))
        self.net.refuse = 2
        with self.assertRaises(OSError) as e:
            self.socket.socket().connect(("example.com", 80))
        self.assertEqual(code(e.exception), errno.ENOBUFS)

    def test_poll_ioctl(self):
        s, conn = self.tcp()
        conn.poll_script = [3]
        self.assertEqual(s.ioctl(3, 0x1 | 0x4), 0x5)
        conn.poll_script = [4]
        self.assertEqual(s.ioctl(3, 0x1), 0x8)


class UdpTest(PortTest):
    def test_sendto_and_recvfrom(self):
        s = self.socket.socket(self.socket.AF_INET, self.socket.SOCK_DGRAM)
        s.setsockopt(self.socket.SOL_SOCKET, self.socket.SO_BROADCAST, 1)
        s.bind(("", 5000))
        conn = self.net.conns[-1]
        self.assertEqual(conn.target, (5000, True))
        sent = []
        conn.sendto = lambda data, ip, port: sent.append((bytes(data), ip, port)) or len(data)
        replies = [(0, "0.0.0.0", 0), (3, "192.168.1.7", 6000)]

        def recvfrom(buf):
            n, ip, port = replies.pop(0)
            buf[:n] = b"abc"[:n]
            return n, ip, port
        conn.recvfrom = recvfrom
        self.assertEqual(s.sendto(b"hi", ("255.255.255.255", 5000)), 2)
        self.assertEqual(sent, [(b"hi", "255.255.255.255", 5000)])
        self.assertEqual(s.recvfrom(16), (b"abc", ("192.168.1.7", 6000)))


class TlsTest(PortTest):
    def https(self, host="tulipcc-production.up.railway.app", tls_conn=None, timeout=None):
        """What tuliprequests does for an https:// URL."""
        if tls_conn is not None:
            self.net.next_tls = tls_conn
        ai = self.socket.getaddrinfo(host, 443, 0, self.socket.SOCK_STREAM)[0]
        s = self.socket.socket(ai[0], self.socket.SOCK_STREAM, ai[2])
        if timeout is not None:
            s.settimeout(timeout)
        s.connect(ai[-1])
        tcp = self.net.conns[-1]
        context = self.tls.SSLContext(self.tls.PROTOCOL_TLS_CLIENT)
        context.verify_mode = self.tls.CERT_NONE
        return context.wrap_socket(s, server_hostname=host), tcp, self.net.conns[-1]

    def test_wrap_socket_hands_the_connection_to_the_loader(self):
        conn = FakeConn("tls", None)
        conn.status_script = [0, 0, 1]
        conn.recv_script = [0, b"HTTP/1.0 200 OK\r\n\r\n{}", -1]
        s, tcp, tls = self.https(tls_conn=conn)
        self.assertTrue(tcp.closed)
        self.assertEqual(tls.target, ("tulipcc-production.up.railway.app", 443))
        s.write(b"GET /api HTTP/1.0\r\n")
        s.write("Host: x\r\n\r\n")
        self.assertEqual(bytes(tls.sent), b"GET /api HTTP/1.0\r\nHost: x\r\n\r\n")
        self.assertEqual(s.readline(), b"HTTP/1.0 200 OK\r\n")
        self.assertEqual(s.readline(), b"\r\n")
        self.assertEqual(s.read(), b"{}")   # -1 with status 1: the server closed
        s.close()
        self.assertTrue(tls.closed)

    def test_a_failed_handshake_raises_and_closes(self):
        conn = FakeConn("tls", None)
        conn.status_script = [0, -1]
        with self.assertRaises(OSError) as e:
            self.https(tls_conn=conn)
        self.assertEqual(code(e.exception), errno.ECONNABORTED)
        self.assertIn("certificate", str(e.exception))
        self.assertTrue(conn.closed)

    def test_a_handshake_timeout(self):
        conn = FakeConn("tls", None)
        conn.status_script = [0]
        with self.assertRaises(OSError) as e:
            self.https(tls_conn=conn, timeout=1)
        self.assertEqual(code(e.exception), errno.ETIMEDOUT)
        self.assertTrue(conn.closed)

    def test_an_error_after_the_handshake(self):
        conn = FakeConn("tls", None)
        conn.status_script = [1, -1]
        conn.recv_script = [b"par", -1]
        s, _, _ = self.https(tls_conn=conn)
        self.assertEqual(s.recv(10), b"par")
        with self.assertRaises(OSError) as e:
            s.recv(10)
        self.assertEqual(code(e.exception), errno.ECONNABORTED)

    def test_tls_send_retries_the_same_bytes(self):
        conn = FakeConn("tls", None)
        conn.send_script = [0, 5, 0]
        s, _, tls = self.https(tls_conn=conn)
        s.write(b"0123456789")
        self.assertEqual(tls.attempts, [b"0123456789", b"0123456789", b"56789", b"56789"])
        self.assertEqual(bytes(tls.sent), b"0123456789")

    def test_ssl_wrap_socket_and_no_tls_service(self):
        s = self.socket.socket()
        s.connect(("example.com", 443))
        w = self.ssl.wrap_socket(s, server_hostname="example.com")
        self.assertEqual(self.net.conns[-1].target, ("example.com", 443))
        w.close()
        self.net.services.discard("tls")
        s = self.socket.socket()
        s.connect(("example.com", 443))
        with self.assertRaises(OSError) as e:
            self.ssl.wrap_socket(s, server_hostname="example.com")
        self.assertEqual(code(e.exception), errno.ENODEV)
        self.assertIn("TLS", str(e.exception))

    def test_certificates_cannot_be_switched_off_or_replaced(self):
        context = self.ssl.create_default_context()
        context.verify_mode = self.ssl.CERT_NONE   # accepted, changes nothing
        with self.assertRaises(OSError):
            context.load_verify_locations(cadata=b"-----BEGIN CERTIFICATE-----")
        with self.assertRaises(OSError):
            self.tls.SSLContext(self.tls.PROTOCOL_TLS_SERVER)

    def test_no_free_session(self):
        self.net.refuse = 0
        s = self.socket.socket()
        s.connect(("example.com", 443))
        self.net.refuse = 2
        with self.assertRaises(OSError) as e:
            self.ssl.wrap_socket(s, server_hostname="example.com")
        self.assertEqual(code(e.exception), errno.ENOBUFS)


class NetworkTest(PortTest):
    def test_wlan_reports_the_device_network(self):
        wlan = self.network.WLAN(self.network.STA_IF)
        self.assertTrue(wlan.active(True))
        self.assertIs(wlan.isconnected(), True)
        self.assertEqual(wlan.ifconfig(), ("192.168.1.50", "255.255.255.0", "0.0.0.0", "0.0.0.0"))
        self.assertEqual(wlan.status(), self.network.STAT_GOT_IP)
        out = io.StringIO()
        with redirect_stdout(out):
            wlan.connect("ssid", "password")
        self.assertIn("own network", out.getvalue())

    def test_what_tulip_ip_and_wifi_do(self):
        # tulip.ip(): WLAN(STA_IF).isconnected() and ifconfig()[0]
        sta = self.network.WLAN(self.network.STA_IF)
        self.assertEqual(sta.ifconfig()[0] if sta.isconnected() else None, "192.168.1.50")
        self.net.address = None
        self.assertIs(sta.isconnected(), False)
        self.assertEqual(sta.ifconfig()[0], "0.0.0.0")
        self.assertEqual(sta.status(), self.network.STAT_IDLE)

    def test_an_older_loader(self):
        self.net.services = set()
        wlan = self.network.WLAN(self.network.STA_IF)
        self.assertIs(wlan.isconnected(), False)
        out = io.StringIO()
        with redirect_stdout(out):
            wlan.connect("ssid", "password")
        self.assertIn("update the loader", out.getvalue())


if __name__ == "__main__":
    unittest.main()
