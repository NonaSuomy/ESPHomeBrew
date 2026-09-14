# webdrop: a small web server in Tulip for running code and dropping files
# onto it from another computer on the network.
#
#   >>> import webdrop
#   >>> webdrop.start()          # prints the address to open, with its key
#   >>> webdrop.stop()
#
# The page it serves has a box to run Python in Tulip (the REPL's own
# globals, output shown back) and file upload, list and download for the
# current folder. The same works from a script:
#
#   curl --data-binary @song.py "http://<ip>:8080/run?key=<key>"
#   curl --data-binary @song.py "http://<ip>:8080/upload?key=<key>&name=song.py"
#   curl "http://<ip>:8080/get?key=<key>&name=song.py" -o song.py
#
# Anyone who can reach the port and knows the key can run code on this
# Tulip, so the key is random for each start() and only printed here. It
# polls from tulip.defer(), so the REPL and apps keep running meanwhile.
import errno
import io
import json
import os
import sys

import socket
import tulip


def _print_exception(e, stream=None):
    # MicroPython has sys.print_exception; CPython (for tests) has traceback.
    if hasattr(sys, "print_exception"):
        return sys.print_exception(e, stream) if stream is not None else sys.print_exception(e)
    import traceback
    traceback.print_exception(type(e), e, e.__traceback__, file=stream)

PORT = 8080
_POLL_MS = 50
_server = None
_key = None
_globals = None

_PAGE = """<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Tulip webdrop</title>
<style>body{font:14px system-ui,sans-serif;margin:16px;max-width:860px}textarea{width:100%;height:14em;font:13px monospace}
pre{background:#111;color:#cfc;padding:8px;white-space:pre-wrap;min-height:2em}button{padding:6px 14px}
li{margin:2px 0}</style></head><body><h2>Tulip webdrop</h2>
<h3>Run Python</h3><textarea id="code">tulip.midi_out((144, 60, 100))</textarea><br>
<button onclick="run()">Run</button> <small>Ctrl-Enter runs too</small><pre id="out"></pre>
<h3>Files in <span id="cwd"></span></h3><input type="file" id="file" multiple> <button onclick="up()">Upload</button>
<ul id="ls"></ul>
<script>
const key=new URLSearchParams(location.search).get("key")||"";
const q=(p,o)=>fetch(p+(p.includes("?")?"&":"?")+"key="+encodeURIComponent(key),o);
async function run(){const r=await q("/run",{method:"POST",body:document.getElementById("code").value});
document.getElementById("out").textContent=await r.text();ls();}
async function up(){for(const f of document.getElementById("file").files){
await q("/upload?name="+encodeURIComponent(f.name),{method:"POST",body:f});}ls();}
async function ls(){const r=await q("/ls");if(!r.ok){document.getElementById("out").textContent=await r.text();return;}
const d=await r.json();document.getElementById("cwd").textContent=d.cwd;
document.getElementById("ls").innerHTML=d.files.map(f=>'<li><a href="/get?key='+encodeURIComponent(key)+
'&name='+encodeURIComponent(f)+'">'+f.replace(/[<&]/g,"")+'</a></li>').join("");}
document.getElementById("code").addEventListener("keydown",e=>{if(e.ctrlKey&&e.key==="Enter")run();});ls();
</script></body></html>"""


def _query(target):
    path, qs = (target.split("?", 1) + [""])[:2]
    args = {}
    for part in qs.split("&"):
        if "=" in part:
            name, value = part.split("=", 1)
            args[_unquote(name)] = _unquote(value)
    return path, args


def _unquote(text):
    text = text.replace("+", " ")
    out = bytearray()
    i = 0
    while i < len(text):
        if text[i] == "%" and i + 2 < len(text):
            try:
                out.append(int(text[i + 1:i + 3], 16))
                i += 3
                continue
            except ValueError:
                pass
        out.extend(text[i].encode())
        i += 1
    return out.decode()


def _safe_name(name):
    # A file name in the current folder, or a path on the SD card.
    if not name or "\x00" in name:
        return None
    if name.startswith("/sd/") and ".." not in name.split("/"):
        return name
    if "/" in name or name in (".", ".."):
        return None
    return name


def _reply(conn, status, body, kind="text/plain; charset=utf-8"):
    if isinstance(body, str):
        body = body.encode()
    head = "HTTP/1.0 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n" % (status, kind, len(body))
    conn.sendall(head.encode())
    if body:
        conn.sendall(body)


