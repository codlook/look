#include "look/websocket.h"

#include <cstring>
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

std::string ws_encode_text_frame(const std::string& payload) {
    std::string f;
    size_t len = payload.size();
    f += (char)0x81;  // FIN + text opcode
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

std::string ws_encode_pong_frame(const std::string& payload) {
    std::string f;
    f += (char)0x8A;  // FIN + pong
    f += (char)(payload.size() & 0x7F);
    f += payload;
    return f;
}

// ── Frame decode (client→server, always masked) ───────────────────────────────

WsFrame ws_try_decode_frame(const std::string& buf) {
    WsFrame f;
    if (buf.size() < 2) return f;

    uint8_t b0 = (uint8_t)buf[0];
    uint8_t b1 = (uint8_t)buf[1];
    f.fin    = (b0 & 0x80) != 0;
    f.opcode = b0 & 0x0F;
    bool masked      = (b1 & 0x80) != 0;
    size_t plen      = b1 & 0x7F;
    size_t pos       = 2;

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
        if (plen > WS_MAX_FRAME) { f.complete = false; f.consumed = 0; return f; }
    }

    if (masked) {
        if (buf.size() < pos + 4 + plen) return f;
        uint8_t mask[4] = {
            (uint8_t)buf[pos], (uint8_t)buf[pos+1],
            (uint8_t)buf[pos+2], (uint8_t)buf[pos+3]
        };
        pos += 4;
        f.payload.resize(plen);
        for (size_t i = 0; i < plen; i++)
            f.payload[i] = (char)((uint8_t)buf[pos+i] ^ mask[i % 4]);
    } else {
        if (buf.size() < pos + plen) return f;
        f.payload  = buf.substr(pos, plen);
    }

    f.consumed = pos + plen;
    f.complete  = true;
    return f;
}

// ── WsConnection ─────────────────────────────────────────────────────────────

bool WsConnection::send_raw(const std::string& frame) {
    if (closed.load()) return false;
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

bool WsConnection::send_text(const std::string& msg) {
    if (closed.load()) return false;
    std::string frame = ws_encode_text_frame(msg);
    std::lock_guard<std::mutex> lk(write_mutex);
    return send_raw(frame);
}

void WsConnection::close_conn() {
    if (closed.exchange(true)) return;  // already closed
    std::string frame = ws_encode_close_frame();
    std::lock_guard<std::mutex> lk(write_mutex);
    send_raw(frame);
    // fd will be closed by HttpServer::close_ws()
}

// ── WsRegistry ───────────────────────────────────────────────────────────────

WsRegistry g_ws_registry;

void WsRegistry::add(std::shared_ptr<WsConnection> conn) {
    // LOOK_WS_MAX_CONN env ile yapılandırılabilir (varsayılan: 1024)
    static const size_t MAX_WS = []() -> size_t {
        const char* e = std::getenv("LOOK_WS_MAX_CONN");
        return (e && *e) ? (size_t)std::stoul(e) : 1024;
    }();
    std::unique_lock<std::shared_mutex> lk(mutex_);
    if (clients_.size() >= MAX_WS)
        throw std::runtime_error("WebSocket bağlantı limiti aşıldı (" + std::to_string(MAX_WS) + ")");
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

size_t WsRegistry::count() {
    std::shared_lock<std::shared_mutex> lk(mutex_);
    return clients_.size();
}

} // namespace look
