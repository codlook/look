#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include "look/event_loop.h"

// ── LOOK IMAP4rev1 Server (RFC 3501) ──────────────────────────────────────────
//
// SMTP sunucusu maili Maildir'e yazar (deliver_maildir); IMAP aynı Maildir'i
// okur. Ortak store — migration yok, dovecot-uyumlu, crash-safe.
//   Maildir düzeni: <base>/<mailbox>/{new,cur,tmp}/
//
// Milestone planı (her biri kendi güvenlik doğrulamasıyla):
//   M1: greeting · CAPABILITY · LOGIN · LOGOUT · NOOP        (bu sürüm)
//   M2: SELECT/EXAMINE · LIST/LSUB · STATUS
//   M3: FETCH · STORE · EXPUNGE
//   M4: SEARCH · APPEND · UID varyantları
//   M5: STARTTLS · IMAPS(993)
//   M6: IDLE (push)
//   M7: güvenlik turu (fuzzing/UBSan) + performans (eşzamanlı IDLE)
//
// Güvenlik ilkeleri (baştan, env ile ayarlanır):
//   LOOK_IMAP_MAX_CONN     — eşzamanlı oturum      (default: 1000)
//   LOOK_IMAP_MAX_LINE     — komut satırı byte      (default: 8 KB)
//   LOOK_IMAP_MAX_LITERAL  — {N} literal boyutu     (default: 32 MB)
//                            — RESP2 read_bulk dersi: literal boyutu
//                              network-controlled → sınırsız alloc = OOM DoS.
//   LOOK_IMAP_MAX_ERRORS   — hata sonrası kes       (default: 5)
//   LOOK_IMAP_AUTH_DELAY   — başarısız login gecikmesi (brute-force yavaşlatma)
//
// Kimlik doğrulama: SMTP ile aynı PBKDF2-SHA256 hash deposu (auth callback).
// Mailbox izolasyonu: her kullanıcı yalnız kendi Maildir'ine erişir; mailbox
// adları canonical-path ile kök dizin içinde doğrulanır (path traversal yok).

namespace look {

// IMAP kimlik doğrulama sonucu — callback bunu döndürür.
struct ImapAuthResult {
    bool        ok = false;
    std::string maildir_path;   // kullanıcının Maildir kök dizini (ok ise)
};

// Kullanıcı/parola → auth sonucu. SMTP'nin PBKDF2 deposunu kullanır.
using ImapAuthHandler =
    std::function<ImapAuthResult(const std::string& user, const std::string& pass)>;

// ── IMAP server ───────────────────────────────────────────────────────────────
class ImapServer {
public:
    // port_imap  : :143 (STARTTLS ile yükseltilir)
    // port_imaps : :993 (implicit TLS — 0 = M5'e kadar kapalı)
    // workers    : oturum işleyici thread havuzu
    ImapServer(int port_imap, int port_imaps, int workers, ImapAuthHandler auth);
    ~ImapServer();

    ImapServer(const ImapServer&)            = delete;
    ImapServer& operator=(const ImapServer&) = delete;

    bool start();   // listener'ları aç; dinlemeye BAŞLADIYSA true (yanıltıcı "started" logunu önler)
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace look
