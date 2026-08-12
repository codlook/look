#include "look/http_server.h"
#include "look/http_parse.h"   // saf parse_request seam (fuzz + test hedefi)
#include "look/websocket.h"
#include "look/sse.h"
#include "look/event_loop.h"
#include "look/fiber.h"

#include <sstream>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <vector>
#include <unordered_map>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <algorithm>

#if defined(__linux__)
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#elif defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#  define close(x)   closesocket(x)
#  define ssize_t    int
#  define recv(f,b,l,fl) recv((SOCKET)(f),(b),(int)(l),(fl))
#  define send(f,b,l,fl) send((SOCKET)(f),(b),(int)(l),(fl))
#endif

#if defined(__linux__)
#  include <poll.h>
#endif

namespace look {

// ── HTTP/1.1 parser ───────────────────────────────────────────────────────────
// Saf ayrıştırma mantığı look/http_parse.h'ye taşındı (fuzz + test hedefi, drift
// yok). Buradaki thin wrapper mevcut çağrı yerlerini korur.

static bool parse_request(const std::string& raw, HttpRequest& req) {
    return http_parse_request(raw, req);
}

// ── HTTP response builder ─────────────────────────────────────────────────────

std::string HttpResponse::build() const {
    std::ostringstream out;
    out << "HTTP/1.1 " << status_code << " " << status_text << "\r\n";
    // Defense-in-depth: CR/LF strip — HTTP response splitting engellenir
    // (giriş katmanı response::header/redirect/cookie zaten sanitize eder).
    auto strip_crlf = [](const std::string& s) {
        size_t cut = s.find_first_of("\r\n");
        return cut == std::string::npos ? s : s.substr(0, cut);
    };
    for (auto& [k, v] : headers) out << strip_crlf(k) << ": " << strip_crlf(v) << "\r\n";
    // Her çerez ayrı Set-Cookie satırı (std::map birden fazla aynı-anahtarı tutamaz)
    for (auto& c : set_cookies) out << "Set-Cookie: " << strip_crlf(c) << "\r\n";
    out << "Content-Length: " << body.size() << "\r\n";
    out << "Connection: " << (keep_alive ? "keep-alive" : "close") << "\r\n";
    out << "\r\n";
    out << body;
    return out.str();
}

// ── Worker thread pool ────────────────────────────────────────────────────────

class WorkerPool {
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_ = false;
public:
    explicit WorkerPool(int n) {
        for (int i = 0; i < n; ++i) {
            workers_.emplace_back([this] {
                for (;;) {
                    std::function<void()> fn;
                    {
                        std::unique_lock<std::mutex> lk(mtx_);
                        cv_.wait(lk, [this]{ return stop_ || !queue_.empty(); });
                        if (stop_ && queue_.empty()) return;
                        fn = std::move(queue_.front());
                        queue_.pop();
                    }
                    fn();
                }
            });
        }
    }
    void submit(std::function<void()> fn) {
        { std::lock_guard<std::mutex> lk(mtx_); queue_.push(std::move(fn)); }
        cv_.notify_one();
    }
    ~WorkerPool() {
        { std::lock_guard<std::mutex> lk(mtx_); stop_ = true; }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }
};

// ── HttpServer::Impl ──────────────────────────────────────────────────────────

static void send_ws_reject(int fd, const std::string& reason) {
    std::string resp =
        "HTTP/1.1 403 Forbidden\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "Content-Length: " + std::to_string(reason.size()) + "\r\n"
        "\r\n" + reason;
#ifdef _WIN32
    ::send(fd, resp.data(), (int)resp.size(), 0);
#else
    ::send(fd, resp.data(), resp.size(), MSG_NOSIGNAL);
#endif
}

struct HttpServer::Impl {
    int  port    = 0;
    int  workers = 0;
    HttpHandler  handler;
    WsHandler    ws_handler;
    SseHandler   sse_handler;
    // CSWSH koruması (gerçek davranış — handle_ws_upgrade): BOŞ = güvenli-varsayılan
    // SAME-ORIGIN (Origin varsa Host'la eşleşmeli; cross-origin tarayıcı isteği=CSWSH
    // vektörü reddedilir; Origin'siz backend/wscat izinli). DOLU (LOOK_WS_ORIGINS) = yalnız
    // listedeki origin'ler (Origin zorunlu). NOT: "boş=tüm originlere izin" DEĞİL (eski
    // yorum yanıltıcıydı; same-origin eklenmeden önceki davranışı anlatıyordu).
    std::vector<std::string> allowed_origins;

    int  server_fd = -1;
    std::atomic<bool> running{false};

    std::unique_ptr<WorkerPool> pool;

    // Event loop — yalnızca WS/SSE async frame okuma için
    std::unique_ptr<EventLoop> loop;
    std::thread loop_thread;

    // WS state
    std::unordered_map<int, std::shared_ptr<WsConnection>> ws_clients;
    std::unordered_map<int, std::string>                   ws_bufs;
    std::mutex ws_mtx;

    // SSE state
    std::unordered_map<int, std::shared_ptr<SseConnection>> sse_clients;
    std::mutex sse_mtx;

    // Maksimum request body boyutu (byte) — LOOK_MAX_BODY_SIZE ile ayarlanır,
    // varsayılan 10 MB. Sınırsız body bellek tüketimi DoS'unu engeller.
    static size_t max_body_size() {
        static const size_t v = []() -> size_t {
            const char* e = std::getenv("LOOK_MAX_BODY_SIZE");
            if (e && *e) { long long n = std::atoll(e); if (n > 0) return (size_t)n; }
            return 10 * 1024 * 1024;
        }();
        return v;
    }

