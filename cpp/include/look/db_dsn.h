#pragma once
// DB DSN saf ayrıştırıcılar — "dikişi aç" (smtp_parse.h / imap_parse.h deseni): TLS karar
// mantığını I/O'dan ayır → tek inline tanım, tablo-testi mümkün (TLS handshake / canlı-DB YOK).
#include <string>

namespace look {

// ── Boundary-aware DSN query-param lookup ────────────────────────────────────
// SORUN (parser adversarial turu, 2026-09-21): resolver'lar query'de query.find("tls=insecure")
// gibi ÇIPLAK-SUBSTRING arıyordu → sessiz-yanlış-parse (fuzzer YAKALAMAZ, crash yok):
//   · ssl=false / ssl=0 / ssl=off  → "ssl=" substring'i eşleşip TLS'i ZORLA açıyordu (niyet TERS)
//   · parola/dbname içinde "insecure_plaintext=1" geçmesi REQUIRE_TLS'i sessizce ATLIYORDU
//   · ?dbname=x&opt=tls=insecure → değer-içi enjeksiyon verify'ı düşürüyordu
// ÇÖZÜM: key'i yalnız param SINIRINDA eşle (string başı veya '&' sonrası), değeri '&'e kadar al.
// dsn_lookup: key varsa true + değeri (val); yoksa false. Değersiz key ("?x") → true, val="".
inline bool dsn_lookup(const std::string& q, const std::string& key, std::string& val) {
    val.clear();
    size_t n = q.size(), klen = key.size();
    size_t pos = 0;
    while (pos <= n) {
        if (pos + klen <= n && q.compare(pos, klen, key) == 0) {
            size_t after = pos + klen;
            if (after == n || q[after] == '=' || q[after] == '&') {   // param sınırı
                if (after < n && q[after] == '=') {
                    size_t vend = q.find('&', after + 1);
                    val = q.substr(after + 1, vend == std::string::npos ? n - after - 1 : vend - after - 1);
                }
                return true;
            }
        }
        size_t amp = q.find('&', pos);
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return false;
}
// Bir DSN boolean-değerinin "kapalı" mı olduğu (açık niyet). Kapalı: 0/false/off/no/disable/disabled.
// Değersiz key ("?tls") veya bilinmeyen değer → kapalı DEĞİL (varlık = açık niyet olarak yorumla).
inline bool dsn_val_falsey(const std::string& v) {
    return v == "0" || v == "false" || v == "off" || v == "no" ||
           v == "disable" || v == "disabled";
}

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
    std::string v;
    bool tls_kv = dsn_lookup(query, "tls", v);
    std::string sv; dsn_lookup(query, "ssl", sv);
    std::string mode; dsn_lookup(query, "sslmode", mode);
    bool insecure = (tls_kv && v == "insecure") || sv == "insecure" || mode == "require";
    bool wantverify = (tls_kv && (v == "verify" || v == "1" || v == "true")) ||
                      sv == "verify" || mode == "verify-full" || mode == "verify-ca"; // 558d735
    if (insecure) { tls = true; verify = false; }
    else if (wantverify) { tls = true; verify = true; }
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
    std::string tv; bool has_tls = dsn_lookup(query, "tls", tv);
    std::string sv; bool has_ssl = dsn_lookup(query, "ssl", sv);
    if ((has_tls && tv == "insecure") || (has_ssl && sv == "insecure")) {
        tls = true; verify = false;   // açık opt-out
    } else if ((has_tls && tv == "verify") ||
               (has_ssl && (sv == "verify" || sv == "verify_identity"))) {
        tls = true; verify = true;
    } else if ((has_tls && !dsn_val_falsey(tv)) ||
               (has_ssl && !dsn_val_falsey(sv))) {
        // tls=1/true VEYA herhangi truthy ssl=... → TLS aç (verify güvenli-varsayılanı korunur).
        // #1 fix: ssl=false/0/off → TLS'i ZORLAMA (açık kapatma niyeti onurlandırılır).
        tls = true;
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
    std::string tv; bool has_tls = dsn_lookup(query, "tls", tv);
    std::string sv; bool has_ssl = dsn_lookup(query, "ssl", sv);
    if ((has_tls && tv == "insecure") || (has_ssl && sv == "insecure")) {
        tls = true; verify = false;   // açık opt-out
    } else if ((has_tls && tv == "verify") || (has_ssl && sv == "verify")) {
        tls = true; verify = true;
    } else if ((has_tls && !dsn_val_falsey(tv)) ||
               (has_ssl && !dsn_val_falsey(sv))) {
        tls = true;   // verify güvenli-varsayılanı (true) korunur (#1 fix: falsey ZORLAMAZ)
    }
}

// ── Require-TLS policy (LOOK_DB_REQUIRE_TLS) ──────────────────────────────────
// Opt-in operatör politikası: PLAINTEXT bir bağlantıyı UZAK host'a REDDET. Loopback /
// Unix-soket düz metni HER ZAMAN serbest (trafik makineden çıkmıyor — yaygın, güvenli
// app+DB-aynı-makine kurulumu) → politika o kurulumu asla kilitlemez. Bilinçli-güvenilen
// uzak düz-metin için kaçış: DSN'de ?insecure_plaintext=1 (?tls=insecure gibi adlandırıldı
// → "bilerek güvensizim" log/config'te GÖRÜNÜR; nötr "allow" değil). Loopback tespiti
// literal host üzerinde STRING-BAZLI (deterministik, DNS-çözüm/spoofing yüzeyi YOK): 127.0.0.1'e
// çözülen bir hostname UZAK sayılır → IP literali yaz ya da ?insecure_plaintext=1. (SECURITY.md'de belgeli.)
inline bool db_host_is_local(const std::string& host) {
    if (host.empty() || host[0] == '/' ||     // unix soket / belirtilmemiş / soket yolu
        host == "localhost" ||
        host == "::1" || host == "[::1]") return true;
    // 127.0.0.0/8 — ama YALNIZ IPv4 LİTERALİ (yalnız rakam+nokta). #3 fix: "127.evil.com"
    // gibi UZAK'a çözülen bir hostname eskiden "127." önekiyle yerel sayılıp REQUIRE_TLS'i
    // atlıyordu. Hostname (harf içerir) artık uzak sayılır.
    return host.rfind("127.", 0) == 0 &&
           host.find_first_not_of("0123456789.") == std::string::npos;
}
inline bool db_insecure_plaintext_opt(const std::string& query) {
    // #2 fix: sınır-duyarlı — parola/dbname içindeki "insecure_plaintext=1" substring'i
    // artık REQUIRE_TLS'i sessizce ATLAMAZ. Yalnız gerçek key=truthy-değer sayılır.
    std::string v;
    return dsn_lookup(query, "insecure_plaintext", v) && !dsn_val_falsey(v) && !v.empty();
}
// true → bağlantı REDDEDİLMELİ. require_tls: LOOK_DB_REQUIRE_TLS açık ·
// encrypted: TLS kurulacak · insecure_plaintext: ?insecure_plaintext=1 opt-out.
inline bool db_refuse_plaintext(bool require_tls, bool encrypted,
                                const std::string& host, bool insecure_plaintext) {
    if (!require_tls || encrypted || insecure_plaintext) return false;
    return !db_host_is_local(host);
}

} // namespace look
