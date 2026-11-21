"""download.sh's rules in Python, for a Windows build host with no bash and no curl of a known
version. fetch-driver.sh's Windows branch downloads with lib/download.sh, so only the tests run
this one.

    python3 -I download.py <url> <out-file> <max-bytes>

Writes the URL's body, exactly as the server sent it, to <out-file>, which must not exist. Only an
https:// URL is fetched, as written, with the system trust store, through no proxy and following
no redirect: anything but a 200 answer fails. The body is bounded in size (a declared length over
the bound fails before the body is read, a body that passes it is stopped, and a body that ends
short of its declared length fails) and in time (30 seconds for the connection and for each read,
and 600 seconds overall: each wait for a part of the body is given at most the time left, so a
server that sends a byte at a time cannot hold the body past the deadline; the answer's headers,
and a chunked body's chunk-size and trailer lines, are bounded only by the read timeout). The
declared length is http.client's reading of Content-Length, the one its reads stop at. The
variables that would add a trusted certificate or load other OpenSSL configuration (common.sh's
no_trust_variables) are removed before ssl is imported. On failure it removes what it wrote of
<out-file>, prints one line to stderr and exits 1. Standard library only, Python 3.6 and later.
"""

import os

NO_TRUST_VARIABLES = ("SSL_CERT_FILE", "SSL_CERT_DIR", "SSL_CERT_URI", "OPENSSL_CONF",
                      "OPENSSL_CONF_INCLUDE", "OPENSSL_MODULES", "OPENSSL_ENGINES")
for _name in NO_TRUST_VARIABLES:
    os.environ.pop(_name, None)

import http.client  # noqa: E402 (after the variables are gone, so OpenSSL never reads them)
import ssl  # noqa: E402
import sys  # noqa: E402
import time  # noqa: E402
import urllib.parse  # noqa: E402

TIMEOUT, DEADLINE, CHUNK = 30, 600, 1 << 16


class Failure(Exception):
    pass


def connect(host, port, timeout):
    for name in NO_TRUST_VARIABLES:
        os.environ.pop(name, None)
    return http.client.HTTPSConnection(host, port, timeout=timeout, context=ssl.create_default_context())


def limit_wait(sock, start, clock, url):
    """Gives the next wait on `sock` at most the time the deadline leaves, or fails."""
    left = DEADLINE - (clock() - start)
    if left <= 0:
        raise Failure("downloading %s failed: it took longer than %d seconds" % (url, DEADLINE))
    sock.settimeout(min(TIMEOUT, left))


def download(url, out, max_bytes, opener=connect, clock=time.monotonic):
    parts = urllib.parse.urlsplit(url)
    if parts.scheme != "https" or not parts.hostname or parts.username or parts.password or parts.fragment:
        # Named without its user part, so a password in a well-formed URL never reaches the message.
        shown = urllib.parse.urlunsplit(parts._replace(netloc=parts.netloc.rpartition("@")[2]))
        raise Failure("downloading %s failed: only an https:// URL with no user or fragment is downloaded" % shown)
    try:
        port = parts.port  # parsed only here, and raises ValueError for one that is not a port
    except ValueError:
        raise Failure("downloading %s failed: its port is not a number from 0 to 65535" % url)
    start, created = clock(), False
    connection = opener(parts.hostname, port, TIMEOUT)
    try:
        connection.request("GET", url[len("https://") + len(parts.netloc):] or "/",
                           headers={"Accept-Encoding": "identity"})
        # Kept here: the connection drops its socket once an answer says it will close, while the
        # response still reads from it.
        sock = connection.sock
        limit_wait(sock, start, clock, url)
        response = connection.getresponse()
        if response.status != 200:
            raise Failure("%s answered HTTP %d; redirects are not followed" % (url, response.status))
        # None when http.client found no length it can use: the body is then read to the
        # connection's close, and the size bound still stops it.
        declared = response.length
        if declared is not None and declared > max_bytes:
            raise Failure("downloading %s failed: its declared %d bytes are over the %d-byte bound"
                          % (url, declared, max_bytes))
        with open(out, "xb") as f:
            created, size = True, 0
            while True:
                # http.client closes a response once its declared length is read, and with it the
                # socket of an answer that closes the connection: no wait is set on that socket.
                if response.isclosed():
                    chunk = b""
                else:
                    limit_wait(sock, start, clock, url)
                    # read1 waits on the socket at most once; read would wait for all of CHUNK.
                    chunk = response.read1(CHUNK)
                if not chunk:
                    # read1 answers b"" when the connection closes early, as at the body's end.
                    if declared is not None and size != declared:
                        raise Failure("downloading %s failed: the body ended after %d of its "
                                      "declared %d bytes" % (url, size, declared))
                    return size
                size += len(chunk)
                if size > max_bytes:
                    raise Failure("downloading %s failed: the body passed the %d-byte bound and was stopped"
                                  % (url, max_bytes))
                f.write(chunk)
    except (OSError, http.client.HTTPException) as e:
        failure = Failure("downloading %s failed: %s" % (url, e))
    except Failure as e:
        failure = e
    finally:
        connection.close()
    # Only a failure reaches here: a partial body is never left behind, and a file that was
    # there before is never removed.
    if created:
        os.unlink(out)
    raise failure


def main(argv):
    if len(argv) != 4 or not argv[3].isdigit():
        sys.stderr.write("usage: download.py <url> <out-file> <max-bytes>\n")
        return 2
    try:
        download(argv[1], argv[2], int(argv[3]))
    except Failure as e:
        sys.stderr.write("%s\n" % e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
