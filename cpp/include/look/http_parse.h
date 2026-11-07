// HTTP/1.1 istek/başlık saf ayrıştırıcısı — socket'ten AYRI (fuzz + test hedefi).
// "Dikişi aç": ham istek tamponu → HttpRequest yorumlama mantığı burada; I/O (recv,
// timeout, chunked gövde) http_server.cpp'de. Tek tanım (http_server.cpp bunu include
// eder) → drift yok. Bu yüzey PRE-AUTH: ağdaki herkes bu baytları besler.
//
// Referans: Go net/http (1.27) sunucusunun davranışı — aynı ham istekler Go'ya ve LOOK'a
// gönderilerek ölçüldü (tests/http_rfc9112_test.py tablosu). Go'dan GEVŞEK olduğumuz her yer
// Go'ya getirildi (çift Host, Host'suz 1.1, başlık adı/değeri doğrulaması, başlık sınırı, sürüm,
// istek hedefi, reddedilene yanıt). Go'dan KATI olduğumuz yerler (çıplak LF, obs-fold, CL+TE,
// chunk satırı) request smuggling savunmasıdır → katı kalır, yalnız artık 400 yanıtı yazılır.
#pragma once
#include <string>
#include <algorithm>
#include <cctype>
#include <cstring>
#include "look/http_server.h"

namespace look {

// Go: MaxHeaderBytes varsayılanı 1 MB (+4096 pay); başlık alanı sayısı 500'ü aşınca 431 (ölçüldü).
constexpr size_t HTTP_MAX_HEADER_BYTES  = 1024 * 1024 + 4096;
constexpr size_t HTTP_MAX_HEADER_FIELDS = 500;

// Karakter sınıfları — 256'lık tablo (bayt başına tek okuma; sıcak yol: her istekte her başlık baytı).
//   1 = tchar (RFC 9110 §5.6.2: başlık adı, yöntem)
//   2 = alan değeri baytı (HTAB dışında CTL yok; ≥0x80 serbest — Go)
//   4 = Host baytı (Go httpguts.ValidHostHeader kümesi)
struct HttpCharTable {
    unsigned char t[256];
    constexpr HttpCharTable() : t{} {
        for (int c = 0; c < 256; ++c) {
            bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
            unsigned char v = 0;
            if (alnum) v |= 1 | 4;
            if (!((c < 0x20 && c != '\t') || c == 0x7f)) v |= 2;
            t[c] = v;
        }
        for (char c : {'!', '#', '$', '%', '&', '\'', '*', '+', '-', '.', '^', '_', '`', '|', '~'})
            t[(unsigned char)c] |= 1;
        for (char c : {'!', '$', '%', '&', '(', ')', '*', '+', ',', '-', '.', ':', ';', '=', '[', '\'', ']', '_', '~'})
            t[(unsigned char)c] |= 4;
    }
};
inline constexpr HttpCharTable HTTP_CHARS{};

inline bool http_tchar(unsigned char c) { return HTTP_CHARS.t[c] & 1; }
inline bool http_span_is(const char* p, size_t n, unsigned char cls) {
    for (size_t i = 0; i < n; ++i) if (!(HTTP_CHARS.t[(unsigned char)p[i]] & cls)) return false;
    return true;
}
inline bool http_token(const std::string& s) { return !s.empty() && http_span_is(s.data(), s.size(), 1); }
// Alan değeri: HTAB dışında kontrol karakteri (NUL, CR, LF, DEL…) yok. ≥0x80 serbest (Go).
inline bool http_field_value_ok(const std::string& v) { return http_span_is(v.data(), v.size(), 2); }
// Host değeri: Go httpguts.ValidHostHeader karakter kümesi (boş değer geçerli).
inline bool http_host_ok(const std::string& h) { return http_span_is(h.data(), h.size(), 4); }

inline const char* http_status_reason(int code) {
    switch (code) {
        case 400: return "Bad Request";
        case 431: return "Request Header Fields Too Large";
        case 505: return "HTTP Version Not Supported";
        default:  return "Bad Request";
    }
}

// Ham istek baytlarını (request-line + header bloğu, "\r\n\r\n"e kadar) ayrıştır.
// Gövde okuma çağırana aittir (Content-Length / Transfer-Encoding).
// Dönüş 0 = geçerli; aksi halde yanıt durum kodu (400 / 431 / 505) — çağıran o yanıtı yazıp
// bağlantıyı kapatır (eskiden yanıtsız kapatılıyordu: istemci yalnız "bağlantı kesildi" görüyordu).
// Erişimler saldırgan-kontrollü.
inline int http_parse_request(const std::string& raw, HttpRequest& req) {
    size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) return 400;
    if (header_end + 4 > HTTP_MAX_HEADER_BYTES) return 431;

