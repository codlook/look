#include "look/websocket.h"

#include <cstring>
#include <chrono>
#include <thread>
#include <stdexcept>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
#  include <winsock2.h>
#  pragma comment(lib, "ws2_32.lib")
#else
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace look {

// ── SHA-1 (RFC 3174) — for WebSocket handshake ───────────────────────────────

static void sha1(const unsigned char* msg, size_t len, unsigned char out[20]) {
    uint32_t h0=0x67452301, h1=0xEFCDAB89, h2=0x98BADCFE,
             h3=0x10325476, h4=0xC3D2E1F0;

    size_t new_len = ((len + 8) / 64 + 1) * 64;
    std::vector<unsigned char> buf(new_len, 0);
    memcpy(buf.data(), msg, len);
    buf[len] = 0x80;
    uint64_t bit_len = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++)
        buf[new_len - 8 + i] = (unsigned char)(bit_len >> ((7-i)*8));

    auto rotl = [](uint32_t v, int n){ return (v<<n)|(v>>(32-n)); };

    for (size_t chunk = 0; chunk < new_len; chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)buf[chunk+i*4]   << 24) | ((uint32_t)buf[chunk+i*4+1] << 16) |
                   ((uint32_t)buf[chunk+i*4+2] <<  8) |  (uint32_t)buf[chunk+i*4+3];
        for (int i = 16; i < 80; i++) {
            uint32_t t = w[i-3]^w[i-8]^w[i-14]^w[i-16];
            w[i] = rotl(t, 1);
        }
        uint32_t a=h0,b=h1,c=h2,d=h3,e=h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if      (i<20){ f=(b&c)|(~b&d);         k=0x5A827999u; }
            else if (i<40){ f=b^c^d;                 k=0x6ED9EBA1u; }
            else if (i<60){ f=(b&c)|(b&d)|(c&d);    k=0x8F1BBCDCu; }
            else          { f=b^c^d;                 k=0xCA62C1D6u; }
            uint32_t t = rotl(a,5)+f+e+k+w[i];
            e=d; d=c; c=rotl(b,30); b=a; a=t;
        }
        h0+=a; h1+=b; h2+=c; h3+=d; h4+=e;
    }
    uint32_t h[5] = {h0,h1,h2,h3,h4};
    for (int i = 0; i < 5; i++) {
        out[i*4+0]=(h[i]>>24)&0xFF; out[i*4+1]=(h[i]>>16)&0xFF;
        out[i*4+2]=(h[i]>> 8)&0xFF; out[i*4+3]=(h[i]    )&0xFF;
    }
}

// ── Base64 encode ─────────────────────────────────────────────────────────────

static std::string base64_encode(const unsigned char* data, size_t len) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t b = (uint32_t)(unsigned char)data[i] << 16;
        if (i+1 < len) b |= (uint32_t)(unsigned char)data[i+1] << 8;
        if (i+2 < len) b |= (unsigned char)data[i+2];
        out += t[(b>>18)&63];
        out += t[(b>>12)&63];
        out += (i+1 < len) ? t[(b>>6)&63] : '=';
        out += (i+2 < len) ? t[ b    &63] : '=';
    }
    return out;
}

// ── Handshake ─────────────────────────────────────────────────────────────────

std::string ws_accept_key(const std::string& client_key) {
    std::string combined = client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char hash[20];
    sha1((const unsigned char*)combined.data(), combined.size(), hash);
    return base64_encode(hash, 20);
}

std::string ws_handshake_101(const std::string& client_key) {
    return "HTTP/1.1 101 Switching Protocols\r\n"
           "Upgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           "Sec-WebSocket-Accept: " + ws_accept_key(client_key) + "\r\n"
           "\r\n";
}

// ── Frame encode (server→client, no masking) ──────────────────────────────────

static std::string ws_encode_data_frame(uint8_t opcode, const std::string& payload);
std::string ws_encode_text_frame(const std::string& payload)   { return ws_encode_data_frame(0x1, payload); }
std::string ws_encode_binary_frame(const std::string& payload) { return ws_encode_data_frame(0x2, payload); }

