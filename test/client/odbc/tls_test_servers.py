#!/usr/bin/env python3
"""The servers tls_verification_test.cpp connects the real driver to, started by tls-verification.sh.

Usage: python3 -I tls_test_servers.py <directory>

<directory> holds the run's test PKI: control.pem and control.key for the control plane, and
<case>.pem and <case>.key for each database case; sspi and gss serve the trusted case's certificate,
ask for an authentication method and write the reply to <case>.auth (AuthenticationProbe). Each case
listens on a loopback TCP port, answers the PostgreSQL SSLRequest with 'S' and then serves its
certificate with Python's TLS, so the same servers run on Windows. The control plane answers every
request with fixed Redshift
Serverless GetCredentials output, so the driver goes on to the database hop, once over HTTPS and
once over plain HTTP: Windows' driver verifies its control plane against the Windows store, which
does not trust this run's CA. A closed port, bound and never listened on, refuses every
connection. Every listener binds 127.0.0.1 alone. The ports go to <directory>/ports, written once all
are ready; each case appends a line to <case>.connections per connection, and to <case>.startups
when a startup message has come through TLS. Runs until killed, until <directory>/stop exists, or
LIFETIME_SECONDS at most. The stop file is for Windows, where this runs in a native Python, outside
Git Bash's POSIX signals; elsewhere SIGTERM, sent with it, ends it first.
"""

import hashlib
import http.server
import json
import os
import socket
import socketserver
import ssl
import struct
import sys
import threading
import time

CASES = ("untrusted", "wrong_host", "trusted")
LOOPBACK = "127.0.0.1"
LIFETIME_SECONDS = 600
#: How long a server waits for a peer before it gives up on the connection; tls_verification_test.cpp
#: waits longer than this for an authentication case's record.
PEER_TIMEOUT_SECONDS = 20
#: The PostgreSQL SSLRequest: its length, 8, then the code 80877103.
SSL_REQUEST = struct.pack("!ii", 8, 80877103)
#: A key every startup message carries.
STARTUP_MARKER = b"user\x00"
#: The cases that ask for an authentication method in place of the password exchange, each with
#: its AuthenticationRequest code in the driver's libpq: pqcomm.h gives SSPI and GSS 12 and 10, and
#: fe-auth.c refuses PostgreSQL's 9 and 7 as unsupported methods.
AUTHENTICATION_CASES = (("sspi", 12), ("gss", 10))
#: The longest token read; a Kerberos ticket with its authorization data stays well under it.
MAX_TOKEN_BYTES = 1 << 20
#: What starts an NTLM message, bare or inside SPNEGO, and the message types that follow it: only
#: an authenticate message carries the user's response.
NTLMSSP_SIGNATURE = b"NTLMSSP\x00"
NTLM_MESSAGES = {1: "negotiate", 2: "challenge", 3: "authenticate"}
#: Kerberos 5's object identifier (RFC 1964's, and Microsoft's) followed by the AP-REQ token ID: the
#: start of a Kerberos initial token, bare or inside SPNEGO.
KERBEROS_AP_REQ_MARKERS = (
    bytes.fromhex("06092a864886f712010202" "0100"),
    bytes.fromhex("06092a864882f712010202" "0100"),
)
#: The ErrorResponse that ends every probe, SQLSTATE 28000, so no session opens.
REFUSAL_FIELDS = b"SFATAL\x00VFATAL\x00C28000\x00Mthe test server accepts no authentication\x00\x00"
REFUSAL = b"E" + struct.pack("!i", 4 + len(REFUSAL_FIELDS)) + REFUSAL_FIELDS


def receive_exactly(peer, size):
    data = b""
    while len(data) < size:
        chunk = peer.recv(size - len(data))
        if not chunk:
            break
        data += chunk
    return data


def append_line(path, line):
    with open(path, "a", encoding="utf-8") as out:
        out.write(line + "\n")


def token_mechanism(token):
    """Which mechanism a token starts, by the start of its NTLM or Kerberos message, and for NTLM
    which message it is."""
    start = token.find(NTLMSSP_SIGNATURE)
    if start >= 0:
        number = token[start + len(NTLMSSP_SIGNATURE) : start + len(NTLMSSP_SIGNATURE) + 4]
        return "ntlm-" + NTLM_MESSAGES.get(struct.unpack("<I", number)[0] if len(number) == 4 else 0, "unknown")
    if any(marker in token for marker in KERBEROS_AP_REQ_MARKERS):
        return "kerberos"
    return "unknown"


def describe_reply(peer):
    """The client's reply to an authentication request, as the record's fields. A token's bytes stay
    out, as a Kerberos token carries the user's ticket, and no field is named token, which the
    output scan reads as a credential."""
    try:
        header = receive_exactly(peer, 5)
    except socket.timeout:
        return "reply=none timeout"
    except OSError:
        return "reply=none reset"
    if len(header) < 5:
        return "reply=none closed"
    kind, length = struct.unpack("!ci", header)
    fields = "reply=%s length=%d" % (kind.decode("ascii") if kind.isalpha() else "0x" + kind.hex(), length)
    if kind != b"p" or not 4 < length <= 4 + MAX_TOKEN_BYTES:
        return fields
    try:
        token = receive_exactly(peer, length - 4)
    except OSError:
        return fields + " body=unread"
    mechanism = token_mechanism(token)
    fields += " mechanism=%s bytes=%d" % (mechanism, len(token))
    if len(token) < length - 4:
        fields += " truncated"
    # An unknown token may be a password, whose fast hash could be guessed back; NTLM and Kerberos
    # tokens are hashed: an NTLM message's hash can match across runs, a Kerberos AP-REQ's never
    # does, as each carries a fresh authenticator.
    if mechanism != "unknown":
        fields += " sha256=" + hashlib.sha256(token).hexdigest()
    return fields


