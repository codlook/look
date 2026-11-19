# DB-TLS guard (requires mysql:8 / postgres:16)

## PostgreSQL TLS (pg_*.lk) — 4-case matrix + positive control

TLS was added to postgres_client: `postgresqls://` / `?tls=verify` -> SSLRequest -> 'S' -> SSL_connect
-> (verify) SSL_VERIFY_PEER + SSL_set1_host. **verify is the CORRECT DEFAULT** (PG has no legacy TLS
users -> it starts secure; the opposite of mysql/redis). If the server does not offer TLS ('N') it does NOT
silently fall back to plaintext; it fails with a clean error. `?tls=insecure` -> encrypted but unverified
(self-signed dev).

Empirical matrix (postgres:16, CA-signed cert SAN=look-pg, look-build openssl):
- `pg_reject_tls.lk`  — ssl=off server -> 'N' -> clean error (does not fall back to plaintext). ✓
- `pg_untrusted.lk`   — untrusted CA + verify (default) -> certificate verify failed -> reject. ✓
- `pg_valid.lk`       — SSL_CERT_FILE=ca.crt + hostname matches -> connection + SELECT 1. ✓
- `pg_mismatch.lk`    — valid CA but connecting by IP (SAN mismatch) -> SSL_set1_host rejects. ✓
- `pg_insecure.lk`    — POSITIVE CONTROL: ?tls=insecure -> connects (42) DESPITE the untrusted cert;
                        proves that the rejections in the other 2 of the 4 cases come from verify, not
                        from a broken handshake. ✓

Run (network looknet + postgres:16 ssl=on, cert SAN=look-pg):
    build/lk tests/db_tls/pg_reject_tls.lk        # OK rejected (server without TLS): ...
    build/lk tests/db_tls/pg_untrusted.lk         # OK rejected (untrusted cert): ...
    SSL_CERT_FILE=/certs/ca.crt build/lk tests/db_tls/pg_valid.lk    # 1
    SSL_CERT_FILE=/certs/ca.crt PG_MISMATCH_DSN=postgresqls://.../@<IP>/testdb \
        build/lk tests/db_tls/pg_mismatch.lk      # OK rejected (hostname mismatch): ...

On failure these scripts print `ERROR: ...` (for example when an untrusted cert or a hostname mismatch
is ACCEPTED, or when a TLS-less server is connected to).

---

# DB-TLS guard: MySQL (requires mysql:8)

MySQL DB-TLS implementation (mysqls:// / ?tls=1) — SSLRequest -> SSL_connect -> auth + data over TLS.
It was thought to exist for Redis (a rediss stub, which actually did not); real TLS was added for MySQL
(the asymmetry is closed).

## Level of evidence
- Code: the http_client SSL pattern was mirrored (SSL_CTX TLS_client_method, SSL_connect, SSL_read/write);
  MySQL-specific SSLRequest ORDER: after the initial handshake, BEFORE auth (credentials go after TLS).
  caching_sha2 full-auth sends the password in CLEARTEXT inside TLS (not RSA, since the channel is already encrypted).
- Empirical (developer, Docker mysql:8): `Ssl_cipher=TLS_AES_256_GCM_SHA384` (real TLS 1.3 encryption),
  full-auth (first connection) + fast-auth (cached 2nd connection), TLS and non-TLS all worked, SELECT parity.
  Non-TLS regression: Ssl_cipher empty, connection not broken.
- SSL_VERIFY_NONE (--ssl-mode=REQUIRED): wire encryption (works with self-signed DB certs);
  certificate verification is a separate mode (see below).

## Run (mysql:8 + docker network looknet)
    docker network create looknet
    docker run -d --name look-mysql8 --network looknet -e MYSQL_ROOT_PASSWORD=rootpw \
      -e MYSQL_DATABASE=testdb -e MYSQL_USER=looktest -e MYSQL_PASSWORD=testpass mysql:8
    # when it is ready:
    docker run --rm --network looknet -v <repo>/cpp:/look/cpp -w /look/cpp look-build \
      bash -lc "/look/cpp/build/lk tests/db_tls/tls_on.lk"   # -> TLS_AES_256_GCM_SHA384

## Two modes (?tls=1 vs ?tls=verify)
- `?tls=1` (or mysqls://): encryption, SSL_VERIFY_NONE. It protects against passive eavesdropping but
  NOT against an ACTIVE MITM (the server cert is not verified). Works with self-signed DB certs
  (--ssl-mode=REQUIRED). Evidence: mysql:8 -> Ssl_cipher=TLS_AES_256_GCM_SHA384.
- `?tls=verify`: encryption + SSL_VERIFY_PEER + SSL_set1_host (hostname). It protects against MITM
  (--ssl-mode=VERIFY_IDENTITY). An untrusted/self-signed/wrong-host cert -> the connection is REJECTED.
  Positive control (tls_verify.lk): mysql:8 self-signed -> "certificate verify failed" (verify is active);
  the script prints `OK verify is active (self-signed rejected): ...`, or `ERROR: self-signed was ACCEPTED`
  if verify does not work.
- NO downgrade: a failed TLS handshake -> throw (it does not silently fall back to plaintext).
