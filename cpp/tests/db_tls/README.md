# DB-TLS guard (mysql:8 gerektirir)

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
