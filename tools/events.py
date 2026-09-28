#!/usr/bin/env python3
# ABOUTME: Prints a TT7's live input events (touch, buttons) from tt7d's WebSocket GET /api/v1/events, one JSON per line.
# ABOUTME: Standard library only: a minimal RFC 6455 client (masked frames, ping/pong, close). Also imported by the e2e test.
"""usage: tools/events.py <host[:port]> [--count N] [--timeout S] [--query-token]

Connects to ws://<host>/api/v1/events and prints every event as one line of
JSON, starting with tt7d's "hello" message. Touch events carry x/y in the
panel's logical 1280x800 coordinates plus the frame_id on screen at the time.

  --count N       exit after N events (the hello does not count)
  --timeout S     exit with status 1 if no message arrives for S seconds
  --query-token   send the token as ?token= (what browsers must do) instead of
                  the Authorization header

The token comes from $TT7_TOKEN_FILE (default ~/.config/tt7/token). Fetch it
once from the panel:

  mkdir -p ~/.config/tt7
  ssh root@<panel> cat /data/tt7/tt7d/token > ~/.config/tt7/token
  chmod 600 ~/.config/tt7/token
"""
import argparse
import base64
import hashlib
import os
import socket
import struct
import sys
import urllib.parse

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
OP_TEXT, OP_CLOSE, OP_PING, OP_PONG = 1, 8, 9, 10


class HandshakeError(Exception):
    """tt7d answered the upgrade with an HTTP status other than 101."""

    def __init__(self, status, body):
        super().__init__(f"HTTP {status}: {body[:200]!r}")
        self.status = status
        self.body = body


class EventStream:
    """One WebSocket connection to tt7d's event stream."""

    def __init__(self, host, port, token, query_token=False, timeout=10.0, rcvbuf=None):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if rcvbuf:  # set before connect so the TCP window starts small (the slow-client test)
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        self.sock.settimeout(timeout)
        self.sock.connect((host, port))
        self.buf = b""
        key = base64.b64encode(os.urandom(16)).decode()
        path = "/api/v1/events"
        headers = [f"GET {path} HTTP/1.1", f"Host: {host}:{port}", "Upgrade: websocket", "Connection: Upgrade",
                   f"Sec-WebSocket-Key: {key}", "Sec-WebSocket-Version: 13"]
        if token is not None and query_token:
            headers[0] = f"GET {path}?token={urllib.parse.quote(token, safe='')} HTTP/1.1"
        elif token is not None:
            headers.append(f"Authorization: Bearer {token}")
        self.sock.sendall(("\r\n".join(headers) + "\r\n\r\n").encode())
        head = self._read_until(b"\r\n\r\n")
        status_line, *lines = head.decode("latin-1").split("\r\n")
        status = int(status_line.split()[1])
        if status != 101:
            self.sock.close()
            raise HandshakeError(status, self.buf)
        fields = {k.strip().lower(): v.strip() for k, v in (l.split(":", 1) for l in lines if ":" in l)}
        want = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        if fields.get("sec-websocket-accept") != want:
            raise ValueError(f"bad Sec-WebSocket-Accept {fields.get('sec-websocket-accept')!r}, want {want!r}")
        self.close_code = None

    def _read_until(self, marker):
        while marker not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("tt7d closed the connection during the handshake")
            self.buf += chunk
        head, self.buf = self.buf.split(marker, 1)
        return head

    def _read_exact(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("connection closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def send_frame(self, opcode, payload=b"", fin=True):
        """A client frame, masked as RFC 6455 requires."""
        mask = os.urandom(4)
        n = len(payload)
        head = bytes([(0x80 if fin else 0) | opcode])
        if n < 126:
            head += bytes([0x80 | n])
        elif n < 65536:
            head += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            head += bytes([0x80 | 127]) + struct.pack(">Q", n)
        masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self.sock.sendall(head + mask + masked)

    def read_frame(self):
        """(opcode, payload) of the next server frame, which must be unmasked and unfragmented."""
        b0, b1 = self._read_exact(2)
        if b1 & 0x80:
            raise ValueError("the server masked a frame")
        if not b0 & 0x80:
            raise ValueError("the server fragmented a message")
        n = b1 & 0x7F
        if n == 126:
            n = struct.unpack(">H", self._read_exact(2))[0]
        elif n == 127:
            n = struct.unpack(">Q", self._read_exact(8))[0]
        return b0 & 0x0F, self._read_exact(n)

    def next_message(self):
        """The next text message as a str, answering pings on the way; None once the server closed."""
        while True:
            op, payload = self.read_frame()
            if op == OP_TEXT:
                return payload.decode("utf-8")
            if op == OP_PING:
                self.send_frame(OP_PONG, payload)
            elif op == OP_CLOSE:
                self.close_code = struct.unpack(">H", payload[:2])[0] if len(payload) >= 2 else 1005
                return None

    def close(self, code=1000):
        try:
            self.send_frame(OP_CLOSE, struct.pack(">H", code))
        except OSError:
            pass
        self.sock.close()


def read_token():
    path = os.environ.get("TT7_TOKEN_FILE") or os.path.expanduser("~/.config/tt7/token")
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError as e:
        sys.exit(f"events: cannot read the token from {path}: {e.strerror} (see --help)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[1], usage=__doc__.split("\n", 1)[0][7:],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host", help="panel address, host or host:port")
    ap.add_argument("--count", type=int, default=0, help="exit after this many events (hello not counted)")
    ap.add_argument("--timeout", type=float, default=0, help="exit 1 after this many seconds without a message")
    ap.add_argument("--query-token", action="store_true", help="send the token as ?token= instead of a header")
    args = ap.parse_args()
    host, _, port = args.host.partition(":")
    token = read_token()
    try:
        ws = EventStream(host, int(port or 80), token, args.query_token, timeout=args.timeout or None)
    except HandshakeError as e:
        sys.exit(f"events: tt7d refused the stream: {e}")
    except OSError as e:
        sys.exit(f"events: cannot connect to {args.host}: {e}")
    seen = 0
    try:
        while True:
            msg = ws.next_message()
            if msg is None:
                sys.exit(f"events: tt7d closed the stream (code {ws.close_code})")
            print(msg, flush=True)
            if not msg.startswith('{"type":"hello"'):
                seen += 1
                if args.count and seen >= args.count:
                    break
    except socket.timeout:
        sys.exit(f"events: nothing for {args.timeout:g} s")
    except KeyboardInterrupt:
        pass
    finally:
        ws.close()


if __name__ == "__main__":
    main()
