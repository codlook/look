# Redis TLS guard (redis:7 + TLS gerektirir)

RESP client TLS (rediss:// / ?tls=1 / ?tls=verify) — MySQL DB-TLS ile AYNI kalıp
(SSL_CTX TLS_client_method, SSL_connect, VERIFY_NONE/PEER+set1_host, SNI, fail→throw).
Redis TLS bağlantı BAŞINDA (MySQL'in mid-stream SSLRequest'i yok — daha basit).

## Neden: managed Redis TLS zorunlu tutar
Upstash / AWS ElastiCache (in-transit) / Redis Cloud → rediss:// ŞART. Eski kod stub-throw
("requires OpenSSL — not compiled") → LOOK hiçbir managed Redis'e bağlanamıyordu. Ayrıca
rediss:// tespiti BOZUKtu (substr(0,8)=="rediss://" 9-karakteri asla eşleşmez) — düzeltildi.

## Kanıt (Docker redis:7 + self-signed TLS, ampirik)
    rediss://host?tls=1      → set/get [hello-tls]         (TLS şifreleme çalışır)
    rediss://host?tls=verify → certificate verify failed   (self-signed reddedilir = verify aktif)
    redis://host (TLS-porta)  → graceful hata               (crash yok)

## Koşum
    # cert üret (CN=look-redis-tls, SAN)
    openssl req -x509 -newkey rsa:2048 -days 3 -nodes -keyout server.key -out server.crt \
      -subj "/CN=look-redis-tls" -addext "subjectAltName=DNS:look-redis-tls"
    docker network create looknet
    docker run -d --name look-redis-tls --network looknet -v <certs>:/certs:ro redis:7 \
      redis-server --tls-port 6379 --port 0 --tls-cert-file /certs/server.crt \
      --tls-key-file /certs/server.key --tls-ca-cert-file /certs/server.crt --tls-auth-clients no
    # harness derle + koş (resp_client.cpp.o'ya link)
    g++ -std=c++23 -I include tests/redis_tls/harness.cpp \
      build/CMakeFiles/look.dir/src/resp_client.cpp.o -lssl -lcrypto -o build/resp_tls_harness
    docker run --rm --network looknet ... build/resp_tls_harness 'rediss://look-redis-tls:6379?tls=1'
