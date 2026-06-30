#include "look/web.h"
#include <cstdlib>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <cstring>
#include <cstdint>
#include <iomanip>
#include <filesystem>
#ifdef _WIN32
#  include <bcrypt.h>
#  pragma comment(lib, "bcrypt.lib")
#else
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace look_internal {

// ── SHA-256 (standalone, zero deps) ──────────────────────────────────────────

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32-n)); }

static std::string sha256(const std::string& data) {
    uint32_t h[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };
    std::vector<uint8_t> msg(data.begin(), data.end());
    uint64_t bit_len = (uint64_t)data.size() * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back((uint8_t)(bit_len >> (i*8)));

    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32_t)msg[off+i*4]<<24)|((uint32_t)msg[off+i*4+1]<<16)|
                   ((uint32_t)msg[off+i*4+2]<<8)|(uint32_t)msg[off+i*4+3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr32(w[i-15],7)^rotr32(w[i-15],18)^(w[i-15]>>3);
            uint32_t s1 = rotr32(w[i-2],17)^rotr32(w[i-2],19)^(w[i-2]>>10);
            w[i] = w[i-16]+s0+w[i-7]+s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1=rotr32(e,6)^rotr32(e,11)^rotr32(e,25);
            uint32_t ch=(e&f)^((~e)&g);
            uint32_t temp1=hh+S1+ch+K256[i]+w[i];
            uint32_t S0=rotr32(a,2)^rotr32(a,13)^rotr32(a,22);
            uint32_t maj=(a&b)^(a&c)^(b&c);
            uint32_t temp2=S0+maj;
            hh=g; g=f; f=e; e=d+temp1;
            d=c; c=b; b=a; a=temp1+temp2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
    }
    std::ostringstream oss;
    for (int i = 0; i < 8; ++i)
        oss << std::hex << std::setfill('0') << std::setw(8) << h[i];
    return oss.str();
}

// ── Magic Byte → MIME ─────────────────────────────────────────────────────────

static std::string detect_mime(const std::string& data) {
    auto starts = [&](std::initializer_list<uint8_t> sig) {
        if (data.size() < sig.size()) return false;
        size_t i = 0;
        for (auto b : sig) if ((uint8_t)data[i++] != b) return false;
        return true;
    };
    if (starts({0xFF,0xD8,0xFF}))                   return "image/jpeg";
    if (starts({0x89,0x50,0x4E,0x47,0x0D,0x0A}))   return "image/png";
    if (starts({0x47,0x49,0x46,0x38}))              return "image/gif";
    if (starts({0x52,0x49,0x46,0x46}) &&
        data.size()>=12 && data.substr(8,4)=="WEBP") return "image/webp";
    if (starts({0x25,0x50,0x44,0x46}))              return "application/pdf";
    if (starts({0x50,0x4B,0x03,0x04}))              return "application/zip";
    if (starts({0x1F,0x8B}))                        return "application/gzip";
    // SVG — text based, check content
    if (data.find("<svg") != std::string::npos ||
        data.find("<?xml") != std::string::npos)    return "image/svg+xml";
    return "application/octet-stream";
}

// ── Random hex filename ───────────────────────────────────────────────────────

// CSPRNG tabanlı geçici dosya adı (rand() kaldırıldı — tahmin edilebilir rastgelelik düzeltmesi)
static std::string random_hex(int bytes = 16) {
    std::vector<unsigned char> buf(bytes);
#ifdef _WIN32
    BCryptGenRandom(nullptr, buf.data(), (ULONG)bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) { read(fd, buf.data(), bytes); close(fd); }
#endif
    std::ostringstream oss;
    for (int i = 0; i < bytes; ++i)
        oss << std::hex << std::setfill('0') << std::setw(2) << (int)buf[i];
    return oss.str();
}

// ── MIME → extension ─────────────────────────────────────────────────────────

static std::string mime_to_ext(const std::string& mime) {
    if (mime == "image/jpeg")            return ".jpg";
    if (mime == "image/png")             return ".png";
    if (mime == "image/gif")             return ".gif";
    if (mime == "image/webp")            return ".webp";
    if (mime == "image/svg+xml")         return ".svg";
    if (mime == "application/pdf")       return ".pdf";
    if (mime == "application/zip")       return ".zip";
    if (mime == "application/gzip")      return ".gz";
    return ".bin";
}

} // namespace look_internal

