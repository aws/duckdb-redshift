#!/usr/bin/env python3
"""Fixture tests for tls_test_servers.py's authentication probe, which tls-verification.sh runs
before it starts the servers (python3 -I tls_test_servers_test.py).

Each test feeds a reply through a socket pair, or a token to the classifier. The tokens are
placeholders shaped like an NTLM or Kerberos message's start, not real credentials.
"""

import hashlib
import importlib.util
import os
import socket
import struct
import sys
import tempfile
import threading
import unittest

sys.dont_write_bytecode = True
_SPEC = importlib.util.spec_from_file_location(
    "tls_test_servers", os.path.join(os.path.dirname(os.path.abspath(__file__)), "tls_test_servers.py")
)
servers = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(servers)

NTLM = servers.NTLMSSP_SIGNATURE + struct.pack("<I", 1) + bytes(28)
#: Kerberos 5's object identifier, 1.2.840.113554.1.2.2, then the AP-REQ token ID; Microsoft's
#: identifier for it is 1.2.840.48018.1.2.2.
KERBEROS = b"\x60\x1d" + bytes.fromhex("06092a864886f712010202" "0100") + bytes(16)
MICROSOFT_KERBEROS = b"\x60\x15" + bytes.fromhex("06092a864882f712010202" "0100") + bytes(8)
SPNEGO = bytes.fromhex("06062b0601050502")
#: SPNEGO's mechanism list names Kerberos with no Kerberos token after it.
MECHANISMS_ONLY = (
    b"\x60\x20" + SPNEGO + bytes.fromhex("06092a864886f712010202") + bytes.fromhex("06092a864882f712010202")
)


def message(kind, body):
    return kind + struct.pack("!i", 4 + len(body)) + body


def describe(data, close=True, timeout=2.0):
    """describe_reply's fields for `data` sent by a peer that then closes, or stays silent."""
    ours, theirs = socket.socketpair()
    ours.settimeout(timeout)

    def send():
        try:
            theirs.sendall(data)
            if close:
                theirs.shutdown(socket.SHUT_WR)
        except OSError:
            pass

    sender = threading.Thread(target=send)
    sender.start()
    try:
        return servers.describe_reply(ours)
    finally:
        ours.close()
        sender.join()
        theirs.close()


class TokenMechanismTest(unittest.TestCase):
    def test_bare_and_wrapped_ntlm(self):
        self.assertEqual(servers.token_mechanism(NTLM), "ntlm-negotiate")
        self.assertEqual(servers.token_mechanism(b"\x60\x40" + SPNEGO + NTLM), "ntlm-negotiate")

    def test_the_ntlm_message_type_is_named(self):
        authenticate = servers.NTLMSSP_SIGNATURE + struct.pack("<I", 3) + bytes(8)
        self.assertEqual(servers.token_mechanism(authenticate), "ntlm-authenticate")
        self.assertEqual(servers.token_mechanism(servers.NTLMSSP_SIGNATURE + struct.pack("<I", 9)), "ntlm-unknown")
        self.assertEqual(servers.token_mechanism(servers.NTLMSSP_SIGNATURE + b"\x01"), "ntlm-unknown")

    def test_bare_and_wrapped_kerberos(self):
        self.assertEqual(servers.token_mechanism(KERBEROS), "kerberos")
        self.assertEqual(servers.token_mechanism(b"\x60\x60" + SPNEGO + KERBEROS), "kerberos")
        self.assertEqual(servers.token_mechanism(MICROSOFT_KERBEROS), "kerberos")

    def test_kerberos_named_in_a_mechanism_list_alone_is_unknown(self):
        self.assertEqual(servers.token_mechanism(MECHANISMS_ONLY), "unknown")
        self.assertEqual(servers.token_mechanism(b"\x01\x02\x03"), "unknown")