    // Header-only kısa yanıt — body okumadan önce reddetme yolları için.
    static void send_simple(int fd, int code, const char* reason) {
        std::string r = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n"
                        "Content-Length: 0\r\nConnection: close\r\n\r\n";
#ifdef _WIN32
        ::send(fd, r.data(), (int)r.size(), 0);
#else
        ssize_t n = ::send(fd, r.data(), r.size(), MSG_NOSIGNAL);
        (void)n;
#endif
    }

    // ── Fiber-aware recv (Go netpoller) ──────────────────────────────────────
    // Fiber içindeyse ve soket veri yoksa BLOKLAMAZ: wait_readable ile yield eder,
    // scheduler başka fiber (goroutine) koşturur; veri gelince epoll resume eder.
    // Eskiden düz ::recv() worker thread'ini bloklyordu → keep-alive'da sonraki
    // isteği beklerken tüm worker kilitleniyor, diğer bağlantılar açlıkta kalıyordu
    // (fiber dispatch'in c=100 hang'inin KÖK NEDENİ). Fiber dışında düz blocking recv.
    static ssize_t fiber_aware_recv(int fd, char* buf, size_t len) {
#ifdef __linux__
        look::FiberScheduler* sched = look::get_thread_scheduler();
        look::Fiber*          cur   = look::Fiber::current();
        if (sched && cur) {
            auto self = cur->shared_from_this_fiber();
            while (true) {
                ssize_t r = ::recv(fd, buf, len, MSG_DONTWAIT);
                if (r >= 0) return r;
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Veri yok → yield. wait_readable_tmo epoll'a park eder + 15s idle
                    // sınırı (D Fix #2): keep-alive'da istemci sonraki isteği hiç
                    // göndermezse fiber SÜRESİZ askıda kalmaz — timeout'ta 0 döneriz
                    // (peer-kapandı gibi) → bağlantı kapanır, fiber biter. ab -k
                    // wind-down stall'ının ve kapanışta fiber birikmesinin çözümü.
                    if (self) {
                        int w = sched->wait_readable_tmo(self, fd, 15000);
                        if (w == 1) continue;      // okunabilir — recv'i tekrar dene
                        if (w == 0) return 0;      // idle timeout → bağlantıyı kapat
                    }
                    return ::recv(fd, buf, len, 0);  // epoll yok → blocking fallback
                }
                return r;  // gerçek hata
            }
        }
#endif
        return ::recv(fd, buf, len, 0);  // fiber dışı: blocking (mevcut davranış)
    }

    // Transfer-Encoding: chunked gövdeyi çöz (RFC 7230 §4.1).
    // buf: header'lar + gövdenin ilk parçası; start: gövde offset'i.
    // Çözülen gövde out'a yazılır. Body cap ve malformed kontrolleri dahil.
    // false → istek reddedildi (yanıt gönderildi), caller fd'yi kapatmalı.
    static bool read_chunked_body(int fd, std::string& buf, size_t start,
                                  char* tmp, size_t tmp_sz, std::string& out) {
        std::string stream = buf.substr(start);   // gövde baytları (henüz çözülmemiş)
        size_t p = 0;
        const size_t cap    = max_body_size();
        const size_t raw_lim = cap + 1024 * 1024; // ham akış tavanı (chunk header payı)

        auto refill = [&]() -> bool {
            ssize_t r = fiber_aware_recv(fd, tmp, tmp_sz);
            if (r <= 0) return false;
            if (stream.size() + (size_t)r > raw_lim) return false; // akış patlaması
            stream.append(tmp, (size_t)r);
            return true;
        };

        while (true) {
            // 1) chunk boyutu satırını al (hex, ; ile chunk-ext olabilir)
            size_t nl;
            while ((nl = stream.find("\r\n", p)) == std::string::npos) {
                if (stream.size() - p > 64) { send_simple(fd, 400, "Bad Request"); return false; }
                if (!refill()) { send_simple(fd, 400, "Bad Request"); return false; }
            }
            std::string sizeline = stream.substr(p, nl - p);
            size_t semi = sizeline.find(';');            // chunk-ext'i at
            if (semi != std::string::npos) sizeline = sizeline.substr(0, semi);
            // hex parse (katı)
            if (sizeline.empty() ||
                sizeline.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
                send_simple(fd, 400, "Bad Request"); return false;
            }
            size_t chunk_sz = 0;
            try { chunk_sz = std::stoull(sizeline, nullptr, 16); }
            catch (...) { send_simple(fd, 400, "Bad Request"); return false; }
            p = nl + 2;

            if (chunk_sz == 0) break;                    // son chunk
            // Taşma-güvenli: `out.size() + chunk_sz` chunk_sz ~2^64 (hex
            // fffffffffffffffb) ile wrap yapıp cap kontrolünü atlayabilirdi.
            // out.size() <= cap invariantı gereği `cap - out.size()` güvenli;
            // bu ayrıca chunk_sz'yi ≤cap'e sınırlayıp 280'deki `p+chunk_sz+2`
            // taşmasını da önler.
            if (chunk_sz > cap - out.size()) { send_simple(fd, 413, "Payload Too Large"); return false; }

            // 2) chunk verisi + kapanış CRLF'i gelene kadar oku
            while (stream.size() < p + chunk_sz + 2) {
                if (!refill()) { send_simple(fd, 400, "Bad Request"); return false; }
            }
            out.append(stream, p, chunk_sz);
            p += chunk_sz;
            if (stream.compare(p, 2, "\r\n") != 0) { send_simple(fd, 400, "Bad Request"); return false; }
            p += 2;
        }
        // trailer başlıklarını yut: boş satıra (\r\n) kadar. Keep-alive'da
        // sonraki isteğin sınırı doğru olsun diye akıştan tüketilir.
        while (true) {
            size_t nl2 = stream.find("\r\n", p);
            if (nl2 == std::string::npos) { if (!refill()) break; else continue; }
            if (nl2 == p) { p += 2; break; }             // boş satır → gövde bitti
            p = nl2 + 2;                                  // trailer satırını atla
        }
        return true;
    }