    // RFC 9112 §2.2 KATI CRLF: başlık bloğunda \r\n çifti DIŞINDA çıplak \n veya \r → 400
    // (request smuggling: parser \r\n'de böler, çıplak \n value'da korunur → gömülü ikinci CL/TE
    // yutulur; LF-toleranslı front-end onu ayrı header sayar → gövde-çerçeve desync). Go çıplak
    // LF'yi satır sonu sayar; biz katı kalırız. Ayrı bir tam tarama GEREKMEZ: satırlar "\r\n"de
    // bölündüğünden çıplak CR/LF ancak yöntem, hedef, sürüm, ad ya da değerin İÇİNDE kalabilir ve
    // her birinin doğrulaması CR/LF'yi reddeder (yöntem/ad = tchar, hedef > 0x20, sürüm sabit
    // biçim, değer = CTL yok). Eskiden ayrı bir döngü bloğu baştan sona bir kez daha tarıyordu.

    // ── İstek satırı: method SP request-target SP HTTP-version (Go: tek boşluk ayraç) ──
    // Baştaki boş satır (CRLF) Go'da olduğu gibi 400: istek satırı boş.
    size_t line_end = raw.find("\r\n");
    const std::string rl = raw.substr(0, line_end);
    size_t s1 = rl.find(' ');
    if (s1 == std::string::npos) return 400;
    size_t s2 = rl.find(' ', s1 + 1);
    if (s2 == std::string::npos) return 400;
    req.method = rl.substr(0, s1);
    std::string target = rl.substr(s1 + 1, s2 - s1 - 1);
    std::string version = rl.substr(s2 + 1);

    // Sürüm: tam olarak "HTTP/d.d" (Go ParseHTTPVersion); 1.x dışı → 505.
    if (version.size() != 8 || version.compare(0, 5, "HTTP/") != 0 ||
        version[5] < '0' || version[5] > '9' || version[6] != '.' || version[7] < '0' || version[7] > '9')
        return 400;
    if (version[5] != '1') return 505;
    // 1.1 ve üstü 1.x aynı anlamda (Go ProtoAtLeast(1,1)): keep-alive varsayılanı, Host zorunluluğu.
    req.version = (version[7] == '0') ? "HTTP/1.0" : "HTTP/1.1";
    const bool http11 = (req.version == "HTTP/1.1");

    if (!http_token(req.method)) return 400;
    if (target.empty()) return 400;
    for (unsigned char c : target) if (c < 0x21 || c == 0x7f) return 400;   // CTL / boşluk

    // ── İstek hedefi biçimleri (RFC 9112 §3.2) ─────────────────────────────────
    std::string abs_authority;   // absolute-form'un yetki bölümü (Host'un yerine geçer)
    bool absolute_form = false;
    if (target[0] == '/') {
        // origin-form
    } else if (target == "*") {
        if (req.method != "OPTIONS") return 400;          // asterisk-form yalnız OPTIONS
    } else if (req.method == "CONNECT") {
        // authority-form — LOOK vekil değil; rota eşleşmez (404), Go da işleyiciye verir
    } else {
        // absolute-form: scheme "://" authority [path]
        size_t se = target.find("://");
        if (se == std::string::npos || se == 0) return 400;
        for (size_t i = 0; i < se; ++i) {
            unsigned char c = (unsigned char)target[i];
            bool ok = std::isalpha(c) || (i > 0 && (std::isdigit(c) || c == '+' || c == '-' || c == '.'));
            if (!ok) return 400;
        }
        std::string rest = target.substr(se + 3);
        size_t pe = rest.find_first_of("/?");
        abs_authority = rest.substr(0, pe);
        size_t at = abs_authority.rfind('@');
        if (at != std::string::npos) abs_authority = abs_authority.substr(at + 1);
        if (!http_host_ok(abs_authority)) return 400;
        target = (pe == std::string::npos) ? "/" : rest.substr(pe);
        if (target[0] == '?') target = "/" + target;
        absolute_form = true;
    }

