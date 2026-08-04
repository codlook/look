# LOOK — Gömülü SMTP Sunucu

LOOK gömülü bir SMTP sunucu içerir. Dış servis (Postfix, Exim) gerekmez.  
Yalnızca `--mode http` modunda aktif olur.

---

## Hızlı Başlangıç

`.env` dosyasına ekle:

```
LOOK_SMTP_PORT=25
LOOK_SMTP_SUB_PORT=587
LOOK_SMTP_LOCAL_DOMAINS=myapp.com
LOOK_SMTP_DKIM_KEY_FILE=/etc/dkim/myapp.pem
LOOK_SMTP_DKIM_SELECTOR=default
LOOK_SMTP_DKIM_DOMAIN=myapp.com
LOOK_SMTP_USER_DB=mysql://user:pass@127.0.0.1/mydb
LOOK_MAIL_DIR=/var/mail/look
```

```bash
lk --mode http --port 7400 --workers 4
# Hem HTTP :7400 hem SMTP :25/:587 aynı anda dinler
```

---

## Env Değişkenleri

| Değişken | Varsayılan | Açıklama |
|----------|-----------|----------|
| `LOOK_SMTP_PORT` | — | MTA portu (25). Boşsa SMTP devre dışı. |
| `LOOK_SMTP_SUB_PORT` | 0 | Submission portu (587) — authenticated |
| `LOOK_SMTP_LOCAL_DOMAINS` | — | Virgülle ayrılmış yerel domain listesi. Open relay engeli için zorunlu. |
| `LOOK_SMTP_DKIM_KEY_FILE` | — | RSA private key (PEM). Belirtilmezse DKIM imzası atlanır. |
| `LOOK_SMTP_DKIM_SELECTOR` | `default` | DNS TXT kaydı selector (`default._domainkey.myapp.com`) |
| `LOOK_SMTP_DKIM_DOMAIN` | — | DKIM `d=` alanı (genellikle ana domain) |
| `LOOK_SMTP_USER_DB` | — | `mysql://...` DSN. `mail_users` tablosundan AUTH doğrulama. |
| `LOOK_SMTP_AUTH_TOKEN` | — | Basit tek-token auth (test/geliştirme için). DB yoksa fallback. |
| `LOOK_SMTP_BANNER` | `localhost` | EHLO banner metni |
| `LOOK_MAIL_DIR` | `/var/mail/look` | Maildir teslim dizini |
| `LOOK_SMTP_MAX_CONN` | 1000 | Maksimum eş zamanlı bağlantı |
| `LOOK_SMTP_MAX_MSG_SIZE` | 26214400 | Maksimum mesaj boyutu (25 MB) |
| `LOOK_SMTP_MAX_RCPT` | 100 | Mesaj başına maksimum alıcı |
| `LOOK_SMTP_MAX_ERRORS` | 5 | Bağlantı başına hata limiti (sonra kes) |

---

## Kullanıcı Veritabanı (`mail_users`)

`LOOK_SMTP_USER_DB` DSN ayarlandığında SMTP AUTH PLAIN, `mail_users` tablosundan doğrulama yapar.

```sql
CREATE TABLE mail_users (
    id       INT AUTO_INCREMENT PRIMARY KEY,
    email    VARCHAR(255) UNIQUE NOT NULL,
    password VARCHAR(255) NOT NULL,  -- pbkdf2$sha256$... (auth::hash ile üret)
    active   TINYINT DEFAULT 1
);
```

