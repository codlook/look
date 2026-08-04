# LOOK — Gömülü IMAP Sunucu

LOOK gömülü, sıfır bağımlılık bir **IMAP4rev1** sunucu içerir (RFC 3501 + RFC 2177 IDLE).
Dış servis (Dovecot, Courier) gerekmez. Gömülü SMTP sunucu ile aynı Maildir deposunu
paylaşır: **SMTP mektubu teslim eder → IMAP istemcilere sunar.** Yalnızca `--mode http`
modunda aktif olur.

Gerçek istemciler (Thunderbird, Roundcube, Apple Mail, K-9 / mobil) bağlanıp mail
**listeler, okur, yazar, bayraklar, siler, arar ve anlık bildirim (IDLE push) alır** —
hepsi TLS altında.

---

## Hızlı Başlangıç

`.env` dosyasına ekle:

```
# --- Gelen posta zinciri (SMTP alır → Maildir → IMAP sunar) ---
LOOK_SMTP_PORT=25                 # MTA (sunucu→sunucu teslim)
LOOK_SMTP_SUB_PORT=587            # Submission (authenticated giden)
LOOK_SMTP_LOCAL_DOMAINS=myapp.com # yerel domainler (open relay engeli)

LOOK_IMAP_PORT=143                # IMAP (STARTTLS ile yükseltilir)
LOOK_IMAP_PORT_TLS=993            # IMAPS (implicit TLS) — opsiyonel
LOOK_IMAP_CERT=/etc/look/mail.crt # TLS sertifikası (yoksa LOOK_SMTP_CERT paylaşılır)
LOOK_IMAP_KEY=/etc/look/mail.key

# --- Ortak kimlik + depo ---
LOOK_MAIL_USER_DB=mysql://user:pass@127.0.0.1/mydb  # mail_users tablosu (SMTP ile ortak)
LOOK_MAIL_DIR=/var/mail/look                          # Maildir kökü
```

```bash
lk --mode http --port 7400 --workers 4
# Aynı anda: HTTP :7400 · SMTP :25/:587 · IMAP :143 · IMAPS :993
```

---

## Env Değişkenleri

| Değişken | Varsayılan | Açıklama |
|----------|-----------|----------|
| `LOOK_IMAP_PORT` | — | IMAP portu (143). Boşsa IMAP devre dışı. |
| `LOOK_IMAP_PORT_TLS` | 0 | IMAPS / implicit TLS portu (993). Yalnız sertifika varsa açılır. |
| `LOOK_IMAP_CERT` | — | TLS sertifikası (PEM zinciri). Yoksa `LOOK_SMTP_CERT` kullanılır. |
| `LOOK_IMAP_KEY` | — | TLS özel anahtarı (PEM). Yoksa `LOOK_SMTP_KEY` kullanılır. |
| `LOOK_MAIL_USER_DB` | — | `mysql://...` DSN. `mail_users` tablosundan LOGIN doğrulama. Yoksa `LOOK_SMTP_USER_DB`. |
| `LOOK_MAIL_USER` / `LOOK_MAIL_PASS` | — | DB yoksa tek-kullanıcı fallback (dev / basit deployment). Sabit-zamanlı karşılaştırma. |
| `LOOK_MAIL_DIR` | `/var/mail/look` | Maildir kökü. Kullanıcının INBOX'ı: `<dir>/<user>/inbox`. |
| `LOOK_IMAP_MAX_CONN` | 1000 | Maksimum eş zamanlı bağlantı (DoS sınırı). |
| `LOOK_IMAP_MAX_LINE` | 8192 | Komut satırı üst sınırı (satır bombası → bağlantı kesilir). |
| `LOOK_IMAP_MAX_LITERAL` | 33554432 | APPEND literal üst sınırı (32 MB). Aşılırsa **veri okunmadan** `NO [TOOBIG]`. |
| `LOOK_IMAP_MAX_ERRORS` | 5 | Bağlantı başına protokol hatası limiti (sonra kes). |
| `LOOK_IMAP_AUTH_DELAY_MS` | 500 | Başarısız LOGIN'de sabit gecikme (brute-force yavaşlatma). |
| `LOOK_IMAP_IDLE_TIMEOUT` | 1800 | Boşta bağlantı recv timeout'u — saniye (Slowloris + RFC autologout). |
| `LOOK_IMAP_IDLE_TICK_MS` | 15000 | IDLE sırasında mailbox tarama periyodu (yeni-mail tespiti). |

---

## Desteklenen Komutlar