    // ── Blocking HTTP bağlantı yöneticisi — worker thread'de çalışır ──────────
    //
    // Her bağlantı için:
    //   1. blocking recv() → headers tamamlanana dek oku
    //   2. parse → WS/SSE upgrade tespiti
    //   3. HTTP: handler → send() → keep-alive döngüsü
    //   4. WS/SSE: event loop'a devret, worker serbest kalır
    //
    // Event loop yok — Apache ile aynı model.
    void handle_connection(int fd, std::string remote_addr = "") {
        // Boşta timeout — istemci N ms yanıt vermezse kapat (default 30s). LOOK_HTTP_IDLE_MS
        // ile ayarlanabilir (test hızı için). PLATFORM-DOĞRU TİP: Windows SO_RCVTIMEO bir
        // DWORD-milisaniye bekler, POSIX timeval bekler. ESKİ BUG: Windows'a timeval{30,0}
        // geçiliyordu → Winsock ilk DWORD'ü (=30) okuyup 30sn'yi 30ms'ye çeviriyordu (idle
        // keep-alive Win'de ~30ms'de düşüyordu). Ampirik ölçüldü (~34ms), düzeltildi.
        static const long idle_ms = []{
            const char* e = std::getenv("LOOK_HTTP_IDLE_MS");
            return (e && *e) ? std::atol(e) : 30000L;
        }();
#ifdef _WIN32
        DWORD tv = (DWORD)idle_ms;
#else
        timeval tv{ (time_t)(idle_ms/1000), (suseconds_t)((idle_ms%1000)*1000) };
#endif
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#ifndef _WIN32
        // Nagle kapalı — küçük paketler hemen gönderilir
        int nd = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
#endif

        char tmp[65536];

        // Slowloris savunması: bir istek BAŞLADIKTAN (ilk bayt) sonra header'lar
        // toplam bu süre içinde tamamlanmalı. idle timeout (15s/30s) her baytta
        // sıfırlandığı için tek başına yetmez — yavaş-damla (idle altında bayt/6s)
        // bağlantıyı süresiz tutup pool modunda worker thread'i tüketir (DoS).
        // LOOK_HEADER_TIMEOUT ms ile ayarlanır (varsayılan 15s). Keep-alive'da
        // istekLER ARASI boşta bekleme buna dahil DEĞİL (timer ilk baytta başlar).
        static const auto HDR_TMO = []() -> std::chrono::milliseconds {
            const char* e = std::getenv("LOOK_HEADER_TIMEOUT");
            long ms = (e && *e) ? std::atol(e) : 15000;
            if (ms <= 0) ms = 15000;
            return std::chrono::milliseconds(ms);
        }();

        while (true) {
            // Headers gelene kadar blocking oku
            std::string buf;
            std::chrono::steady_clock::time_point rd_start{};   // ilk baytta set
            while (buf.find("\r\n\r\n") == std::string::npos) {
                ssize_t r = fiber_aware_recv(fd, tmp, sizeof(tmp));
                if (r <= 0) { ::close(fd); return; }
                if (rd_start == std::chrono::steady_clock::time_point{})
                    rd_start = std::chrono::steady_clock::now();
                buf.append(tmp, (size_t)r);
                if (buf.size() > 2 * 1024 * 1024) { ::close(fd); return; }
                if (std::chrono::steady_clock::now() - rd_start > HDR_TMO) {
                    send_simple(fd, 408, "Request Timeout"); ::close(fd); return;
                }
            }

            HttpRequest req;
            req.remote_addr = remote_addr;
            if (!parse_request(buf, req)) { ::close(fd); return; }

            size_t header_end  = buf.find("\r\n\r\n") + 4;
            size_t body_in_buf = buf.size() - header_end;
            auto clen_it = req.headers.find("content-length");
            auto te_it   = req.headers.find("transfer-encoding");

            // HTTP Request Smuggling koruması: Content-Length ve Transfer-Encoding
            // aynı anda gelirse istek belirsizdir (RFC 7230 §3.3.3) — katı ret.
            if (te_it != req.headers.end() && clen_it != req.headers.end()) {
                send_simple(fd, 400, "Bad Request"); ::close(fd); return;
            }

            // ── Transfer-Encoding: chunked gövde çözme ────────────────────────
            if (te_it != req.headers.end()) {
                std::string te = te_it->second;
                std::transform(te.begin(), te.end(), te.begin(), ::tolower);
                // RFC 7230 §3.3.1: TE token-listesidir; "chunked" son coding olmalı.
                // Eskiden `find("chunked")` SUBSTRING eşliyordu → "xchunked",
                // "chunkedx", "chunked, gzip" hepsi chunked sanılıyordu (ölçüldü:
                // 200/blen=5). Bir front-end bunları farklı çerçevelerse desync
                // (request smuggling). LOOK yalnız chunked destekler → TAM eşleşme:
                // değer birebir "chunked" değilse reddet (gzip/compress zaten çözülemez).
                if (te != "chunked") {
                    send_simple(fd, 400, "Bad Request"); ::close(fd); return;
                }
                if (!read_chunked_body(fd, buf, header_end, tmp, sizeof(tmp), req.body)) {
                    // malformed chunk veya body cap aşımı → read_chunked_body yanıtı gönderdi
                    ::close(fd); return;
                }
            } else if (clen_it != req.headers.end()) {
                // Content-Length güvenli parse — bozuk/taşan değer worker'ı
                // düşürmesin (std::stoul exception fırlatıyordu = DoS).
                size_t content_len = 0;
                {
                    const std::string& cl = clen_it->second;
                    if (cl.empty() || cl.size() > 19 ||
                        cl.find_first_not_of("0123456789") != std::string::npos) {
                        send_simple(fd, 400, "Bad Request"); ::close(fd); return;
                    }
                    try { content_len = std::stoull(cl); }
                    catch (...) { send_simple(fd, 400, "Bad Request"); ::close(fd); return; }
                }
                // Maksimum body boyutu — sınırsız body bellek tüketimi DoS'u.
                // LOOK_MAX_BODY_SIZE (byte) ile ayarlanır, varsayılan 10 MB.
                if (content_len > max_body_size()) {
                    send_simple(fd, 413, "Payload Too Large"); ::close(fd); return;
                }
                if (body_in_buf > 0)
                    req.body = buf.substr(header_end,
                                          std::min(body_in_buf, content_len));
                while (req.body.size() < content_len) {
                    ssize_t r = fiber_aware_recv(fd, tmp, sizeof(tmp));
                    if (r <= 0) { ::close(fd); return; }
                    // Content-Length KESİN üst sınır (55. bug): body ayrı segmentte
                    // gelip son recv fazla bayt getirirse (pipeline'daki sonraki
                    // isteğin başlangıcı), o baytları body'ye YUTMA — yoksa gövde
                    // CL'yi aşar (app bozuk gövde görür + smuggling-bitişik: sonraki
                    // istek bu gövdeye karışır). İlk-buffer yolu (yukarıda) zaten
                    // min() ile kırpıyordu; bu döngü kırpmıyordu.
                    size_t need = content_len - req.body.size();
                    req.body.append(tmp, std::min((size_t)r, need));
                }
            }

            // WS upgrade → event loop'a devret, worker serbest kalır
            if (req.upgrade_websocket && ws_handler) {
                handle_ws_upgrade(fd, req);
                return;
            }

            // SSE upgrade → event loop'a devret
            if (req.upgrade_sse && sse_handler) {
                handle_sse_upgrade(fd, req);
                return;
            }

            // Keep-alive tespiti (HTTP/1.1 default = keep-alive)
            bool req_keep_alive = (req.version == "HTTP/1.1");
            {
                auto ci = req.headers.find("connection");
                if (ci != req.headers.end()) {
                    std::string cv = ci->second;
                    std::transform(cv.begin(), cv.end(), cv.begin(), ::tolower);
                    if (cv.find("close")      != std::string::npos) req_keep_alive = false;
                    if (cv.find("keep-alive") != std::string::npos) req_keep_alive = true;
                }
            }

            // Dispatch — worker thread doğrudan işler
            HttpResponse resp;
            resp.keep_alive = req_keep_alive;
            resp.headers["Content-Type"] = "application/json; charset=utf-8";
            try {
                handler(req, resp);
            } catch (...) {
                resp.status_code = 500;
                resp.status_text = "Internal Server Error";
                resp.body        = "{\"ok\":false,\"hata\":\"Sunucu hatası\"}";
                resp.keep_alive  = false;
            }

            std::string raw = resp.build();
#ifdef _WIN32
            ssize_t w = ::send(fd, raw.data(), (int)raw.size(), 0);
#else
            ssize_t w = ::send(fd, raw.data(), raw.size(), MSG_NOSIGNAL);
#endif
            if (w <= 0 || !resp.keep_alive) { ::close(fd); return; }
            // keep-alive: sonraki isteği bekle
        }
    }