    size_t qpos = target.find('?');
    if (qpos != std::string::npos) {
        req.path         = target.substr(0, qpos);
        req.query_string = target.substr(qpos + 1);
    } else {
        req.path = target;
    }

    // ── Başlık alanları ────────────────────────────────────────────────────────
    size_t pos = line_end + 2;
    size_t fields = 0;
    bool have_host = false;
    const char* d = raw.data();
    while (pos < header_end + 2) {
        size_t end = raw.find("\r\n", pos);
        if (end == std::string::npos || end > header_end) break;
        if (++fields > HTTP_MAX_HEADER_FIELDS) return 431;
        const size_t ls = pos, le = end;   // satır [ls, le) — ara kopya yok (sıcak yol)
        pos = end + 2;
        // RFC 9112 §5.2: obs-fold (SP/HTAB ile başlayan devam satırı) istekte YASAK. Katlanmış
        // bir çerçeve başlığını (CL/TE) katı bir front-end farklı çözerse desync. Go kabul eder;
        // biz katı kalırız.
        if (ls < le && (d[ls] == ' ' || d[ls] == '\t')) return 400;
        const void* cp = std::memchr(d + ls, ':', le - ls);
        if (!cp) return 400;                                  // ':'siz satır (Go: 400)
        const size_t colon = (size_t)((const char*)cp - d);
        // Ad = token: boş ad, addaki boşluk ("X Y"), ':' öncesi boşluk ("Content-Length : 5" —
        // katı bir front-end reddedip LOOK kabul edince smuggling) hepsi 400.
        if (colon == ls || !http_span_is(d + ls, colon - ls, 1)) return 400;
        size_t vs = colon + 1, ve = le;
        while (vs < ve && (d[vs] == ' ' || d[vs] == '\t')) ++vs;
        while (ve > vs && (d[ve - 1] == ' ' || d[ve - 1] == '\t')) --ve;
        if (!http_span_is(d + vs, ve - vs, 2)) return 400;   // NUL, CR, LF, DEL, diğer CTL
        std::string key(d + ls, colon - ls);
        for (auto& ch : key) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
        std::string val(d + vs, ve - vs);
        // RFC 9112 §6.3: çift/tutarsız Content-Length request smuggling'e yol açar.
        if (key == "content-length") {
            auto ex = req.headers.find("content-length");
            if (ex != req.headers.end() && ex->second != val) return 400;
        }
        // RFC 9112 §3.2: Host tam bir kez (Go: "too many Host headers" → 400). İki Host'un hangisinin
        // kullanılacağı vekil ile LOOK arasında farklı seçilirse sanal-host / önbellek zehirlenmesi.
        if (key == "host") {
            if (have_host) return 400;
            have_host = true;
            if (!http_host_ok(val)) return 400;
        }
        req.headers[std::move(key)] = std::move(val);
    }

    // HTTP/1.1'de Host zorunlu (RFC 9112 §3.2, Go "missing required Host header").
    if (http11 && !have_host && req.method != "CONNECT") return 400;
    // absolute-form'da Host başlığı yok sayılır, yetki bölümü kullanılır (RFC 9112 §3.2.2, Go r.Host).
    if (absolute_form && !abs_authority.empty()) req.headers["host"] = abs_authority;

    // WebSocket upgrade
    auto it_up  = req.headers.find("upgrade");
    auto it_key = req.headers.find("sec-websocket-key");
    if (it_up != req.headers.end()) {
        std::string up = it_up->second;
        std::transform(up.begin(), up.end(), up.begin(), ::tolower);
        if (up.find("websocket") != std::string::npos) {
            req.upgrade_websocket = true;
            if (it_key != req.headers.end()) req.ws_key = it_key->second;
        }
    }

    // SSE detection
    auto it_acc = req.headers.find("accept");
    if (it_acc != req.headers.end()) {
        if (it_acc->second.find("text/event-stream") != std::string::npos)
            req.upgrade_sse = true;
    }

    return 0;
}

} // namespace look
