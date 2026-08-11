#pragma once
// DB DSN saf ayrıştırıcılar — "dikişi aç" (smtp_parse.h / imap_parse.h deseni): TLS karar
// mantığını I/O'dan ayır → tek inline tanım, tablo-testi mümkün (TLS handshake / canlı-DB YOK).
#include <string>

namespace look {

// PostgreSQL DSN query'sinden TLS kararı. scheme_secure = "postgresqls://" mi.
//   → tls: TLS kurulacak mı · verify: sertifika+hostname doğrulanacak mı (SSL_VERIFY_PEER).
// PG'de TLS YENİ → verify DOĞRU VARSAYILAN (mysql/redis'in tersine güvenli başlar).
//   insecure / sslmode=require        → tls, verify=false (şifreli, doğrulamasız — libpq 'require')
//   verify / sslmode=verify-full/-ca  → tls, verify=true  (CA+hostname; LOOK verify-ca'yı da katıya çeker)
// KRİTİK (558d735): sslmode=verify-ca eskiden HİÇBİR dala uymuyordu → tls=scheme (postgres://'de
//   FALSE) kalıyor → SESSİZCE PLAINTEXT. Şimdi verify dalında. Bu fonksiyonun tablo-testi o
//   regresyonu ağsız kilitler.
inline void pg_resolve_tls(const std::string& query, bool scheme_secure,
                           bool& tls, bool& verify) {
    tls = scheme_secure;
    verify = true;   // güvenli varsayılan
    if (query.find("tls=insecure")    != std::string::npos ||
        query.find("ssl=insecure")    != std::string::npos ||
        query.find("sslmode=require") != std::string::npos) {
        tls = true; verify = false;
    } else if (query.find("tls=verify")          != std::string::npos ||
               query.find("ssl=verify")          != std::string::npos ||
               query.find("sslmode=verify-full") != std::string::npos ||
               query.find("sslmode=verify-ca")   != std::string::npos ||   // 558d735
               query.find("tls=1")               != std::string::npos ||
               query.find("tls=true")            != std::string::npos) {
        tls = true; verify = true;
    }
}

} // namespace look
