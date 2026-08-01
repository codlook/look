# DB-TLS guard (mysql:8 / postgres:16 gerektirir)

## PostgreSQL TLS (pg_*.lk) — 4 durum matrisi + pozitif kontrol

postgres_client'a TLS eklendi: `postgresqls://` / `?tls=verify` → SSLRequest → 'S' → SSL_connect
→ (verify) SSL_VERIFY_PEER + SSL_set1_host. **verify DOGRU VARSAYILAN** (PG'de eski TLS
kullanicisi yok → guvenli baslar; mysql/redis'in tersine). Sunucu TLS vermezse ('N') SESSIZCE
plaintext'e DUSMEZ — temiz hata. `?tls=insecure` → sifreli ama dogrulanmamis (self-signed dev).

Ampirik matris (postgres:16, CA-imzali cert SAN=look-pg, look-build openssl):
- `pg_reject_tls.lk`  — ssl=off sunucu → 'N' → temiz hata (plaintext'e dusmez). ✓
- `pg_untrusted.lk`   — guvenilmeyen CA + verify(varsayilan) → certificate verify failed → reddet. ✓
- `pg_valid.lk`       — SSL_CERT_FILE=ca.crt + hostname eslesir → baglanti + SELECT 1. ✓
- `pg_mismatch.lk`    — gecerli CA ama IP ile baglan (SAN uyusmaz) → SSL_set1_host reddet. ✓
- `pg_insecure.lk`    — POZITIF KONTROL: ?tls=insecure → guvenilmeyen cert'e RAGMEN baglanir (42);
                        2/4 reddinin verify'dan geldigini, bozuk handshake'ten degil, kanitlar. ✓

Kosum (network looknet + postgres:16 ssl=on, cert SAN=look-pg):
    build/lk tests/db_tls/pg_reject_tls.lk        # OK reddedildi (TLS-siz sunucu)
    build/lk tests/db_tls/pg_untrusted.lk         # OK reddedildi (guvenilmeyen cert)
    SSL_CERT_FILE=/certs/ca.crt build/lk tests/db_tls/pg_valid.lk    # 1
    SSL_CERT_FILE=/certs/ca.crt PG_MISMATCH_DSN=postgresqls://.../@<IP>/testdb \
        build/lk tests/db_tls/pg_mismatch.lk      # OK reddedildi (hostname)

---

# DB-TLS guard: MySQL (mysql:8 gerektirir)

MySQL DB-TLS implementasyonu (mysqls:// / ?tls=1) — SSLRequest→SSL_connect→auth+data TLS üstünde.
Redis'te (rediss stub, aslında yok) vardı sanılıyordu; MySQL'de gerçek TLS eklendi (asimetri kapandı).

## Kanıt düzeyi
- Kod: http_client SSL kalıbı aynalandı (SSL_CTX TLS_client_method, SSL_connect, SSL_read/write);
  MySQL-özgü SSLRequest SIRASI: initial-handshake sonrası, auth ÖNCESİ (credentials TLS'ten sonra).
  caching_sha2 full-auth TLS'te CLEARTEXT şifre (RSA değil — kanal zaten şifreli).
- Ampirik (yazılımcı, Docker mysql:8): `Ssl_cipher=TLS_AES_256_GCM_SHA384` (TLS 1.3 gerçek şifreleme),
  full-auth (ilk bağlantı) + fast-auth (cache'li 2. bağlantı) TLS+non-TLS hepsi çalıştı, SELECT parite.
  Non-TLS regresyon: Ssl_cipher boş, bağlantı bozulmadı.
- SSL_VERIFY_NONE (--ssl-mode=REQUIRED): kablo şifreleme (self-signed DB cert'lerle çalışır);
  sertifika doğrulama gelecekte ayrı mod.

## Koşum (mysql:8 + docker network looknet)
    docker network create looknet
    docker run -d --name look-mysql8 --network looknet -e MYSQL_ROOT_PASSWORD=rootpw \
      -e MYSQL_DATABASE=testdb -e MYSQL_USER=looktest -e MYSQL_PASSWORD=testpass mysql:8
    # hazır olunca:
    docker run --rm --network looknet -v <repo>/cpp:/look/cpp -w /look/cpp look-build \
      bash -lc "/look/cpp/build/lk tests/db_tls/tls_on.lk"   # → TLS_AES_256_GCM_SHA384

## İki mod (?tls=1 vs ?tls=verify)
- `?tls=1` (veya mysqls://): şifreleme, SSL_VERIFY_NONE — pasif dinlemeye karşı korur,
  AKTİF MITM'e karşı DEĞİL (sunucu cert'i doğrulanmaz). Self-signed DB cert'lerle çalışır
  (--ssl-mode=REQUIRED). Kanıt: mysql:8 → Ssl_cipher=TLS_AES_256_GCM_SHA384.
- `?tls=verify`: şifreleme + SSL_VERIFY_PEER + SSL_set1_host (hostname) — MITM'e karşı korur
  (--ssl-mode=VERIFY_IDENTITY). Güvenilmez/self-signed/yanlış-host cert → bağlantı REDDEDİLİR.
  Pozitif kontrol (tls_verify.lk): mysql:8 self-signed → "certificate verify failed" (verify aktif).
- Downgrade YOK: TLS handshake başarısız → throw (plaintext'e sessizce düşmez).