// RFC 3629: 1–4 bayt, en kısa kodlama, vekil (U+D800–DFFF) yok, en çok U+10FFFF.
bool ws_utf8_valid(const std::string& s) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        if (c < 0x80) { ++i; continue; }
        size_t len; uint32_t cp;
        if      (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }
        else return false;                                   // 0x80–0xC1, 0xF5–0xFF
        if (i + len > n) return false;
        for (size_t k = 1; k < len; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)) return false;   // fazla uzun
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;                              // vekil
        if (cp > 0x10FFFF) return false;
        i += len;
    }
    return true;
}

static std::string ws_encode_data_frame(uint8_t opcode, const std::string& payload) {
    std::string f;
    size_t len = payload.size();
    f += (char)(0x80 | opcode);  // FIN + text (0x1) / binary (0x2)
    if (len < 126) {
        f += (char)len;
    } else if (len < 65536) {
        f += (char)126;
        f += (char)((len >> 8) & 0xFF);
        f += (char)( len       & 0xFF);
    } else {
        f += (char)127;
        for (int i = 7; i >= 0; i--) f += (char)((len >> (i*8)) & 0xFF);
    }
    f += payload;
    return f;
}

std::string ws_encode_close_frame() {
    return std::string("\x88\x00", 2);  // FIN + close, no payload
}

std::string ws_encode_close_frame_code(uint16_t code) {
    std::string f("\x88\x02", 2);       // FIN + close, 2 baytlık durum kodu (ağ sırası)
    f += (char)((code >> 8) & 0xFF);
    f += (char)(code & 0xFF);
    return f;
}

std::string ws_encode_pong_frame(const std::string& payload) {
    std::string f;
    // RFC 6455 §5.5: kontrol frame payload'u ≤125 bayt. Decoder zaten aşırı boy
    // ping'i reddediyor ama savunma amaçlı burada da kısıtla — aksi halde
    // uzunluk baytı (7-bit) gerçek gövdeyle uyuşmayıp wire'ı desync ederdi.
    size_t n = payload.size();
    if (n > 125) n = 125;
    f += (char)0x8A;  // FIN + pong
    f += (char)(n & 0x7F);
    f.append(payload.data(), n);
    return f;
}

// ── Frame decode (client→server, always masked) ───────────────────────────────

WsFrame ws_try_decode_frame(const std::string& buf) { return ws_try_decode_frame(std::string_view(buf)); }