    // ── WebSocket upgrade — worker thread'de çalışır ──────────────────────────
    // Origin başlığından host[:port] kısmını çıkar (scheme:// ve yolu at).
    static std::string ws_origin_host(const std::string& origin) {
        auto p = origin.find("://");
        std::string h = (p == std::string::npos) ? origin : origin.substr(p + 3);
        auto slash = h.find('/');
        if (slash != std::string::npos) h = h.substr(0, slash);
        auto colon = h.rfind(':');           // :port'u at — host bazlı karşılaştır
        if (colon != std::string::npos) h = h.substr(0, colon);
        return h;
    }

    void handle_ws_upgrade(int fd, const HttpRequest& req) {
        // CSWSH (Cross-Site WebSocket Hijacking) koruması.
        auto it = req.headers.find("origin");
        if (!allowed_origins.empty()) {
            // Açık allowlist yapılandırılmış (LOOK_WS_ORIGINS) — tam eşleşme.
            if (it == req.headers.end()) {
                send_ws_reject(fd, "403 Forbidden - Origin header gerekli");
                ::close(fd);
                return;
            }
            const std::string& origin = it->second;
            bool ok = false;
            for (const auto& allowed : allowed_origins)
                if (allowed == "*" || allowed == origin) { ok = true; break; }
            if (!ok) {
                send_ws_reject(fd, "403 Forbidden - Origin izinli değil: " + origin);
                ::close(fd);
                return;
            }
        } else if (it != req.headers.end()) {
            // Allowlist yok → güvenli varsayılan: SAME-ORIGIN uygula. Tarayıcı
            // isteği (Origin başlığı var) ancak host'u Host başlığıyla eşleşirse
            // kabul edilir; cross-origin tarayıcı isteği (asıl CSWSH vektörü)
            // reddedilir. Origin başlığı OLMAYAN istekler (backend servis, wscat
            // gibi tarayıcı-dışı client'lar) etkilenmez — çapraz-site riski yok.
            // Allow-all'a dönmek için: LOOK_WS_ORIGINS=*
            std::string oh = ws_origin_host(it->second);
            auto host_it = req.headers.find("host");
            std::string hh;
            if (host_it != req.headers.end()) {
                hh = host_it->second;                    // "host[:port]"
                auto c = hh.rfind(':');
                if (c != std::string::npos) hh = hh.substr(0, c);
            }
            if (oh.empty() || hh.empty() || oh != hh) {
                send_ws_reject(fd, "403 Forbidden - Cross-origin WebSocket reddedildi "
                                   "(same-origin varsayilan; izin icin LOOK_WS_ORIGINS)");
                ::close(fd);
                return;
            }
        }
        std::string hs = ws_handshake_101(req.ws_key);
#ifdef _WIN32
        ::send(fd, hs.data(), (int)hs.size(), 0);
#else
        ::send(fd, hs.data(), hs.size(), MSG_NOSIGNAL);
#endif
        auto conn = std::make_shared<WsConnection>(fd);
        {
            std::lock_guard<std::mutex> lk(ws_mtx);
            ws_clients[fd] = conn;
            ws_bufs[fd]    = "";
        }
        look::g_ws_registry.add(conn);

        ws_handler(conn, req);

        // fd'yi event loop'a devret: blocking → non-blocking + EPOLL_CTL_ADD
        loop->add_client(fd, [this, fd](const char* d, size_t l) {
            on_ws_data(fd, d, l);
        });
    }

