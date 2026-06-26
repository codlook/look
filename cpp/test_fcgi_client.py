#!/usr/bin/env python3
"""Minimal FastCGI client to test look-fcgi on port 9000"""
import socket
import struct
import sys

FCGI_VERSION = 1
FCGI_BEGIN_REQUEST = 1
FCGI_PARAMS = 4
FCGI_STDIN = 5
FCGI_STDOUT = 6
FCGI_END_REQUEST = 3
FCGI_RESPONDER = 1

def make_record(rtype, req_id, data=b""):
    length = len(data)
    padding = (8 - (length % 8)) % 8
    header = struct.pack(">BBHHBB", FCGI_VERSION, rtype, req_id, length, padding, 0)
    return header + data + b"\x00" * padding

def encode_params(params):
    out = b""
    for k, v in params.items():
        k = k.encode()
        v = v.encode()
        def encode_len(n):
            if n < 128: return bytes([n])
            return struct.pack(">I", n | 0x80000000)
        out += encode_len(len(k)) + encode_len(len(v)) + k + v
    return out

def send_request(host, port, path="/", method="GET"):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect((host, port))

    req_id = 1
    # BEGIN_REQUEST
    body = struct.pack(">HB5x", FCGI_RESPONDER, 0)
    s.sendall(make_record(FCGI_BEGIN_REQUEST, req_id, body))

    # PARAMS
    params = {
        "REQUEST_METHOD": method,
        "REQUEST_URI": path,
        "SCRIPT_FILENAME": "/src/test_linux_fcgi.lk",
        "SCRIPT_NAME": "/index.lk",
        "SERVER_NAME": "localhost",
        "SERVER_PORT": "9000",
        "GATEWAY_INTERFACE": "CGI/1.1",
        "SERVER_PROTOCOL": "HTTP/1.1",
        "QUERY_STRING": "",
        "REMOTE_ADDR": "127.0.0.1",
    }
    param_data = encode_params(params)
    s.sendall(make_record(FCGI_PARAMS, req_id, param_data))
    s.sendall(make_record(FCGI_PARAMS, req_id))  # empty = end

    # STDIN (empty for GET)
    s.sendall(make_record(FCGI_STDIN, req_id))

    # Read response
    response = b""
    while True:
        try:
            hdr = s.recv(8)
            if len(hdr) < 8:
                break
            ver, rtype, rid, clen, plen, _ = struct.unpack(">BBHHBB", hdr)
            data = b""
            while len(data) < clen:
                chunk = s.recv(clen - len(data))
                if not chunk:
                    break
                data += chunk
            s.recv(plen)  # padding
            if rtype == FCGI_STDOUT and data:
                response += data
            elif rtype == FCGI_END_REQUEST:
                break
        except socket.timeout:
            break

    s.close()
    return response.decode(errors="replace")

if __name__ == "__main__":
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 9000
    path = sys.argv[3] if len(sys.argv) > 3 else "/"

    try:
        result = send_request(host, port, path)
        print(result)
        if "FastCGI Linux OK" in result or "ok" in result:
            print("=== FCGI TEST PASSED ===")
            sys.exit(0)
        else:
            print("=== FCGI TEST FAILED (unexpected response) ===")
            sys.exit(1)
    except Exception as e:
        print(f"ERROR: {e}")
        sys.exit(1)
