import socket, time
HOST,PORT="127.0.0.1",7720
def send_raw(data):
    try:
        s=socket.create_connection((HOST,PORT),timeout=3); s.settimeout(3)
        s.sendall(data); r=s.recv(120); s.close()
        return r.split(b"\r\n")[0].decode(errors="replace")
    except Exception as e: return "ERR %s"%e
# 1) normal POST → 200
print("normal POST :", send_raw(b"POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"))
# 2) bare-LF smuggling → reddedilmeli (400/close)
print("bare-LF smug:", send_raw(b"POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nX-Fold: junk\nContent-Length: 44\r\nConnection: close\r\n\r\n"))
# 3) bare-CR → reddedilmeli
print("bare-CR     :", send_raw(b"POST /x HTTP/1.1\r\nHost: x\rX: y\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"))