    // ── SSE upgrade — worker thread'de çalışır ───────────────────────────────
    void handle_sse_upgrade(int fd, const HttpRequest& req) {
        const char* hdrs =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "X-Accel-Buffering: no\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n";
#ifdef _WIN32
        ::send(fd, hdrs, (int)strlen(hdrs), 0);
#else
        ::send(fd, hdrs, strlen(hdrs), MSG_NOSIGNAL);
#endif
        auto conn = std::make_shared<SseConnection>(fd);
        // sse::close (worker thread) → close_sse'yi loop thread'inde koştur (fd/epoll/map
        // temizliği loop'a ait). Bu köprü olmadan sse::close yalnız closed işaretliyordu →
        // kaynak istemci TCP-kopana kadar sızıyordu (ölçüldü).
        conn->request_loop_close = [this, fd]() { loop->post([this, fd]() { close_sse(fd); }); };
        {
            std::lock_guard<std::mutex> lk(sse_mtx);
            sse_clients[fd] = conn;
        }
        look::g_sse_registry.add(conn);

        sse_handler(conn, req);

        // fd'yi event loop'a devret — disconnect tespiti için
        loop->add_client(fd, [this, fd](const char* d, size_t l) {
            on_sse_data(fd, d, l);
        });
    }

    // ── SSE disconnect ────────────────────────────────────────────────────────
    void on_sse_data(int fd, const char* /*data*/, size_t len) {
        bool do_close = (len == 0);
        if (!do_close) {
            std::lock_guard<std::mutex> lk(sse_mtx);
            auto it = sse_clients.find(fd);
            do_close = (it == sse_clients.end() || it->second->closed.load());
        }
        if (do_close) { close_sse(fd); return; }
        loop->async_read(fd, [this, fd](const char* d, size_t l) {
            on_sse_data(fd, d, l);
        });
    }