class AuthenticationProbe:
    """Asks for one authentication method after the startup message, appends a line with the
    client's reply to <case>.auth, then refuses."""

    def __init__(self, name, code):
        self.request = "request=%s code=%d" % (name, code)
        self.code = code

    def run(self, tls, received, path):
        startup_length = struct.unpack("!i", received[:4])[0]
        receive_exactly(tls, startup_length - len(received))
        tls.sendall(struct.pack("!cii", b"R", 8, self.code))
        append_line(path, self.request + " " + describe_reply(tls))
        tls.sendall(REFUSAL)


class ControlPlane(http.server.BaseHTTPRequestHandler):
    """GetCredentials output for every POST; the request itself is not checked."""

    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", "0")))
        expiration = int(time.time()) + 900
        body = json.dumps(
            {
                "dbUser": "IAMR:tls-test",
                "dbPassword": "tls-test-password",
                "expiration": expiration,
                "nextRefreshTime": expiration,
            }
        ).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/x-amz-json-1.1")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


class ControlPlaneServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    """ThreadingHTTPServer's parts, which Python 3.6 does not have under that name."""

    daemon_threads = True

    def handle_error(self, request, client_address):
        # A client that refuses the certificate drops the connection in the handshake, as a case
        # expects; anything else is reported.
        if not isinstance(sys.exc_info()[1], OSError):
            super().handle_error(request, client_address)


def start_control_plane(directory, tls):
    server = ControlPlaneServer((LOOPBACK, 0), ControlPlane)
    if tls:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(os.path.join(directory, "control.pem"), os.path.join(directory, "control.key"))
        # The handshake runs in each request's thread, so a stalled client cannot block the accept loop.
        server.socket = context.wrap_socket(server.socket, server_side=True, do_handshake_on_connect=False)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


class DatabaseCase:
    """One case's certificate, served with Python's TLS behind the SSLRequest listener, and its
    AuthenticationProbe, if it has one, after the startup message."""

    def __init__(self, directory, name, certificate=None, probe=None):
        self.directory = directory
        self.name = name
        self.probe = probe
        stem = os.path.join(directory, certificate or name)
        self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.context.load_cert_chain(stem + ".pem", stem + ".key")
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.bind((LOOPBACK, 0))
        self.listener.listen(4)

    def port(self):
        return self.listener.getsockname()[1]

    def serve(self):
        while True:
            client, _ = self.listener.accept()
            append_line(os.path.join(self.directory, self.name + ".connections"), "connection")
            with client:
                try:
                    self.answer(client)
                except OSError:
                    # A client that refuses the certificate, or gives up, drops the connection; a
                    # retried connect, which the cases count, still needs this listener.
                    pass

    def answer(self, client):
        client.settimeout(PEER_TIMEOUT_SECONDS)
        if receive_exactly(client, len(SSL_REQUEST)) != SSL_REQUEST:
            return
        client.sendall(b"S")
        # Closed without TLS's closing alert once the startup message arrives, which shows that the
        # client accepted the certificate; that ends the connect at once, or after the probe.
        with self.context.wrap_socket(client, server_side=True) as tls:
            received = b""
            while STARTUP_MARKER not in received:
                chunk = tls.recv(4096)
                if not chunk:
                    return
                received += chunk
            append_line(os.path.join(self.directory, self.name + ".startups"), "startup")
            if self.probe:
                self.probe.run(tls, received, os.path.join(self.directory, self.name + ".auth"))


def write_ports(directory, control, control_http, closed, cases):
    lines = ["control %d" % control.server_address[1], "control_http %d" % control_http.server_address[1]]
    lines.append("closed %d" % closed.getsockname()[1])
    lines += ["%s %d" % (case.name, case.port()) for case in cases]
    staged = os.path.join(directory, "ports.staged")
    with open(staged, "w", encoding="utf-8") as out:
        out.write("\n".join(lines) + "\n")
    # Renamed into place, so a reader never sees a partial file.
    os.replace(staged, os.path.join(directory, "ports"))


def main(directory):
    cases = []
    directory = os.path.abspath(directory)
    control = start_control_plane(directory, tls=True)
    control_http = start_control_plane(directory, tls=False)
    # Held open, so no other program takes the port while the run lasts.
    closed = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    closed.bind((LOOPBACK, 0))
    for name in CASES:
        cases.append(DatabaseCase(directory, name))
        threading.Thread(target=cases[-1].serve, daemon=True).start()
    for name, code in AUTHENTICATION_CASES:
        cases.append(DatabaseCase(directory, name, certificate="trusted", probe=AuthenticationProbe(name, code)))
        threading.Thread(target=cases[-1].serve, daemon=True).start()
    write_ports(directory, control, control_http, closed, cases)
    lifetime = time.monotonic() + LIFETIME_SECONDS
    stop_file = os.path.join(directory, "stop")
    while not os.path.exists(stop_file) and time.monotonic() < lifetime:
        time.sleep(0.2)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