def _read_request(conn):
    # The request line and headers, then (method, target, headers, body reader).
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(1024)
        if not chunk:
            break
        data += chunk
        if len(data) > 16384:
            raise OSError(errno.EMSGSIZE, "headers too long")
    end = data.find(b"\r\n\r\n")
    head, rest = (data, b"") if end < 0 else (data[:end], data[end + 4:])
    lines = head.decode().split("\r\n")
    method, target = lines[0].split(" ")[:2]
    headers = {}
    for line in lines[1:]:
        if ":" in line:
            name, value = line.split(":", 1)
            headers[name.strip().lower()] = value.strip()
    return method, target, headers, rest


def _body_chunks(conn, headers, first):
    remaining = int(headers.get("content-length", "0")) - len(first)
    if first:
        yield first
    while remaining > 0:
        chunk = conn.recv(min(4096, remaining))
        if not chunk:
            break
        remaining -= len(chunk)
        yield chunk


def _run(code):
    out = io.StringIO()
    env = _globals

    def captured_print(*args, **kwargs):
        kwargs.pop("file", None)
        print(*args, file=out, **kwargs)
        print(*args, **kwargs)  # on Tulip's screen too

    saved = env.get("print")
    env["print"] = captured_print
    try:
        try:
            result = eval(code, env)  # a single expression shows its value
            if result is not None:
                captured_print(repr(result))
        except SyntaxError:
            exec(code, env)
    except Exception as e:
        _print_exception(e, out)
        _print_exception(e)
    finally:
        if saved is None:
            env.pop("print", None)
        else:
            env["print"] = saved
    return out.getvalue() or "(ok, no output)"


def _handle(conn):
    conn.settimeout(3)
    method, target, headers, first = _read_request(conn)
    path, args = _query(target)
    if path == "/" and method == "GET":
        return _reply(conn, "200 OK", _PAGE, "text/html; charset=utf-8")
    if args.get("key") != _key:
        return _reply(conn, "403 Forbidden", "Wrong or missing key: open the address webdrop.start() printed.\n")
    if path == "/run" and method == "POST":
        code = b"".join(_body_chunks(conn, headers, first)).decode()
        return _reply(conn, "200 OK", _run(code))
    if path == "/ls":
        files = sorted(os.listdir())
        return _reply(conn, "200 OK", json.dumps({"cwd": os.getcwd(), "files": files}), "application/json")
    name = _safe_name(args.get("name", ""))
    if path == "/upload" and method == "POST":
        if name is None:
            return _reply(conn, "400 Bad Request", "Give name=<file> (a name in the current folder, or /sd/...).\n")
        size = 0
        with open(name, "wb") as f:
            for chunk in _body_chunks(conn, headers, first):
                f.write(chunk)
                size += len(chunk)
        print("webdrop: saved %s (%d bytes)" % (name, size))
        return _reply(conn, "200 OK", "Saved %s (%d bytes)\n" % (name, size))
    if path == "/get" and method == "GET":
        if name is None:
            return _reply(conn, "400 Bad Request", "Give name=<file>.\n")
        try:
            with open(name, "rb") as f:
                data = f.read()
        except OSError:
            return _reply(conn, "404 Not Found", "No file %s\n" % name)
        return _reply(conn, "200 OK", data, "application/octet-stream")
    return _reply(conn, "404 Not Found", "Try /, /run, /upload, /ls or /get.\n")


def _poll(_arg=None):
    if _server is None:
        return
    try:
        while True:
            try:
                conn, _peer = _server.accept()
            except OSError:
                break  # nobody waiting
            try:
                _handle(conn)
            except Exception as e:
                print("webdrop: request failed:", e)
            finally:
                conn.close()
    finally:
        if _server is not None:
            tulip.defer(_poll, None, _POLL_MS)


def start(port=PORT):
    """Serve the webdrop page on port (8080). Prints the address to open."""
    global _server, _key, _globals, PORT
    if _server is not None:
        print("webdrop is already running:", url())
        return url()
    try:
        import __main__
        _globals = __main__.__dict__
    except ImportError:
        _globals = {"tulip": tulip}
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("0.0.0.0", port))
    s.listen(2)
    s.setblocking(False)
    PORT = port
    _key = _new_key()
    _server = s
    tulip.defer(_poll, None, _POLL_MS)
    print("webdrop: open", url())
    return url()


def _new_key():
    try:
        return "".join("%02x" % b for b in os.urandom(4))
    except (AttributeError, OSError):
        import random
        import time
        random.seed(time.ticks_us() if hasattr(time, "ticks_us") else int(time.time() * 1e6))
        return "%08x" % random.getrandbits(32)


def url():
    return "http://%s:%d/?key=%s" % (tulip.ip() or "<this-ip>", PORT, _key)


def stop():
    """Stop serving."""
    global _server
    if _server is not None:
        _server.close()
        _server = None
        print("webdrop stopped")
