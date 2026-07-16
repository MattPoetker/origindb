"""Asserts WHERE-filtered websocket delivery (stdlib only, no pip deps).

Two sql_subscribe subscriptions on table t: WHERE a = 1 must receive the
event produced by the testmod "w" reducer; WHERE a = 99 must receive nothing.

Usage: ws_filter_check.py <instantdb_client_path> <ws_port> <grpc_port>
"""
import base64
import json
import os
import socket
import struct
import subprocess
import sys
import threading
import time


def ws_connect(host, port):
    s = socket.create_connection((host, port), timeout=10)
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        f"GET / HTTP/1.1\r\nHost: {host}:{port}\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
    )
    s.sendall(req.encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        resp += s.recv(4096)
    assert b"101" in resp.split(b"\r\n")[0], resp
    return s


def ws_send(s, text):
    payload = text.encode()
    mask = os.urandom(4)
    header = bytes([0x81])
    n = len(payload)
    if n < 126:
        header += bytes([0x80 | n])
    elif n < 65536:
        header += bytes([0x80 | 126]) + struct.pack(">H", n)
    else:
        header += bytes([0x80 | 127]) + struct.pack(">Q", n)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    s.sendall(header + mask + masked)


def ws_recv(s):
    def read_exact(n):
        buf = b""
        while len(buf) < n:
            chunk = s.recv(n - len(buf))
            if not chunk:
                raise EOFError
            buf += chunk
        return buf

    hdr = read_exact(2)
    opcode = hdr[0] & 0x0F
    length = hdr[1] & 0x7F
    if length == 126:
        length = struct.unpack(">H", read_exact(2))[0]
    elif length == 127:
        length = struct.unpack(">Q", read_exact(8))[0]
    payload = read_exact(length) if length else b""
    if opcode == 8:
        raise EOFError("close frame")
    return payload.decode(errors="replace")


def subscriber(where, events, ready, ws_port):
    s = ws_connect("localhost", ws_port)
    ws_recv(s)  # welcome
    ws_send(s, json.dumps({
        "type": "sql_subscribe",
        "sql": f"SELECT * FROM t WHERE {where}",
    }))
    deadline = time.time() + 15
    s.settimeout(3)
    ready.set()
    while time.time() < deadline:
        try:
            msg = json.loads(ws_recv(s))
        except (socket.timeout, EOFError):
            continue
        if msg.get("type") == "sql_changefeed_event":
            events.append(msg)


def main():
    client, ws_port, grpc_port = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    got_match, got_nomatch = [], []
    r1, r2 = threading.Event(), threading.Event()
    threading.Thread(target=subscriber, args=("a = 1", got_match, r1, ws_port),
                     daemon=True).start()
    threading.Thread(target=subscriber, args=("a = 99", got_nomatch, r2, ws_port),
                     daemon=True).start()
    r1.wait(10)
    r2.wait(10)
    time.sleep(1)

    subprocess.run([client, "-s", f"localhost:{grpc_port}", "call", "testmod", "w"],
                   check=True, capture_output=True)
    time.sleep(4)

    print(f"WHERE a=1  received {len(got_match)} event(s); "
          f"WHERE a=99 received {len(got_nomatch)}")
    ok = len(got_match) == 1 and len(got_nomatch) == 0
    print("ws filter check:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