class DescribeReplyTest(unittest.TestCase):
    def test_a_token_gives_its_mechanism_size_and_hash_not_its_bytes(self):
        fields = describe(message(b"p", NTLM))
        self.assertEqual(
            fields,
            "reply=p length=%d mechanism=ntlm-negotiate bytes=%d sha256=%s"
            % (4 + len(NTLM), len(NTLM), hashlib.sha256(NTLM).hexdigest()),
        )

    def test_no_reply_closed_reset_or_timed_out(self):
        self.assertEqual(describe(b""), "reply=none closed")
        self.assertEqual(describe(b"p\x00\x00"), "reply=none closed")
        self.assertEqual(describe(b"", close=False, timeout=0.3), "reply=none timeout")
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        peer = socket.create_connection(listener.getsockname())
        ours, _ = listener.accept()
        ours.settimeout(2)
        # A zero linger makes the close a reset; Winsock's linger is two shorts.
        linger = struct.pack("HH" if os.name == "nt" else "ii", 1, 0)
        peer.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, linger)
        peer.close()
        try:
            self.assertEqual(servers.describe_reply(ours), "reply=none reset")
        finally:
            ours.close()
            listener.close()

    def test_a_message_that_is_no_token_gives_its_type_and_length(self):
        self.assertEqual(describe(message(b"X", b"")), "reply=X length=4")
        self.assertEqual(describe(message(b"Q", b"abc")), "reply=Q length=7")
        self.assertEqual(describe(message(b"p", b"")), "reply=p length=4")
        self.assertEqual(describe(b"\x01" + struct.pack("!i", 4)), "reply=0x01 length=4")

    def test_a_kerberos_token_with_its_authorization_data_is_read_whole(self):
        token = KERBEROS + bytes(64 * 1024)
        self.assertEqual(
            describe(message(b"p", token)).split(" sha256=")[0],
            "reply=p length=%d mechanism=kerberos bytes=%d" % (4 + len(token), len(token)),
        )

    def test_a_token_longer_than_the_bound_is_not_read(self):
        too_long = 5 + servers.MAX_TOKEN_BYTES
        self.assertEqual(describe(b"p" + struct.pack("!i", too_long)), "reply=p length=%d" % too_long)
        at_bound = bytes(servers.MAX_TOKEN_BYTES)
        self.assertEqual(
            describe(message(b"p", at_bound)),
            "reply=p length=%d mechanism=unknown bytes=%d" % (4 + len(at_bound), len(at_bound)),
        )

    def test_an_unknown_token_such_as_a_password_gets_no_hash(self):
        self.assertEqual(describe(message(b"p", b"placeholder\x00")), "reply=p length=16 mechanism=unknown bytes=12")

    def test_a_cut_or_stalled_token(self):
        self.assertEqual(
            describe(b"p" + struct.pack("!i", 20) + NTLM[:8]).split(" sha256=")[0],
            "reply=p length=20 mechanism=ntlm-unknown bytes=8 truncated",
        )
        self.assertEqual(
            describe(b"p" + struct.pack("!i", 20) + b"abc", close=False, timeout=0.3), "reply=p length=20 body=unread"
        )


class AuthenticationProbeTest(unittest.TestCase):
    def test_the_probe_asks_records_and_refuses(self):
        directory = tempfile.mkdtemp()
        record = os.path.join(directory, "sspi.auth")
        ours, theirs = socket.socketpair()
        ours.settimeout(2)
        theirs.settimeout(2)
        body = struct.pack("!i", 196608) + b"user\x00tls-test\x00database\x00dev\x00\x00"
        startup = struct.pack("!i", 4 + len(body)) + body
        seen = {}

        def client():
            theirs.sendall(startup[12:])
            seen["request"] = servers.receive_exactly(theirs, 9)
            theirs.sendall(message(b"p", NTLM))
            seen["refusal"] = servers.receive_exactly(theirs, len(servers.REFUSAL))

        peer = threading.Thread(target=client)
        peer.start()
        try:
            servers.AuthenticationProbe("sspi", 12).run(ours, startup[:12], record)
        finally:
            peer.join()
            ours.close()
            theirs.close()
        with open(record, encoding="utf-8") as lines:
            written = lines.read()
        os.remove(record)
        os.rmdir(directory)
        self.assertEqual(seen["request"], b"R" + struct.pack("!ii", 8, 12))
        self.assertEqual(written, "request=sspi code=12 " + describe(message(b"p", NTLM)) + "\n")
        refusal = seen["refusal"]
        self.assertEqual(refusal[:1], b"E")
        self.assertEqual(struct.unpack("!i", refusal[1:5])[0], len(refusal) - 1)
        self.assertTrue(refusal.endswith(b"\x00\x00"))
        fields = {field[:1]: field[1:] for field in refusal[5:-1].split(b"\x00") if field}
        self.assertEqual(fields[b"S"], b"FATAL")
        self.assertEqual(fields[b"V"], b"FATAL")
        self.assertEqual(fields[b"C"], b"28000")
        self.assertTrue(fields[b"M"])

    def test_the_cases_ask_for_the_drivers_sspi_and_gss_codes(self):
        self.assertEqual(servers.AUTHENTICATION_CASES, (("sspi", 12), ("gss", 10)))


if __name__ == "__main__":
    unittest.main()
