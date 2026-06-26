#!/bin/bash
echo '=== Phase 9.5: FastCGI+MySQL on Linux ==='

# Start look-fcgi
/src/build-linux/look-fcgi --port 9000 &
FCGI_PID=$!
sleep 3
echo "look-fcgi started (PID=$FCGI_PID)"

# Test helper - pass script file as 4th arg
run_test() {
    local path=$1
    local script=$2
    python3 /src/test_fcgi_client.py 127.0.0.1 9000 "$path" "$script"
}

echo "--- GET / (DB query) ---"
python3 - <<'PYEOF'
import socket, struct, sys

FCGI_VERSION=1; FCGI_BEGIN_REQUEST=1; FCGI_PARAMS=4; FCGI_STDIN=5; FCGI_STDOUT=6; FCGI_END_REQUEST=3; FCGI_RESPONDER=1

def make_record(rtype, req_id, data=b""):
    length=len(data); padding=(8-(length%8))%8
    return struct.pack(">BBHHBB",FCGI_VERSION,rtype,req_id,length,padding,0)+data+b"\x00"*padding

def enc_params(p):
    out=b""
    for k,v in p.items():
        k,v=k.encode(),v.encode()
        def el(n): return bytes([n]) if n<128 else struct.pack(">I",n|0x80000000)
        out+=el(len(k))+el(len(v))+k+v
    return out

def req(path, script):
    s=socket.socket(); s.settimeout(5); s.connect(("127.0.0.1",9000))
    s.sendall(make_record(FCGI_BEGIN_REQUEST,1,struct.pack(">HB5x",FCGI_RESPONDER,0)))
    params={"REQUEST_METHOD":"GET","REQUEST_URI":path,"SCRIPT_FILENAME":script,
            "SCRIPT_NAME":"/index.lk","SERVER_NAME":"localhost","SERVER_PORT":"9000",
            "GATEWAY_INTERFACE":"CGI/1.1","SERVER_PROTOCOL":"HTTP/1.1",
            "QUERY_STRING":"","REMOTE_ADDR":"127.0.0.1"}
    s.sendall(make_record(FCGI_PARAMS,1,enc_params(params)))
    s.sendall(make_record(FCGI_PARAMS,1))
    s.sendall(make_record(FCGI_STDIN,1))
    resp=b""
    while True:
        try:
            h=s.recv(8)
            if len(h)<8: break
            ver,rt,rid,cl,pl,_=struct.unpack(">BBHHBB",h)
            d=b""
            while len(d)<cl:
                c=s.recv(cl-len(d))
                if not c: break
                d+=c
            s.recv(pl)
            if rt==FCGI_STDOUT and d: resp+=d
            elif rt==FCGI_END_REQUEST: break
        except: break
    s.close()
    return resp.decode(errors="replace")

script="/src/test_linux_fcgi_mysql.lk"
for path in ["/","/" ,"/insert","/select"]:
    print(f"--- {path} ---")
    print(req(path,script))
PYEOF

kill $FCGI_PID 2>/dev/null
echo "=== DONE ==="
