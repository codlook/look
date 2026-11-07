// TLS karşı taraf adı: SNI + sertifika ad doğrulaması — TEK tanım (http::, MySQL, PostgreSQL,
// Redis, SMTP). Model Go crypto/tls:
//   * ad (example.com) → SNI gönderilir, sertifikanın DNS adlarıyla eşleşir;
//   * IP adresi (10.0.0.5, ::1) → SNI GÖNDERİLMEZ (RFC 6066 §3 IP'yi yasaklar) ve yalnız sertifikanın
//     IP adresleriyle (IP SAN) eşleşir.
// ESKİ HATA (ölçüldü): IP de ad gibi işleniyordu → geçerli IP SAN sertifikası REDDEDİLİYOR
// ("https://10.0.0.5" iç servise hiç bağlanılamıyordu), buna karşılık DNS adı olarak "127.0.0.1"
// yazan sertifika KABUL ediliyordu; IP, SNI'de gönderiliyordu. Ayrıca SSL_set1_host'un sonucu
// http::'de denetlenmiyordu: başarısız olsa ad doğrulaması sessizce atlanırdı.
#pragma once
#include <string>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <openssl/err.h>
#ifdef _WIN32
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#endif

namespace look {

// Her istemci bağlamı (http::, MySQL, PostgreSQL, Redis, SMTP): en az TLS 1.2 (Go crypto/tls
// istemci varsayılanı; LOOK'un SMTP/IMAP sunucuları zaten böyle). ESKİ: taban OpenSSL'in ve
// işletim sisteminin ayarına kalmıştı — AlmaLinux'un crypto-policies'i TLS 1.1'i reddederken
// Debian'da aynı ikili TLS 1.1 sunucusuna bağlanıyordu (ölçüldü): davranış makineye göre değişiyordu.
inline void tls_client_defaults(SSL_CTX* ctx) {
    if (ctx) SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
}

inline bool tls_host_is_ip(const std::string& host) {
    unsigned char buf[16];
    return inet_pton(AF_INET, host.c_str(), buf) == 1 || inet_pton(AF_INET6, host.c_str(), buf) == 1;
}

// verify=false → yalnız SNI (doğrulamasız bağlantı: ?tls=insecure). Dönüş false = ad ayarlanamadı
// (çağıran bağlantıyı kurmamalı — doğrulama sessizce atlanmasın).
inline bool tls_set_peer(SSL* ssl, const std::string& host, bool verify) {
    const bool ip = tls_host_is_ip(host);
    if (!ip) SSL_set_tlsext_host_name(ssl, host.c_str());
    if (!verify) return true;
    if (ip) return X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), host.c_str()) == 1;
    return SSL_set1_host(ssl, host.c_str()) == 1;
}

// El sıkışma başarısızsa kullanıcıya giden neden (Go'nun x509 hataları gibi): sertifika doğrulama
// hatası varsa onun metni ("certificate has expired", "unable to get local issuer certificate",
// "hostname mismatch" …), yoksa OpenSSL'in son hata nedeni ("unsupported protocol" …).
inline std::string tls_failure_reason(SSL* ssl) {
    long vr = ssl ? SSL_get_verify_result(ssl) : X509_V_OK;
    std::string why;
    if (vr != X509_V_OK) why = std::string("certificate verify failed: ") + X509_verify_cert_error_string(vr);
    else {
        unsigned long e = ERR_peek_last_error();
        if (e) { const char* r = ERR_reason_error_string(e); why = r ? r : "error"; }
    }
    ERR_clear_error();
    // Hata kuyruğu boş: karşı taraf el sıkışma sırasında bağlantıyı kapattı (eskiden DB istemcileri
    // burada "error:00000000:lib(0):func(0):reason(0)" yazıyordu).
    if (why.empty()) why = "the server closed the connection during the handshake";
    return why;
}

} // namespace look