// string_view: olay döngüsü tamponu baştan sona tek geçişte çözer (her çerçevede kopya/silme yok).
WsFrame ws_try_decode_frame(std::string_view buf) {
    WsFrame f;
    if (buf.size() < 2) return f;

    uint8_t b0 = (uint8_t)buf[0];
    uint8_t b1 = (uint8_t)buf[1];
    f.fin    = (b0 & 0x80) != 0;
    f.opcode = b0 & 0x0F;
    bool masked      = (b1 & 0x80) != 0;
    size_t plen      = b1 & 0x7F;
    size_t pos       = 2;

    // RFC 6455 §5.2: RSV1-3 (0x70) sıfır olmalı — hiçbir uzantı müzakere edilmediğinden
    // (LOOK permessage-deflate desteklemez) sıfır-dışı RSV protokol hatasıdır. Ayrıştırıcı
    // adversarial tur (2026-09-21): sıfır-dışı RSV eskiden SESSİZCE kabul ediliyordu.
    if ((b0 & 0x70) != 0) { f.protocol_error = true; return f; }
    // RFC 6455 §5.2: yalnız tanımlı opcode'lar — data 0x0/0x1/0x2, kontrol 0x8/0x9/0xA.
    // Ayrılmış opcode'lar (0x3-0x7, 0xB-0xF) → protokol hatası (eskiden sessizce düşürülüyordu).
    if (f.opcode > 0x0A || (f.opcode > 0x02 && f.opcode < 0x08)) {
        f.protocol_error = true; return f;
    }

    if (plen == 126) {
        if (buf.size() < 4) return f;
        plen = ((size_t)(uint8_t)buf[2] << 8) | (uint8_t)buf[3];
        pos  = 4;
    } else if (plen == 127) {
        if (buf.size() < 10) return f;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | (uint8_t)buf[2+i];
        pos = 10;
        // RFC 6455 §5.2: sunucu 16 MB'ı aşan frame'leri reddeder
        static constexpr size_t WS_MAX_FRAME = 16 * 1024 * 1024;
        // Protokol hatası (kapat). Eskiden "daha fazla veri gerekiyor" dönüyordu: 2^40 bildiren bir
        // çerçeve gelen baytları sonsuza dek tamponlatıyor, her okumada tampon baştan kopyalanıyordu
        // (tek olay döngüsü %100 CPU, öteki WebSocket'ler saniyelerce bekledi).
        if (plen > WS_MAX_FRAME) { f.protocol_error = true; return f; }
    }

    // RFC 6455 §5.5: kontrol frame'leri (close 0x8 / ping 0x9 / pong 0xA) ≤125
    // bayt olmalı ve parçalanamaz (FIN=1). Aşırı boy/parçalı kontrol frame →
    // protokol hatası. (Aksi halde >125 ping'e üretilecek pong'un 7-bit uzunluğu
    // taşıp wire'ı desync ederdi.)
    if ((f.opcode & 0x08) != 0 && (plen > 125 || !f.fin)) {
        f.protocol_error = true;
        return f;
    }

    // RFC 6455 §5.1: client→server frame'leri MUTLAKA maskeli olmalı. Maskesiz
    // frame gelirse sunucu bağlantıyı 1002 (protocol error) ile kapatmalı —
    // cache poisoning / proxy zehirlenmesi savunması. Sessizce kabul edilmez.
    if (!masked) { f.protocol_error = true; return f; }
    if (buf.size() < pos + 4 + plen) return f;
    uint8_t mask[4] = {
        (uint8_t)buf[pos], (uint8_t)buf[pos+1],
        (uint8_t)buf[pos+2], (uint8_t)buf[pos+3]
    };
    pos += 4;
    f.payload.resize(plen);
    for (size_t i = 0; i < plen; i++)
        f.payload[i] = (char)((uint8_t)buf[pos+i] ^ mask[i % 4]);

    f.consumed = pos + plen;
    f.complete  = true;
    return f;
}

// ── WsConnection ─────────────────────────────────────────────────────────────

bool WsConnection::write_all(const std::string& frame) {
    const char* p = frame.data();
    size_t remaining = frame.size();
    while (remaining > 0) {
#if defined(_WIN32)
        int n = ::send(fd, p, (int)remaining, 0);
#else
        ssize_t n = ::send(fd, p, remaining, MSG_NOSIGNAL);
#endif
        if (n <= 0) { closed.store(true); return false; }
        p += n; remaining -= n;
    }
    return true;
}

bool WsConnection::send_raw(const std::string& frame) {
    if (closed.load()) return false;
    return write_all(frame);
}

bool WsConnection::send_text(const std::string& msg) {
    if (closed.load()) return false;
    std::string frame = ws_encode_text_frame(msg);
    std::lock_guard<std::mutex> lk(write_mutex);
    return send_raw(frame);
}

bool WsConnection::send_binary(const std::string& bytes) {
    if (closed.load()) return false;
    std::string frame = ws_encode_binary_frame(bytes);
    std::lock_guard<std::mutex> lk(write_mutex);
    return send_raw(frame);
}

void (*g_ws_server_close_hook)(int fd) = nullptr;

