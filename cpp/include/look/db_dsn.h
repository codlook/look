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

// MySQL/MariaDB DSN query'sinden TLS kararı. scheme_secure = "mysqls://"/"mariadbs://" mi.
// PG'NİN TERSİNE: verify=false VARSAYILAN — DB sertifikaları çoğu kez self-signed, amaç
// kablo-şifreleme (--ssl-mode=REQUIRED). Doğrulama açıkça istenir (?tls=verify).
//   tls=verify / ssl=verify / ssl=verify_identity → tls, verify=true (CA+hostname, MITM'e karşı)
//   tls=1 / tls=true / herhangi ssl=...            → tls, verify=false (yalnız şifreleme)
// NOT: verify=false varsayılanı BİLİNÇLİ karar; değiştirmek kullanıcı-kararıdır (kırıcı).
inline void mysql_resolve_tls(const std::string& query, bool scheme_secure,
                              bool& tls, bool& verify) {
    tls = scheme_secure;
    verify = false;
    if (query.find("tls=verify")        != std::string::npos ||
        query.find("ssl=verify")        != std::string::npos ||
        query.find("ssl=verify_identity")!= std::string::npos) {
        tls = true; verify = true;
    } else if (query.find("tls=1")    != std::string::npos ||
               query.find("tls=true") != std::string::npos ||
               query.find("ssl=")     != std::string::npos) {
        tls = true;
    }
}

// Redis DSN query'sinden TLS kararı. scheme_secure = "rediss://" mi. MySQL ile aynı
// felsefe (verify=false varsayılan); MySQL'den farkı: ssl=verify_identity ve tls=true
// şifreleme-yalnız dalında yok (mevcut davranış aynen korunur).
//   tls=verify / ssl=verify → tls, verify=true · tls=1 / herhangi ssl=... → tls (şifreleme)
inline void redis_resolve_tls(const std::string& query, bool scheme_secure,
                              bool& tls, bool& verify) {
    tls = scheme_secure;
    verify = false;
    if (query.find("tls=verify") != std::string::npos ||
        query.find("ssl=verify") != std::string::npos) {
        tls = true; verify = true;
    } else if (query.find("tls=1") != std::string::npos ||
               query.find("ssl=")  != std::string::npos) {
        tls = true;
    }
}

} // namespace look