namespace look {

void WebContext::init_from_cgi() {
    // Method
    const char* m = std::getenv("REQUEST_METHOD");
    method = m ? m : "GET";

    // Path resolution — iki mod:
    //
    // 1. Apache Action (CGI): PATH_INFO = /index.lk/menu/route
    //    .lk prefix'ini soy, geri kalan route path
    //
    // 2. mod_proxy_fcgi RewriteRule [P] (FastCGI): PATH_INFO bos,
    //    REQUEST_URI = /menu/route (query string dahil degil)
    //    SCRIPT_NAME = /index.lk (ekledigimiz hedef)
    //    Gercek route = REQUEST_URI - SCRIPT_NAME prefix'i (varsa)
    {
        const char* pi_env = std::getenv("PATH_INFO");
        std::string pi_str = pi_env ? pi_env : "";

        if (!pi_str.empty()) {
            // Mod 1: CGI Action — PATH_INFO icinde .lk varsa soyu
            size_t lk = pi_str.find(".lk");
            if (lk != std::string::npos) {
                size_t slash = pi_str.find('/', lk + 3);
                path = (slash != std::string::npos) ? pi_str.substr(slash) : "/";
            } else {
                path = pi_str;
            }
        } else {
            // Mod 2: FastCGI (mod_proxy_fcgi RewriteRule [P]) — REQUEST_URI kullan
            // REQUEST_URI = /menu/burger-cafe (orijinal URL, routing icin dogru)
            // SCRIPT_NAME = Apache'nin atadigi: /index.lk ise soyu, degilse kullanma
            const char* ru = std::getenv("REQUEST_URI");
            if (ru && *ru) {
                std::string uri(ru);
                // Query string'i kopar (?...) — sadece path
                size_t q = uri.find('?');
                if (q != std::string::npos) uri = uri.substr(0, q);
                // Sadece SCRIPT_NAME /index.lk gibi .lk ile bitiyorsa soy
                // /menu/burger-cafe gibi route path'lerini soyma
                const char* sn = std::getenv("SCRIPT_NAME");
                if (sn && *sn) {
                    std::string script_name(sn);
                    bool sn_is_lk = script_name.size() > 3 &&
                                    script_name.substr(script_name.size()-3) == ".lk";
                    if (sn_is_lk && uri.rfind(script_name, 0) == 0)
                        uri = uri.substr(script_name.size());
                }
                path = uri.empty() ? "/" : uri;
            } else {
                path = "/";
            }
        }
    }

    // Query string
    const char* qs = std::getenv("QUERY_STRING");
    query_string = qs ? qs : "";
    get_params = parse_query(query_string);

    // Body (POST)
    const char* cl = std::getenv("CONTENT_LENGTH");
    if (cl && *cl) {
        int len = std::atoi(cl);
        if (len > 0 && len < 10 * 1024 * 1024) { // max 10MB
            body.resize(len);
            std::cin.read(&body[0], len);
        }
    }

    // POST params
    const char* ct = std::getenv("CONTENT_TYPE");
    content_type = ct ? ct : "";
    if (content_type.find("application/x-www-form-urlencoded") != std::string::npos)
        post_params = parse_query(body);
    else if (content_type.find("multipart/form-data") != std::string::npos) {
        // boundary=... kısmını çıkar
        size_t bpos = content_type.find("boundary=");
        if (bpos != std::string::npos)
            parse_multipart(content_type.substr(bpos + 9));
    }

    // Remote addr
    const char* ra = std::getenv("REMOTE_ADDR");
    remote_addr = ra ? ra : "";

    // Cookies
    const char* ck = std::getenv("HTTP_COOKIE");
    if (ck) cookies_in = parse_cookies(ck);
}

std::string WebContext::url_decode(const std::string& s) {
    std::string result;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            result += ' ';
        } else if (s[i] == '%' && i + 2 < s.size()) {
            try {
                int h = std::stoi(s.substr(i + 1, 2), nullptr, 16);
                result += (char)h;
                i += 2;
            } catch (...) {
                result += s[i];  // geçersiz %xx → literal '%' olarak bırak
            }
        } else {
            result += s[i];
        }
    }
    return result;
}