Kullanıcı oluşturma (LOOK route'unda):

```lk
route("GET", "/admin/mail-user-ekle", function() {
    $conn = db::connect(env("DB_DSN", ""))
    $hash = auth::hash(request::get("sifre"))
    db::exec($conn,
        "INSERT INTO mail_users (email, password, active) VALUES (?, ?, 1)",
        [request::get("email"), $hash]
    )
    response::json(["ok" => true])
})
```

---

## DKIM Anahtarı Oluşturma

```bash
# Anahtar üret
openssl genrsa -out /etc/dkim/myapp.pem 2048
chmod 600 /etc/dkim/myapp.pem

# DNS TXT kaydı için public key al
openssl rsa -in /etc/dkim/myapp.pem -pubout -outform DER 2>/dev/null | base64 | tr -d '\n'
```

DNS'e ekle (`default._domainkey.myapp.com`):

```
v=DKIM1; k=rsa; p=<public_key_base64>
```

---

## Güvenlik Özellikleri

| Özellik | Detay |
|---------|-------|
| Open relay engeli | `LOOK_SMTP_LOCAL_DOMAINS` dışına unauthenticated relay → `550 Relay access denied` |
| DKIM imzalama | AUTH başarılı veya TLS varsa → `DKIM-Signature: v=1; a=rsa-sha256` eklenir |
| SPF kontrolü | Gelen mesaj SPF kontrolünden geçirilir (`SpfResult` enum) |
| PBKDF2-HMAC-SHA256 | AUTH şifreleri `pbkdf2$sha256$100000$...` formatında — `auth::hash()` ile uyumlu |
| Token bucket | Bağlantı başına hız sınırı |

---

## Maildir Formatı

Teslim edilen mesajlar Maildir formatında **her yerel alıcının kendi dizinine** yazılır:
`LOOK_MAIL_DIR/<alıcı>/inbox/new/`. Böylece gömülü **IMAP sunucusu** her kullanıcının
INBOX'ını doğru yerden okur (bkz. [imap-server.md](imap-server.md)).

```
/var/mail/look/
  alice@myapp.com/
    inbox/
      new/  1751234567.abc   ← alice'e gelen mesaj
      cur/                    ← IMAP STORE ile okunmuş/bayraklı (:2,S)
      tmp/
  bob@myapp.com/
    inbox/ ...
```

> Alıcı adı dizin bileşeni olduğundan, `/` `\` `..` veya kontrol karakteri içeren
> güvensiz alıcılar teslim sırasında atlanır (path traversal koruması).

---

## IMAP ile Birlikte — Tam Mail Zinciri

SMTP tek başına yalnız **teslim eder** (Maildir'e yazar). Kullanıcıların mektuplarını
okuyabilmesi için gömülü **IMAP sunucusunu** da aç: `LOOK_IMAP_PORT=143`. İkisi aynı
`mail_users` tablosunu ve aynı Maildir'i paylaşır — SMTP alır, IMAP sunar.
Ayrıntı: [imap-server.md](imap-server.md).

---

## Docker ile Kullanım

```bash
docker run -p 7400:7400 -p 25:2525 -p 587:5870 \
  -v /etc/dkim:/opt/dkim:ro \
  -v /var/mail/look:/var/mail/look \
  -e LOOK_SMTP_PORT=2525 \
  -e LOOK_SMTP_SUB_PORT=587 \
  -e LOOK_SMTP_LOCAL_DOMAINS=myapp.com \
  -e LOOK_SMTP_DKIM_KEY_FILE=/opt/dkim/myapp.pem \
  -e LOOK_SMTP_DKIM_DOMAIN=myapp.com \
  -e LOOK_SMTP_USER_DB=mysql://user:pass@host.docker.internal/mydb \
  -e LOOK_MAIL_DIR=/var/mail/look \
  look-prod
```

> Container içinde root port'larına bağlanamaz — `LOOK_SMTP_PORT=2525` kullan, host'ta `iptables` veya Docker `-p 25:2525` ile map et.

---

## Test

```bash
# Bağlantı testi
telnet localhost 587

# AUTH PLAIN (base64: \0user@domain.com\0password)
B64=$(printf '\0user@domain.com\0mypassword' | base64 | tr -d '\n')
echo "AUTH PLAIN $B64"

# Relay engeli testi
# RCPT TO dış domain → 550 beklenir
```

---

## mail:: Modülü ile Fark

| | `mail::` | Gömülü SMTP |
|--|----------|-------------|
| Yöntem | Harici API (Mailgun, SendGrid...) | Kendi sunucu |
| Kurulum | Sadece API key | DNS MX, DKIM, PTR kaydı gerekir |
| Kullanım | `mail::send(to, subject, body)` | Gelen posta teslimi + giden AUTH |
| Uygun | Transactional e-posta | Mail sunucu kurmak isteyenler |

İkisi birlikte kullanılabilir: `mail::` giden için, gömülü SMTP gelen için.
