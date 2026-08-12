// HTTP/1.1 istek/başlık saf ayrıştırıcısı — socket'ten AYRI (fuzz + test hedefi).
// "Dikişi aç": ham istek tamponu → HttpRequest yorumlama mantığı burada; I/O (recv,
// timeout, chunked gövde) http_server.cpp'de. Tek tanım (http_server.cpp bunu include
// eder) → drift yok. Bu yüzey PRE-AUTH: ağdaki herkes bu baytları besler.
#pragma once
#include <string>
#include <algorithm>
#include "look/http_server.h"

namespace look {

// Ham istek baytlarını (request-line + header bloğu, "\r\n\r\n"e kadar) ayrıştır.
// Gövde okuma çağırana aittir (Content-Length / Transfer-Encoding). false → istek
// reddedildi (malformed / smuggling koruması). Erişimler saldırgan-kontrollü.
inline bool http_parse_request(const std::string& raw, HttpRequest& req) {
    size_t pos = 0;

    size_t line_end = raw.find("\r\n");
    if (line_end == std::string::npos) return false;

    std::string req_line = raw.substr(0, line_end);
    size_t s1 = req_line.find(' ');
    size_t s2 = req_line.rfind(' ');
    if (s1 == std::string::npos || s1 == s2) return false;

    req.method  = req_line.substr(0, s1);
    req.version = req_line.substr(s2 + 1);
    std::string full_path = req_line.substr(s1 + 1, s2 - s1 - 1);

    size_t qpos = full_path.find('?');
    if (qpos != std::string::npos) {
        req.path         = full_path.substr(0, qpos);
        req.query_string = full_path.substr(qpos + 1);
    } else {
        req.path = full_path;
    }

    pos = line_end + 2;

    size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) return false;

    // RFC 9112 §2.2 KATI CRLF: başlık bloğunda \r\n çifti DIŞINDA çıplak \n veya \r
    // → request smuggling (parser \r\n'de böler, çıplak \n value'da korunur → gömülü
    // ikinci CL/TE yutulur; LF-toleranslı front-end onu ayrı header sayar → gövde-çerçeve
    // desync). obs-fold/çift-CL/boşluk zaten sıkılaştırılmış; çıplak-LF eksikti. Katı ret.
    for (size_t i = 0; i < header_end; ++i) {
        if (raw[i] == '\r') { if (i + 1 >= raw.size() || raw[i + 1] != '\n') return false; }
        else if (raw[i] == '\n') { if (i == 0 || raw[i - 1] != '\r') return false; }
    }

    while (pos < header_end) {
        size_t end = raw.find("\r\n", pos);
        if (end == std::string::npos || end > header_end) break;
        std::string line = raw.substr(pos, end - pos);
        // RFC 7230 §3.2.4: obs-fold (SP/HTAB ile başlayan devam satırı) istekte
        // YASAK. Eskiden colon içermediği için sessizce DÜŞÜRÜLÜYORDU → katlanmış
        // bir framing header'ı (CL/TE) katı bir front-end farklı çözerse desync
        // (request smuggling). Katı ret.
        if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) return false;
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            while (!val.empty() && (val[0] == ' ' || val[0] == '\t')) val.erase(0, 1);
            while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.pop_back();
            // RFC 7230 §3.2.4: field-name ile ':' arasında boşluk YASAK. Eskiden
            // trailing boşluk sessizce kırpılıyordu → "Content-Length : 5" geçerli
            // CL sayılıyordu; katı bir front-end onu reddedip LOOK kabul edince
            // gövde-çerçeve desync'i (request smuggling). Katı ret.
            if (!key.empty() && (key.back() == ' ' || key.back() == '\t')) return false;
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            // RFC 7230 §3.3.3: çift/tutarsız Content-Length request smuggling'e yol
            // açar (front-end ilk değeri, LOOK sonuncuyu kullanırsa desync). Çakışan
            // ikinci CL → isteği reddet.
            if (key == "content-length") {
                auto ex = req.headers.find("content-length");
                if (ex != req.headers.end() && ex->second != val) return false;
            }
            req.headers[key] = val;
        }
        pos = end + 2;
    }

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

    return true;
}

} // namespace look
