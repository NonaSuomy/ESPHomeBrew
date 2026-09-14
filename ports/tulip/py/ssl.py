# ssl for Tulip on the ESP32-P4 PAPP loader: MicroPython's ssl API over tls.py
# (TLS made and verified by the loader; see tls.py).
from tls import (CERT_NONE, CERT_OPTIONAL, CERT_REQUIRED, MBEDTLS_VERSION, PROTOCOL_TLS_CLIENT,
                 PROTOCOL_TLS_SERVER, SSLContext, SSLSocket)


def create_default_context(purpose=None, cafile=None, capath=None, cadata=None):
    context = SSLContext(PROTOCOL_TLS_CLIENT)
    if cafile or capath or cadata:
        context.load_verify_locations(cafile, cadata)
    return context


def wrap_socket(sock, server_side=False, key=None, cert=None, cert_reqs=CERT_NONE, cadata=None,
                server_hostname=None, do_handshake=True):
    context = SSLContext(PROTOCOL_TLS_SERVER if server_side else PROTOCOL_TLS_CLIENT)
    if key is not None or cert is not None:
        context.load_cert_chain(cert, key)
    if cadata is not None:
        context.load_verify_locations(cadata=cadata)
    return context.wrap_socket(sock, server_hostname=server_hostname, do_handshake=do_handshake)