    void close_sse(int fd) {
        std::shared_ptr<SseConnection> conn;
        bool owned = false;
        {
            std::lock_guard<std::mutex> lk(sse_mtx);
            auto it = sse_clients.find(fd);
            if (it != sse_clients.end()) { conn = it->second; sse_clients.erase(it); owned = true; }
        }
        // OWN-ONCE: bu fd'yi map'ten erase eden TEK çağrı temizler. İki close_sse yolu var —
        // client-FIN (on_sse_data read-event) ve sse::close (loop->post). Her ikisi de loop
        // thread'inde sıralı koşar; ilki sahiplenir, ikincisi buradan çıkar. Eski kod conn
        // bulunamayınca yine `loop->close_fd(fd)` çağırıyordu → ÇİFT close_fd = fd yeniden
        // kullanıldıysa YANLIŞ-fd kapatma. O tehlikeli dal kaldırıldı (own-once ile yapısal).
        if (!owned) return;
        look::g_sse_registry.remove(fd);
        bool fire_cb = false;
        {
            fire_cb = !conn->closed.exchange(true);
            // SAVUNMA (close_ws 0525ba7 drain'inin aynası): close_fd ÖNCESİ write_mutex'i al.
            // Handler send'i timer::every / parallel ile ERTELERSE (doğal SSE push kalıbı),
            // o callback AYRI thread'de SseConnection::send() → write_mutex altında closed'ı
            // kontrol edip ::send(fd) yapar. Drain olmadan: send() closed-kontrolü (sse.cpp:61)
            // ile ::send(63) arasındayken close_sse (event-loop) close_fd(fd) → use-after-close
            // (fd yeniden kullanıldıysa yanlış istemciye yazma). write_mutex uçuştaki send'i
            // bekletir; sonraki send closed=true görür.
            // NOT: fd-YAŞAM-SÜRESİ semantik yarışı — kernel fd üzerinde, kullanıcı-belleğinde
            // DEĞİL → TSan GÖREMEZ (pozitif kontrol iki variantta da 0, bkz [[tsan-heap-yaris-atfi]]).
            // Kanıt: kod-mantığı + threading. Şiddet DÜŞÜK-ORTA (bozulma değil, yanlış-fd yazımı).
            std::lock_guard<std::mutex> wlk(conn->write_mutex);
            loop->close_fd(fd);   // own-once → tam bir kez (çift-close yok)
        }
        if (fire_cb && conn->on_close_cb) {
            auto cb = conn->on_close_cb;
            pool->submit([cb]() { cb(); });
        }
    }

    // ── WS frame I/O ─────────────────────────────────────────────────────────
    void on_ws_data(int fd, const char* data, size_t len) {
        // len==0 = bağlantı kapandı (event loop clean-FIN/hata bildirimi). Registry
        // + buffer'ı reap et — aksi halde her temiz disconnect'te ws_clients/ws_bufs/
        // g_ws_registry sızar → MAX_WS'e ulaşınca yeni upgrade'ler kalıcı reddedilir
        // + broadcast kapalı/yeniden-açılmış fd'ye yazmaya devam eder.
        if (len == 0) { close_ws(fd); return; }
        std::shared_ptr<WsConnection> conn;
        {
            std::lock_guard<std::mutex> lk(ws_mtx);
            auto it = ws_clients.find(fd);
            if (it == ws_clients.end()) return;
            conn = it->second;
            ws_bufs[fd].append(data, len);
        }
        if (!conn || conn->closed.load()) { close_ws(fd); return; }

        while (true) {
            std::string snap;
            {
                std::lock_guard<std::mutex> lk(ws_mtx);
                auto it = ws_bufs.find(fd);
                if (it == ws_bufs.end()) break;
                snap = it->second;
            }
            WsFrame frame = ws_try_decode_frame(snap);
            if (frame.protocol_error) {
                // RFC 6455 §5.1: maskesiz client frame → 1002 ile kapat
                std::string cf = ws_encode_close_frame();
                { std::lock_guard<std::mutex> lk(conn->write_mutex); conn->send_raw(cf); }
                conn->closed.store(true);
                break;
            }
            if (!frame.complete) break;

            {
                std::lock_guard<std::mutex> lk(ws_mtx);
                auto it = ws_bufs.find(fd);
                if (it != ws_bufs.end()) it->second.erase(0, frame.consumed);
            }

            // RFC 6455 §5.4 parça birleştirme + §5.1 protokol ihlallerinde 1002.
            auto ws_deliver = [&](const std::string& msg) {
                if (conn->on_message) {
                    auto cb = conn->on_message;
                    pool->submit([cb, msg]() { cb(msg); });
                }
            };
            auto ws_proto_close = [&]() {
                std::string cf = ws_encode_close_frame();
                { std::lock_guard<std::mutex> lk(conn->write_mutex); conn->send_raw(cf); }
                conn->closed.store(true);
            };

            uint8_t op = frame.opcode;
            if (op == 0x00 || op == 0x01 || op == 0x02) {
                // Birleştirilmiş mesaj tavanı — parça yığma DoS'una karşı.
                static constexpr size_t WS_MAX_MESSAGE = 16 * 1024 * 1024;
                if (op == 0x00) {
                    // Continuation — aktif bir parçalı mesaj olmalı (RFC §5.4).
                    if (!conn->frag_active) { ws_proto_close(); break; }
                    conn->frag_buf += frame.payload;
                } else {
                    // Yeni text/binary — önceki parça bitmeden yenisi başlayamaz.
                    if (conn->frag_active) { ws_proto_close(); break; }
                    if (frame.fin) { ws_deliver(frame.payload); continue; }  // tek-frame
                    conn->frag_active = true;
                    conn->frag_opcode = op;
                    conn->frag_buf    = frame.payload;
                }
                if (conn->frag_buf.size() > WS_MAX_MESSAGE) { ws_proto_close(); break; }
                if (frame.fin) {                       // parçalı mesaj tamamlandı
                    std::string msg = std::move(conn->frag_buf);
                    conn->frag_buf.clear();
                    conn->frag_active = false;
                    ws_deliver(msg);
                }
            } else if (frame.opcode == 0x09) {
                std::string pong = ws_encode_pong_frame(frame.payload);
                std::lock_guard<std::mutex> lk(conn->write_mutex);
                conn->send_raw(pong);
            } else if (frame.opcode == 0x08) {
                std::string cf = ws_encode_close_frame();
                { std::lock_guard<std::mutex> lk(conn->write_mutex); conn->send_raw(cf); }
                conn->closed.store(true);
                if (conn->on_close) {
                    auto cb = conn->on_close;
                    pool->submit([cb]() { cb(); });
                }
                close_ws(fd);
                return;
            }
        }

        if (!conn->closed.load()) {
            loop->async_read(fd, [this, fd](const char* d, size_t l) {
                on_ws_data(fd, d, l);
            });
        } else {
            close_ws(fd);
        }
    }

