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
// GÜVENLİ-VARSAYILAN (2026-08-11 flip): TLS açıkken verify=true VARSAYILAN — http:: ve
// PostgreSQL ile hizalı (üç sürücüden ikisi zaten doğruluyordu; MySQL/Redis tek istisnaydı,
// "LOOK TLS'i doğrular" öğrenen kullanıcı MySQL'de SESSİZCE yanılıyordu). Self-signed DB'ye
// bağlanmak için AÇIK opt-out gerekir: ?tls=insecure.
//   tls=insecure / ssl=insecure                    → tls, verify=false (açık opt-out, self-signed)
//   tls=verify / ssl=verify / ssl=verify_identity  → tls, verify=true  (açık, gereksiz ama zararsız)
//   tls=1 / tls=true / herhangi ssl=...             → tls, verify=true  (güvenli varsayılan)
// KIRICI-DEĞİŞİKLİK: eski varsayılan verify=false'du. 0-kullanıcı penceresinde flip; ilk gerçek
// kullanıcıda imkânsızlaşırdı. Ölçüldü: bilinen kurulum (test.codlook.com) plaintext mysql://
// localhost → etkilenmiyor. Kaçış kapağı ?tls=insecure (aşağıda) hazır.
inline void mysql_resolve_tls(const std::string& query, bool scheme_secure,
                              bool& tls, bool& verify) {
    tls = scheme_secure;
    verify = true;   // güvenli varsayılan (flip)
    if (query.find("tls=insecure")       != std::string::npos ||
        query.find("ssl=insecure")       != std::string::npos) {
        tls = true; verify = false;   // açık opt-out
    } else if (query.find("tls=verify")        != std::string::npos ||
               query.find("ssl=verify")        != std::string::npos ||
               query.find("ssl=verify_identity")!= std::string::npos) {
        tls = true; verify = true;
    } else if (query.find("tls=1")    != std::string::npos ||
               query.find("tls=true") != std::string::npos ||
               query.find("ssl=")     != std::string::npos) {
        tls = true;   // verify güvenli-varsayılanı (true) korunur
    }
}

// Redis DSN query'sinden TLS kararı. scheme_secure = "rediss://" mi. MySQL ile aynı
// GÜVENLİ-VARSAYILAN felsefesi (verify=true varsayılan, ?tls=insecure açık opt-out).
//   tls=insecure / ssl=insecure → tls, verify=false · tls=verify / ssl=verify → tls, verify=true
//   tls=1 / herhangi ssl=...     → tls, verify=true (güvenli varsayılan)
inline void redis_resolve_tls(const std::string& query, bool scheme_secure,
                              bool& tls, bool& verify) {
    tls = scheme_secure;
    verify = true;   // güvenli varsayılan (flip)
    if (query.find("tls=insecure") != std::string::npos ||
        query.find("ssl=insecure") != std::string::npos) {
        tls = true; verify = false;   // açık opt-out
    } else if (query.find("tls=verify") != std::string::npos ||
               query.find("ssl=verify") != std::string::npos) {
        tls = true; verify = true;
    } else if (query.find("tls=1") != std::string::npos ||
               query.find("ssl=")  != std::string::npos) {
        tls = true;   // verify güvenli-varsayılanı (true) korunur
    }
}

} // namespace look