// Zarif kapanmanın sonunda (look/shutdown.h finish) açık her bağlantıya RFC 6455 §7.4.1 1001
// "going away". Eskiden süreç çıkarken bağlantı çerçevesiz kopuyordu: istemci 1006 (anormal) görüyor,
// sunucunun bilerek gittiğini ayıramıyordu (yeniden bağlanma kararı). Süreç hemen çıkacağı için
// BEKLEMEZ: yazma kilidi kısa sürede alınamazsa ya da soket tamponu doluysa çerçeve atlanır.
void WsConnection::going_away() {
    server_closed.store(true);
    if (closed.exchange(true)) return;
    const std::string f = ws_encode_close_frame_code(1001);
    std::unique_lock<std::mutex> lk(write_mutex, std::defer_lock);
    for (int i = 0; i < 100 && !lk.try_lock(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!lk.owns_lock() || fd_released) return;
#if defined(_WIN32)
    ::send(fd, f.data(), (int)f.size(), 0);
#else
    ::send(fd, f.data(), f.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
#endif
}

void WsConnection::close_conn() {
    server_closed.store(true);           // sunucu kapattı: "close" geri çağrısı çalışmaz (belge kuralı)
    if (closed.exchange(true)) return;  // already closed — yalnız ilk çağıran ilerler
    std::string frame = ws_encode_close_frame();
    {
        std::lock_guard<std::mutex> lk(write_mutex);
        // BUG (düzeltildi): closed ARTIK true → send_raw close-frame'i düşürürdü; sunucu-başlatan
        // ws::close istemciye HİÇ close-frame göndermiyordu (RFC 6455 §5.5.1 ihlali, SSE ikizinin
        // kapatılmamış hali). write_all closed-guard'sız → close-frame gerçekten telde.
        // fd_released: closed'u aldıktan sonra kilidi beklerken close_ws fd'yi kapatmış olabilir;
        // o fd numarası artık başka bir bağlantının olabilir → yazma.
        if (!fd_released) write_all(frame);
    }
    // fd'yi sunucu kapatır (close_ws, olay döngüsünde). Eskiden yalnız istemci bir şey gönderince
    // ya da gidince kapanıyordu: cevap vermeyen istemcide bağlantı ve fd süresiz kalıyordu.
    if (auto hook = g_ws_server_close_hook) hook(fd);
}

// ── WsRegistry ───────────────────────────────────────────────────────────────

WsRegistry g_ws_registry;

// Açık bağlantı sayısına sınır yoktur (LOOK 2): tavan işletim sisteminin dosya sınırıdır;
// adres başına sınır http_server'dadır.
void WsRegistry::add(std::shared_ptr<WsConnection> conn) {
    std::unique_lock<std::shared_mutex> lk(mutex_);
    clients_[conn->fd] = std::move(conn);
}

void WsRegistry::remove(int fd) {
    std::unique_lock<std::shared_mutex> lk(mutex_);
    clients_.erase(fd);
}

void WsRegistry::broadcast(const std::string& msg) {
    // Analyst note: copy list first, release lock, then send — no deadlock.
    std::vector<std::shared_ptr<WsConnection>> snapshot;
    {
        std::shared_lock<std::shared_mutex> lk(mutex_);
        snapshot.reserve(clients_.size());
        for (auto& [fd, conn] : clients_) snapshot.push_back(conn);
    }
    for (auto& conn : snapshot) conn->send_text(msg);
}

void WsRegistry::broadcast_binary(const std::string& bytes) {
    std::vector<std::shared_ptr<WsConnection>> snapshot;
    {
        std::shared_lock<std::shared_mutex> lk(mutex_);
        snapshot.reserve(clients_.size());
        for (auto& [fd, conn] : clients_) snapshot.push_back(conn);
    }
    for (auto& conn : snapshot) conn->send_binary(bytes);
}

void WsRegistry::going_away_all() {
    std::vector<std::shared_ptr<WsConnection>> snapshot;
    {
        std::shared_lock<std::shared_mutex> lk(mutex_);
        snapshot.reserve(clients_.size());
        for (auto& [fd, conn] : clients_) snapshot.push_back(conn);
    }
    for (auto& conn : snapshot) conn->going_away();
}

size_t WsRegistry::count() {
    std::shared_lock<std::shared_mutex> lk(mutex_);
    return clients_.size();
}

} // namespace look