std::map<std::string, std::string> WebContext::parse_query(const std::string& qs) {
    std::map<std::string, std::string> result;
    std::istringstream ss(qs);
    std::string pair;
    while (std::getline(ss, pair, '&')) {
        auto eq = pair.find('=');
        if (eq == std::string::npos) {
            result[url_decode(pair)] = "";
        } else {
            result[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
        }
    }
    return result;
}

std::map<std::string, std::string> WebContext::parse_cookies(const std::string& raw) {
    std::map<std::string, std::string> result;
    std::istringstream ss(raw);
    std::string item;
    while (std::getline(ss, item, ';')) {
        // trim
        size_t s = item.find_first_not_of(" \t");
        if (s == std::string::npos) continue;
        item = item.substr(s);
        auto eq = item.find('=');
        if (eq == std::string::npos) continue;
        result[item.substr(0, eq)] = item.substr(eq + 1);
    }
    return result;
}

void WebContext::set_status(int code) {
    status_code = code;
    switch (code) {
        case 200: status_text = "OK"; break;
        case 201: status_text = "Created"; break;
        case 204: status_text = "No Content"; break;
        case 301: status_text = "Moved Permanently"; break;
        case 302: status_text = "Found"; break;
        case 400: status_text = "Bad Request"; break;
        case 401: status_text = "Unauthorized"; break;
        case 403: status_text = "Forbidden"; break;
        case 404: status_text = "Not Found"; break;
        case 405: status_text = "Method Not Allowed"; break;
        case 422: status_text = "Unprocessable Entity"; break;
        case 500: status_text = "Internal Server Error"; break;
        default:  status_text = "Unknown"; break;
    }
}

void WebContext::parse_multipart(const std::string& boundary) {
    // RFC 2046 multipart — delimiter: "--" + boundary
    std::string delim = "--" + boundary;
    std::string end_delim = delim + "--";

    size_t pos = body.find(delim);
    if (pos == std::string::npos) return;

    while (true) {
        pos += delim.size();
        if (pos + 2 <= body.size() && body.substr(pos, 2) == "--") break;
        if (pos < body.size() && body[pos] == '\r') pos++;
        if (pos < body.size() && body[pos] == '\n') pos++;

        // Header bölümünü oku (\r\n\r\n'e kadar)
        size_t header_end = body.find("\r\n\r\n", pos);
        if (header_end == std::string::npos) break;
        std::string raw_headers = body.substr(pos, header_end - pos);
        pos = header_end + 4;

        // Content-Disposition header'ını parse et
        std::string field_name, filename;
        {
            std::istringstream hs(raw_headers);
            std::string line;
            while (std::getline(hs, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                // Content-Disposition: form-data; name="field"; filename="file.jpg"
                if (line.find("Content-Disposition:") != std::string::npos) {
                    auto extract = [&](const std::string& key) {
                        size_t k = line.find(key + "=\"");
                        if (k == std::string::npos) return std::string{};
                        k += key.size() + 2;
                        size_t e = line.find('"', k);
                        return e != std::string::npos ? line.substr(k, e-k) : std::string{};
                    };
                    field_name = extract("name");
                    filename   = extract("filename");
                }
            }
        }

        // Part gövdesini bul (sonraki delimiter'a kadar)
        size_t part_end = body.find("\r\n" + delim, pos);
        if (part_end == std::string::npos) break;
        std::string part_data = body.substr(pos, part_end - pos);
        pos = part_end + 2;

        if (!filename.empty() && !field_name.empty()) {
            // Dosya parçası — temp dosyaya yaz
            UploadedFile uf;
            uf.field_name = field_name;
            uf.size       = part_data.size();
            uf.mime       = look_internal::detect_mime(part_data);
            uf.sha256     = look_internal::sha256(part_data);

            // Temp dosya yolu
            std::string tmp_dir;
            const char* td = std::getenv("TEMP");
            if (!td) td = std::getenv("TMP");
            if (!td) td = "/tmp";
            tmp_dir = td;
            std::string ext = look_internal::mime_to_ext(uf.mime);
            std::string tmp_name = look_internal::random_hex(16) + ext;
            uf.temp_path = tmp_dir + "/" + tmp_name;

            // Diske yaz
            std::ofstream out(uf.temp_path, std::ios::binary);
            if (out) {
                out.write(part_data.data(), (std::streamsize)part_data.size());
                uf.valid = true;
            }
            uploaded_files[field_name] = std::move(uf);
        } else if (!field_name.empty()) {
            // Normal form alanı
            post_params[field_name] = part_data;
        }

        // Sonraki delimiter'ı bul
        size_t next = body.find(delim, pos);
        if (next == std::string::npos) break;
        pos = next;
    }
}

std::string WebContext::build_headers() const {
    std::string out = "Status: " + std::to_string(status_code) + " " + status_text + "\r\n";
    // Content-Type default
    bool has_ct = false;
    for (auto& [k, v] : headers_out) {
        out += k + ": " + v + "\r\n";
        if (k == "Content-Type") has_ct = true;
    }
    if (!has_ct)
        out += "Content-Type: text/html; charset=utf-8\r\n";
    out += "\r\n";
    return out;
}

} // namespace look
