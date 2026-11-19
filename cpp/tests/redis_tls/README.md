# Redis TLS guard (requires redis:7 + TLS)

RESP client TLS (rediss:// / ?tls=1 / ?tls=verify) — the SAME pattern as MySQL DB-TLS
(SSL_CTX TLS_client_method, SSL_connect, VERIFY_NONE/PEER+set1_host, SNI, fail -> throw).
Redis TLS starts at the BEGINNING of the connection (no mid-stream SSLRequest as in MySQL, so it is simpler).

## Why: managed Redis makes TLS mandatory
Upstash / AWS ElastiCache (in-transit) / Redis Cloud -> rediss:// is REQUIRED. The old code was a stub-throw
("requires OpenSSL — not compiled"), so LOOK could not connect to any managed Redis. In addition,
rediss:// detection was BROKEN (substr(0,8)=="rediss://" can never match a 9-character string); this was fixed.

## Evidence (Docker redis:7 + self-signed TLS, empirical)
    rediss://host?tls=1      → set/get [hello-tls]         (TLS encryption works)
    rediss://host?tls=verify → certificate verify failed   (self-signed is rejected = verify is active)
    redis://host (to TLS port)  → graceful error           (no crash)

## Run
    # generate a cert (CN=look-redis-tls, SAN)
    openssl req -x509 -newkey rsa:2048 -days 3 -nodes -keyout server.key -out server.crt \
      -subj "/CN=look-redis-tls" -addext "subjectAltName=DNS:look-redis-tls"
    docker network create looknet
    docker run -d --name look-redis-tls --network looknet -v <certs>:/certs:ro redis:7 \
      redis-server --tls-port 6379 --port 0 --tls-cert-file /certs/server.crt \
      --tls-key-file /certs/server.key --tls-ca-cert-file /certs/server.crt --tls-auth-clients no
    # build the harness + run (link against resp_client.cpp.o)
    g++ -std=c++23 -I include tests/redis_tls/harness.cpp \
      build/CMakeFiles/look.dir/src/resp_client.cpp.o -lssl -lcrypto -o build/resp_tls_harness
    docker run --rm --network looknet ... build/resp_tls_harness 'rediss://look-redis-tls:6379?tls=1'