| Komut | Durum | Not |
|-------|-------|-----|
| `CAPABILITY` | ✅ | `IMAP4rev1 IDLE` (+ `STARTTLS LOGINDISABLED` düz-metinde) |
| `NOOP` · `LOGOUT` | ✅ | |
| `STARTTLS` | ✅ | RFC 3501 §6.2.1 — sonrası oturum durumu sıfırlanır |
| `LOGIN` | ✅ | `mail_users` pbkdf2 veya tek-kullanıcı fallback |
| `SELECT` / `EXAMINE` | ✅ | Kararlı sequence snapshot; EXISTS/RECENT/UIDVALIDITY/FLAGS |
| `LIST` / `LSUB` | ✅ | Maildir++ alt klasörler (`.Sent` → `Sent`) |
| `STATUS` | ✅ | MESSAGES / RECENT |
| `FETCH` | ✅ | seq-set, `FLAGS` `UID` `RFC822.SIZE` `BODY[]` `BODY[HEADER]` `BODY[TEXT]` |
| `STORE` | ✅ | `+FLAGS` `-FLAGS` `FLAGS` (+`.SILENT`), Maildir `:2,` rename |
| `EXPUNGE` | ✅ | `\Deleted` işaretli mesajları siler + `* n EXPUNGE` bildirimi |
| `APPEND` | ✅ | literal + flag; boyut sınırı OOM'a karşı önden doğrulanır |
| `SEARCH` | ✅ | aşağıya bakın |
| `IDLE` | ✅ | RFC 2177 — canlı yeni-mail push |

### SEARCH ölçütleri

Çoklu ölçüt **AND**'lenir. Tırnaklı argüman desteklenir (`SEARCH FROM "ali veli"`).

```
ALL · SEEN/UNSEEN · DELETED/UNDELETED · FLAGGED/UNFLAGGED · ANSWERED/UNANSWERED
DRAFT/UNDRAFT · NEW/OLD/RECENT
FROM · TO · CC · SUBJECT   (başlık alanı, alan-bazlı — yanlış-pozitif yok)
BODY · TEXT                (gövde / tam metin — bellek sınırlı okuma)
HEADER <alan> <değer>
<seq-set>  (ör. 1:3, 2:*)  ·  UID <seq-set>
```

Bilinmeyen ölçüt → `BAD`. Gövde/tam-metin araması `read_file_capped` ile sınırlıdır (OOM yok).

---

## IDLE — Anlık Bildirim (RFC 2177)

Profesyonel ve mobil istemciler yeni mektubu polling yerine **push** ile alır:

```
C: a1 IDLE
S: + idling
   ... (SMTP yeni mektup teslim eder) ...
S: * 2 EXISTS
S: * 2 RECENT
C: DONE
S: a1 OK IDLE tamamlandı
```

Sunucu, IDLE sırasında hem soketi (`DONE`) hem mailbox'ı (`LOOK_IMAP_IDLE_TICK_MS`
periyodunda) izler. Yeni mesajlar mevcut sequence snapshot'ının **sonuna** eklenir —
oturum içi sequence numaraları RFC 3501 uyarınca sabit kalır.

---

## Kullanıcı Veritabanı (`mail_users`) — SMTP ile Ortak

IMAP LOGIN, SMTP AUTH ile **aynı tabloyu ve aynı pbkdf2 doğrulamasını** kullanır.
Tek kullanıcı kaydı hem giden (SMTP submission) hem gelen (IMAP) için geçerlidir.

```sql
CREATE TABLE mail_users (
    id       INT AUTO_INCREMENT PRIMARY KEY,
    email    VARCHAR(255) UNIQUE NOT NULL,
    password VARCHAR(255) NOT NULL,  -- pbkdf2$sha256$... (auth::hash ile üret)
    active   TINYINT DEFAULT 1
);
```

```lk
route("POST", "/admin/mail-user-ekle", function() {
    $conn = db::connect(env("DB_DSN", ""))
    $hash = auth::hash(request::post("sifre"))
    db::exec($conn,
        "INSERT INTO mail_users (email, password, active) VALUES (?, ?, 1)",
        [request::post("email"), $hash]
    )
    response::json(["ok" => true])
})
```

Kullanıcı adı (e-posta), giriş sonrası Maildir yol bileşeni olur; bu yüzden `/`, `\`,
`..`, kontrol karakteri içeren adlar reddedilir (defense-in-depth).

---

## Maildir Hizalaması — SMTP ↔ IMAP

Gömülü SMTP, her **yerel alıcıya** ayrı Maildir'e teslim eder; IMAP `INBOX` tam olarak
aynı yeri okur:

```
/var/mail/look/
  alice@myapp.com/
    inbox/
      new/  1751234567.abc   ← SMTP'nin teslim ettiği mektup
      cur/                    ← okunmuş/bayraklı (STORE → :2,S rename)
      tmp/
  bob@myapp.com/
    inbox/ ...