    void close_ws(int fd) {
        std::shared_ptr<WsConnection> conn;
        {
            std::lock_guard<std::mutex> lk(ws_mtx);
            auto it = ws_clients.find(fd);
            if (it != ws_clients.end()) conn = it->second;
            ws_clients.erase(fd);
            ws_bufs.erase(fd);
        }
        // fd close-vs-send yarışı (TSan-kanıtlı): broadcast/send_raw ::send(fd) yaparken
        // close_fd(fd) fd'yi kapatırsa → kapalı/yeniden-kullanılan fd'ye yazma (cross-talk).
        // Çözüm: closed=true (yeni send başlamaz — send_raw/send_text closed kontrolü) SONRA
        // write_mutex'i DRAIN et (uçuştaki send bitene kadar bekle) — write_mutex tutulurken
        // hiçbir send_raw ::send içinde olamaz → close_fd güvenli. Kilit close_fd'yi de kapsar.
        if (conn) {
            conn->closed.store(true);
            std::lock_guard<std::mutex> wlk(conn->write_mutex);  // uçuştaki send'i drain et
            look::g_ws_registry.remove(fd);
            loop->close_fd(fd);
        } else {
            look::g_ws_registry.remove(fd);
            loop->close_fd(fd);
        }
    }
};

// ── HttpServer public API ─────────────────────────────────────────────────────

HttpServer::HttpServer(int port, int workers, HttpHandler handler,
                       WsHandler ws_handler, SseHandler sse_handler)
    : impl_(std::make_unique<Impl>())
{
    impl_->port        = port;
    impl_->workers     = workers;
    impl_->handler     = std::move(handler);
    impl_->ws_handler  = std::move(ws_handler);
    impl_->sse_handler = std::move(sse_handler);
    impl_->loop        = EventLoop::create();

    // CSWSH koruması: LOOK_WS_ORIGINS=https://site.com,https://other.com
    if (const char* env = std::getenv("LOOK_WS_ORIGINS")) {
        std::string s = env;
        size_t pos = 0, found;
        while ((found = s.find(',', pos)) != std::string::npos) {
            impl_->allowed_origins.push_back(s.substr(pos, found - pos));
            pos = found + 1;
        }
        if (pos < s.size()) impl_->allowed_origins.push_back(s.substr(pos));
    }
    impl_->pool        = std::make_unique<WorkerPool>(workers);

#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
    int srv = (int)socket(AF_INET, SOCK_STREAM, 0);
#else
    int srv = socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (srv < 0) throw std::runtime_error("socket() failed");

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0)
        throw std::runtime_error("bind() failed on port " + std::to_string(port));
    if (::listen(srv, 16384) < 0)
        throw std::runtime_error("listen() failed");

    impl_->server_fd = srv;
}

HttpServer::~HttpServer() {
    impl_->running = false;
    if (impl_->server_fd >= 0) ::close(impl_->server_fd);
}

