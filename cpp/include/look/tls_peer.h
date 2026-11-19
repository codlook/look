// TLS peer name: SNI + certificate name verification — the SINGLE definition (http::, MySQL, PostgreSQL,
// Redis, SMTP). Modeled on Go crypto/tls:
//   * name (example.com) → SNI is sent, matched against the certificate's DNS names;
//   * IP address (10.0.0.5, ::1) → SNI is NOT SENT (RFC 6066 §3 forbids IP) and it is matched only against the
//     certificate's IP addresses (IP SAN).
// OLD BUG (measured): an IP was handled like a name → a valid IP SAN certificate was REJECTED
// (an internal service at "https://10.0.0.5" could not be reached at all), while a certificate with "127.0.0.1"
// as a DNS name was ACCEPTED; the IP was sent in SNI. Also, the result of SSL_set1_host was
// not checked in http::: if it failed, name verification would be skipped silently.
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

// Every client context (http::, MySQL, PostgreSQL, Redis, SMTP): at least TLS 1.2 (Go crypto/tls
// client default; LOOK's SMTP/IMAP servers already work this way). OLD: the floor was left to OpenSSL's and the
// operating system's setting — AlmaLinux's crypto-policies rejected TLS 1.1 while
// on Debian the same binary connected to a TLS 1.1 server (measured): behavior varied by machine.
inline void tls_client_defaults(SSL_CTX* ctx) {
    if (ctx) SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
}

inline bool tls_host_is_ip(const std::string& host) {
    unsigned char buf[16];
    return inet_pton(AF_INET, host.c_str(), buf) == 1 || inet_pton(AF_INET6, host.c_str(), buf) == 1;
}

// verify=false → SNI only (unverified connection: ?tls=insecure). Return false = name could not be set
// (the caller must not establish the connection — verification must not be skipped silently).
inline bool tls_set_peer(SSL* ssl, const std::string& host, bool verify) {
    const bool ip = tls_host_is_ip(host);
    if (!ip) SSL_set_tlsext_host_name(ssl, host.c_str());
    if (!verify) return true;
    if (ip) return X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), host.c_str()) == 1;
    return SSL_set1_host(ssl, host.c_str()) == 1;
}

// The reason reported to the user when the handshake fails (like Go's x509 errors): if there is a certificate verification
// error, its text ("certificate has expired", "unable to get local issuer certificate",
// "hostname mismatch" …), otherwise OpenSSL's last error reason ("unsupported protocol" …).
inline std::string tls_failure_reason(SSL* ssl) {
    long vr = ssl ? SSL_get_verify_result(ssl) : X509_V_OK;
    std::string why;
    if (vr != X509_V_OK) why = std::string("certificate verify failed: ") + X509_verify_cert_error_string(vr);
    else {
        unsigned long e = ERR_peek_last_error();
        if (e) { const char* r = ERR_reason_error_string(e); why = r ? r : "error"; }
    }
    ERR_clear_error();
    // Error queue is empty: the peer closed the connection during the handshake (DB clients used to
    // print "error:00000000:lib(0):func(0):reason(0)" here).
    if (why.empty()) why = "the server closed the connection during the handshake";
    return why;
}

} // namespace look