```

Böylece `RCPT TO:<alice@myapp.com>` ile gelen mektup, `LOGIN alice@myapp.com` yapan
istemcinin INBOX'ında görünür. Ek klasörler (Sent, Drafts) Maildir++ nokta-önekiyle
tutulur (`.Sent`) ve `LIST`'te `Sent` olarak görünür.

---

## TLS — STARTTLS ve IMAPS

İki yükseltme yolu da desteklenir:

- **STARTTLS (143):** İstemci düz-metin bağlanır, `STARTTLS` ile TLS'e yükseltir.
  Sertifika yapılıyken sunucu `LOGINDISABLED` reklamı yapar ve TLS öncesi `LOGIN`'i
  `NO [PRIVACYREQUIRED]` ile reddeder — **kimlik bilgisi asla şifresiz gitmez.**
- **IMAPS (993):** Bağlantı açılır açılmaz TLS el sıkışması (implicit TLS).

TLS 1.2 taban; SSLv2/SSLv3/TLS1.0/TLS1.1 kapalıdır. Sertifika `LOOK_IMAP_CERT/KEY` ile,
yoksa gömülü SMTP'nin `LOOK_SMTP_CERT/KEY` sertifikası paylaşılarak sağlanır. Sertifika
hiç yoksa sunucu düz-metin çalışır (yalnız geliştirme).

```bash
# Self-signed test sertifikası
openssl req -x509 -newkey rsa:2048 -keyout mail.key -out mail.crt \
  -days 365 -nodes -subj "/CN=mail.myapp.com"
```

---

## Güvenlik Özellikleri

| Özellik | Detay |
|---------|-------|
| Kimlik bilgisi gizliliği | TLS varken düz-metin LOGIN reddedilir (`LOGINDISABLED` / `PRIVACYREQUIRED`) |
| OOM koruması | APPEND literal boyutu **okumadan önce** doğrulanır (`LOOK_IMAP_MAX_LITERAL`) → `NO [TOOBIG]` |
| Satır bombası | Komut satırı `LOOK_IMAP_MAX_LINE` ile sınırlı |
| Bağlantı taşması | `LOOK_IMAP_MAX_CONN` eş zamanlı bağlantı tavanı |
| Slowloris | `SO_RCVTIMEO` idle timeout (`LOOK_IMAP_IDLE_TIMEOUT`) |
| Brute-force | Başarısız LOGIN'de sabit gecikme + hata limiti (bağlantıyı keser) |
| Path traversal | mailbox adı + kullanıcı adı + SMTP alıcısı — üç katmanda `..`/mutlak yol/kontrol-karakteri reddi + `weakly_canonical` kök-içi doğrulama |
| Sequence kararlılığı | RFC 3501 — seq numaraları oturum içinde sabit; yalnız EXPUNGE değiştirir (yanlış mesaj silinmesini önler) |
| Sabit-zamanlı karşılaştırma | pbkdf2 doğrulama ve tek-kullanıcı fallback timing-attack'a karşı |

Doğrulama: ASan+UBSan fuzzing (taşan/negatif literal, bozuk seq-set, tokenizer uç
durumları, protokol ihlali, path traversal bombardımanı) → **0 UB, 0 çökme.** Uçtan uca
interop testi: SMTP teslim → disk → IMAP LOGIN+SELECT+FETCH aynı mesajı okur.

---

## Test

```bash
# STARTTLS + LOGIN (openssl s_client STARTTLS'i otomatik yönetir)
openssl s_client -quiet -crlf -starttls imap -connect localhost:143
a1 LOGIN alice@myapp.com sifre
a2 SELECT INBOX
a3 FETCH 1 (BODY[HEADER])
a4 LOGOUT

# IMAPS (implicit TLS)
openssl s_client -quiet -crlf -connect localhost:993
```

Depodaki hazır senaryolar: `cpp/docker/imap_test.sh` (SEARCH), `imap_build_test.sh`
(TLS + IDLE), `imap_fuzz.sh` (ASan+UBSan), `imap_interop.sh` (uçtan uca SMTP→IMAP).

---

## Mimari Notlar

- **Thread-per-connection** — IMAP oturumları uzun ömürlüdür (IDLE); her bağlantı kendi
  thread'inde. `MAX_CONN` ile toplam kaynak sınırlı. TLS I/O `thread_local` üzerinden
  ele alınır, böylece tüm komut yolları şifrelidir.
- **Sıfır bağımlılık** — yalnız OpenSSL (TLS için, gömülü SMTP zaten linkler). Maildir
  ayrıştırma, sequence yönetimi, SEARCH tümü LOOK çekirdeğinde.
- **Warm start** — sunucu ana binary ile birlikte ayağa kalkar; ayrı süreç/servis yok.