void HttpServer::run() {
    impl_->running = true;

    // Event loop — arka planda, sadece WS/SSE için
    impl_->loop_thread = std::thread([this]() {
        impl_->loop->run();
    });

    // SO_REUSEPORT + fiber burst-accept mode (LOOK_FIBER_DISPATCH=1, Linux only):
    //   Each worker opens its own SO_REUSEPORT socket — kernel distributes
    //   connections evenly across all workers (vs old model: single shared fd
    //   → one worker grabs everything → only 1 CPU active).
    //   Worker loop: poll → burst accept → spawn fibers → run_until_complete()
    static const bool fiber_burst = []() {
#ifdef __linux__
        const char* v = std::getenv("LOOK_FIBER_DISPATCH");
        return v && std::string(v) == "1";
#else
        return false;
#endif
    }();

#ifdef __linux__
    // SO_REUSEPORT mode: per-worker sockets replace the shared fd.
    // Close shared socket now so its queue doesn't steal connections from workers.
    int shared_port = impl_->port;
    if (fiber_burst && impl_->server_fd >= 0) {
        ::close(impl_->server_fd);
        impl_->server_fd = -1;
    }
#endif

    std::vector<std::thread> workers;
    workers.reserve(impl_->workers);
    for (int i = 0; i < impl_->workers; ++i) {
#ifdef __linux__
        workers.emplace_back([this, shared_port, i]() {
#else
        workers.emplace_back([this]() {
#endif
            look::set_thread_event_loop(impl_->loop.get());

#ifdef __linux__
            // Per-worker SO_REUSEPORT listen socket
            int worker_fd = -1;
            if (fiber_burst) {
                worker_fd = ::socket(AF_INET, SOCK_STREAM, 0);
                if (worker_fd >= 0) {
                    int opt = 1;
                    ::setsockopt(worker_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
                    ::setsockopt(worker_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
                    sockaddr_in waddr{};
                    waddr.sin_family      = AF_INET;
                    waddr.sin_addr.s_addr = INADDR_ANY;
                    waddr.sin_port        = htons((uint16_t)shared_port);
                    if (::bind(worker_fd, (sockaddr*)&waddr, sizeof(waddr)) < 0 ||
                        ::listen(worker_fd, 16384) < 0) {
                        ::close(worker_fd);
                        worker_fd = -1;
                    } else {
                        int flags = fcntl(worker_fd, F_GETFL, 0);
                        fcntl(worker_fd, F_SETFL, flags | O_NONBLOCK);
                    }
                }
            }

            static thread_local look::FiberScheduler tl_srv_sched;
            if (fiber_burst && worker_fd >= 0) look::set_thread_scheduler(&tl_srv_sched);
#endif

            while (impl_->running) {
#ifdef __linux__
                if (fiber_burst && worker_fd >= 0) {
                    // ── D Fix #2: acceptor artık BİR FİBER — accept fd, bağlantı
                    // fd'leriyle AYNI scheduler epoll'unda multiplex edilir (Go
                    // netpoller modeli). Eski model: poll(accept) DIŞARIDA +
                    // run_until_complete() TÜM fiber'lar bitene dek bloklar →
                    // keep-alive fiber'ları beklerken YENİ bağlantılar hiç accept
                    // edilmiyordu (c=100 hang'inin accept ayağı). Şimdi acceptor
                    // yield eder etmez scheduler bağlantı fiber'larını koşturur;
                    // yeni bağlantı gelince epoll acceptor'ı uyandırır.
                    constexpr int FIBER_CAP = 1024;  // worker başına fiber üst sınırı
                    FiberScheduler* sched = &tl_srv_sched;
                    sched->spawn([this, worker_fd, sched]() {
                        auto self = look::Fiber::current()->shared_from_this_fiber();
                        while (impl_->running) {
                            // Backpressure: fiber tavanındayken accept'i durdur,
                            // saf zamanlayıcıyla kısa çekil (fd<0 → epoll kaydı yok,
                            // accept-ready spin'i imkansız); bağlantılar bu arada biter.
                            if ((int)sched->total_count() > FIBER_CAP) {
                                sched->wait_readable_tmo(self, -1, 5);
                                continue;
                            }
                            sockaddr_storage peer{};
                            socklen_t peer_len = sizeof(peer);
                            int client = accept4(worker_fd,
                                reinterpret_cast<sockaddr*>(&peer), &peer_len, 0);
                            if (client >= 0) {
                                char peer_ip[INET6_ADDRSTRLEN] = {};
                                if (peer.ss_family == AF_INET)
                                    inet_ntop(AF_INET,
                                        &reinterpret_cast<sockaddr_in*>(&peer)->sin_addr,
                                        peer_ip, sizeof(peer_ip));
                                else if (peer.ss_family == AF_INET6)
                                    inet_ntop(AF_INET6,
                                        &reinterpret_cast<sockaddr_in6*>(&peer)->sin6_addr,
                                        peer_ip, sizeof(peer_ip));
                                std::string ip_str = peer_ip;
                                sched->spawn([this, client, ip_str]() {
                                    impl_->handle_connection(client, ip_str);
                                });
                                continue;   // kuyruk boşalana dek accept'e devam
                            }
                            if (errno == EINTR) continue;
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                // Kuyruk boş → accept fd'yi scheduler epoll'una park et.
                                // 200ms tavan: kapanışta (running=false) en geç 200ms'de
                                // uyanıp çıkarız (eskiden süresiz bekleme riski vardı).
                                if (sched->wait_readable_tmo(self, worker_fd, 200) < 0)
                                    break;   // epoll yok (olmamalı) — fiber modu bırak
                                continue;
                            }
                            break;   // gerçek accept hatası
                        }
                    });
                    // Acceptor + tüm bağlantı fiber'ları bitene dek çalış.
                    // Acceptor running=false görünce (≤200ms) çıkar; bağlantı
                    // fiber'ları idle-timeout ile kapanır → temiz wind-down.
                    tl_srv_sched.run_until_complete();
                    continue;   // running hâlâ true ise (olağandışı) yeniden kur
                }
#endif
                // Blocking path (non-fiber or Windows)
                sockaddr_storage peer{};
                socklen_t peer_len = sizeof(peer);
#ifdef __linux__
                int srv_fd = (impl_->server_fd >= 0) ? impl_->server_fd : worker_fd;
                int client = accept4(srv_fd,
                    reinterpret_cast<sockaddr*>(&peer), &peer_len, 0);
#else
                int client = (int)accept((SOCKET)impl_->server_fd,
                    reinterpret_cast<sockaddr*>(&peer), &peer_len);
#endif
                if (client < 0) {
                    if (!impl_->running) break;
                    continue;
                }
                char peer_ip[INET6_ADDRSTRLEN] = {};
                if (peer.ss_family == AF_INET)
                    inet_ntop(AF_INET,
                        &reinterpret_cast<sockaddr_in*>(&peer)->sin_addr,
                        peer_ip, sizeof(peer_ip));
                else if (peer.ss_family == AF_INET6)
                    inet_ntop(AF_INET6,
                        &reinterpret_cast<sockaddr_in6*>(&peer)->sin6_addr,
                        peer_ip, sizeof(peer_ip));
                impl_->handle_connection(client, peer_ip);
            }

#ifdef __linux__
            if (worker_fd >= 0) {
                look::set_thread_scheduler(nullptr);
                ::close(worker_fd);
            }
#endif
        });
    }

    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }

    impl_->loop->stop();
    if (impl_->loop_thread.joinable()) impl_->loop_thread.join();
}

void HttpServer::stop() {
    impl_->running = false;
    ::close(impl_->server_fd);
    impl_->server_fd = -1;
    impl_->loop->stop();
}

} // namespace look
