#include "look/postgres_client.h"
#include <cstring>
#include <cctype>
#include <sstream>
#include <algorithm>

#ifdef _WIN32
  #pragma comment(lib, "ws2_32.lib")
#endif

namespace look {

// ── MD5 (RFC 1321) ────────────────────────────────────────────────────────────

static const uint32_t MD5_T[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};
static const int MD5_S[64] = {
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
};

std::array<uint8_t,16> PostgresClient::md5_raw(const uint8_t* data, size_t len) {
    uint32_t s[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};

    std::vector<uint8_t> msg(data, data + len);
    uint64_t bit_len = (uint64_t)len * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 0; i < 8; i++) msg.push_back((uint8_t)(bit_len >> (i * 8)));

    auto rot = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };

    for (size_t i = 0; i < msg.size(); i += 64) {
        uint32_t m[16];
        for (int j = 0; j < 16; j++)
            m[j] = msg[i+j*4] | ((uint32_t)msg[i+j*4+1]<<8) |
                   ((uint32_t)msg[i+j*4+2]<<16) | ((uint32_t)msg[i+j*4+3]<<24);
        uint32_t a=s[0], b=s[1], c=s[2], d=s[3];
        for (int j = 0; j < 64; j++) {
            uint32_t f; int g;
            if      (j < 16) { f = (b&c)|(~b&d);  g = j; }
            else if (j < 32) { f = (d&b)|(~d&c);  g = (5*j+1)%16; }
            else if (j < 48) { f = b^c^d;          g = (3*j+5)%16; }
            else             { f = c^(b|(~d));     g = (7*j)%16; }
            f = f + a + MD5_T[j] + m[g];
            a = d; d = c; c = b;
            b = b + rot(f, MD5_S[j]);
        }
        s[0]+=a; s[1]+=b; s[2]+=c; s[3]+=d;
    }

    std::array<uint8_t,16> r{};
    for (int i = 0; i < 4; i++) {
        r[i*4]=(s[i])&0xFF; r[i*4+1]=(s[i]>>8)&0xFF;
        r[i*4+2]=(s[i]>>16)&0xFF; r[i*4+3]=(s[i]>>24)&0xFF;
    }
    return r;
}

std::string PostgresClient::md5_hex(const uint8_t* data, size_t len) {
    static const char hex[] = "0123456789abcdef";
    auto h = md5_raw(data, len);
    std::string out;
    out.reserve(32);
    for (auto b : h) { out += hex[b>>4]; out += hex[b&0xF]; }
    return out;
}

std::string PostgresClient::pg_md5_password(const std::string& password,
                                              const std::string& user,
                                              const uint8_t salt[4]) {
    // inner = md5(password + user)
    std::string inner = password + user;
    std::string inner_hex = md5_hex((const uint8_t*)inner.data(), inner.size());
    // outer = md5(inner_hex + salt)
    std::string outer = inner_hex;
    outer.append((const char*)salt, 4);
    return "md5" + md5_hex((const uint8_t*)outer.data(), outer.size());
}

// ── Big-endian helpers ────────────────────────────────────────────────────────

uint32_t PostgresClient::read_u32_be(const uint8_t* p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|(uint32_t)p[3];
}
int32_t PostgresClient::read_i32_be(const uint8_t* p) {
    return (int32_t)read_u32_be(p);
}
uint16_t PostgresClient::read_u16_be(const uint8_t* p) {
    return (uint16_t)(((uint32_t)p[0]<<8)|(uint32_t)p[1]);
}
void PostgresClient::write_u32_be(uint8_t* p, uint32_t v) {
    p[0]=(v>>24)&0xFF; p[1]=(v>>16)&0xFF; p[2]=(v>>8)&0xFF; p[3]=v&0xFF;
}

// ── OID → LOOK type code ──────────────────────────────────────────────────────

uint8_t PostgresClient::oid_to_type(int32_t oid) {
    // INTEGER türleri
    if (oid == 20 || oid == 21 || oid == 23 || oid == 26 || oid == 16000)
        return pg_type::INT;
    // FLOAT / NUMERIC türleri
    if (oid == 700 || oid == 701 || oid == 1700)
        return pg_type::FLOAT;
    // BOOL
    if (oid == 16)
        return pg_type::BOOL;
    // NULL ve bilinmeyenler → string
    return 0xFE;
}

// ── Constructor / Destructor ──────────────────────────────────────────────────

PostgresClient::PostgresClient() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) == 0)
        wsock_init_ = true;
#endif
}

PostgresClient::~PostgresClient() {
    disconnect();
#ifdef _WIN32
    if (wsock_init_) WSACleanup();
#endif
}

void PostgresClient::disconnect() {
    if (sock_ != PG_SOCK_INVALID) {
        // Terminate mesajı gönder (best effort)
        try {
            uint8_t msg[5] = {'X', 0, 0, 0, 4};
            send(sock_, (const char*)msg, 5, 0);
        } catch (...) {}
        pg_close_sock(sock_);
        sock_ = PG_SOCK_INVALID;
    }
}

// ── Socket I/O ────────────────────────────────────────────────────────────────

bool PostgresClient::recv_bytes(uint8_t* buf, size_t len) {
    size_t received = 0;
    while (received < len) {
        int r = recv(sock_, (char*)(buf + received), (int)(len - received), 0);
        if (r <= 0) {
            pg_close_sock(sock_);
            sock_ = PG_SOCK_INVALID;
            return false;
        }
        received += r;
    }
    return true;
}

void PostgresClient::send_bytes(const uint8_t* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int r = send(sock_, (const char*)(buf + sent), (int)(len - sent), 0);
        if (r <= 0) throw std::runtime_error("db postgres: send failed — connection lost");
        sent += r;
    }
}

void PostgresClient::set_socket_timeout(int ms) {
    if (sock_ == PG_SOCK_INVALID) return;
#ifdef _WIN32
    DWORD t = (DWORD)ms;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof(t));
#else
    struct timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

// ── Message I/O ───────────────────────────────────────────────────────────────

// Backend → type(1) + len(4, len içinde kendisi var ama type yok) + body
PostgresClient::PgMsg PostgresClient::read_message() {
    uint8_t type_byte;
    if (!recv_bytes(&type_byte, 1))
        throw std::runtime_error("db postgres: connection lost");
    uint8_t lb[4];
    if (!recv_bytes(lb, 4))
        throw std::runtime_error("db postgres: connection lost reading length");
    uint32_t len = read_u32_be(lb);
    if (len < 4) throw std::runtime_error("db postgres: invalid message length");
    size_t body_len = len - 4;
    std::vector<uint8_t> body(body_len);
    if (body_len > 0 && !recv_bytes(body.data(), body_len))
        throw std::runtime_error("db postgres: connection lost reading body");
    return PgMsg{(char)type_byte, std::move(body)};
}

// Frontend → type(1) + len(4, len içinde kendisi var) + body
void PostgresClient::send_message(char type, const std::vector<uint8_t>& body) {
    uint32_t len = 4 + (uint32_t)body.size();
    uint8_t hdr[5];
    hdr[0] = (uint8_t)type;
    write_u32_be(hdr + 1, len);
    send_bytes(hdr, 5);
    if (!body.empty()) send_bytes(body.data(), body.size());
}

// ── Connect ───────────────────────────────────────────────────────────────────

void PostgresClient::connect(const std::string& host, int port,
                              const std::string& user, const std::string& password,
                              const std::string& database) {
    host_ = host; port_ = port; user_ = user;
    password_ = password; database_ = database;
    do_connect();
}

void PostgresClient::do_connect() {
    if (sock_ != PG_SOCK_INVALID) {
        pg_close_sock(sock_);
        sock_ = PG_SOCK_INVALID;
    }

    sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock_ == PG_SOCK_INVALID)
        throw std::runtime_error("db postgres: socket creation failed");

    // Non-blocking connect
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(sock_, FIONBIO, &mode);
#else
    int flags = fcntl(sock_, F_GETFL, 0);
    fcntl(sock_, F_SETFL, flags | O_NONBLOCK);
#endif

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port_);

    struct addrinfo* res = nullptr;
    struct addrinfo  hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host_.c_str(), nullptr, &hints, &res) != 0 || !res) {
        pg_close_sock(sock_); sock_ = PG_SOCK_INVALID;
        throw std::runtime_error("db postgres: cannot resolve host: " + host_);
    }
    addr.sin_addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
    freeaddrinfo(res);

    ::connect(sock_, (struct sockaddr*)&addr, sizeof(addr));

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock_, &wfds);
    struct timeval tv;
    tv.tv_sec  = connect_timeout_ms_ / 1000;
    tv.tv_usec = (connect_timeout_ms_ % 1000) * 1000;
#ifdef _WIN32
    int sel = select(0, nullptr, &wfds, nullptr, &tv);
#else
    int sel = select((int)sock_ + 1, nullptr, &wfds, nullptr, &tv);
#endif
    if (sel <= 0) {
        pg_close_sock(sock_); sock_ = PG_SOCK_INVALID;
        throw std::runtime_error("db postgres: timeout connecting to " + host_ + ":" + std::to_string(port_));
    }

    int err = 0;
#ifdef _WIN32
    int elen = sizeof(err);
    getsockopt(sock_, SOL_SOCKET, SO_ERROR, (char*)&err, &elen);
#else
    socklen_t elen = sizeof(err);
    getsockopt(sock_, SOL_SOCKET, SO_ERROR, &err, &elen);
#endif
    if (err != 0) {
        pg_close_sock(sock_); sock_ = PG_SOCK_INVALID;
        throw std::runtime_error("db postgres: cannot connect to " + host_ + ":" + std::to_string(port_));
    }

    // Blocking moduna geri al
#ifdef _WIN32
    u_long bmode = 0;
    ioctlsocket(sock_, FIONBIO, &bmode);
#else
    int bflags = fcntl(sock_, F_GETFL, 0);
    fcntl(sock_, F_SETFL, bflags & ~O_NONBLOCK);
#endif

    set_socket_timeout(query_timeout_ms_);
    send_startup();
    do_auth();
}

// ── Startup ───────────────────────────────────────────────────────────────────

void PostgresClient::send_startup() {
    // Startup: len(4) + protocol(4=0x00030000) + params + '\0'
    std::vector<uint8_t> body;
    uint8_t proto[4];
    write_u32_be(proto, 196608); // 3 << 16
    body.insert(body.end(), proto, proto + 4);

    auto append_param = [&](const char* key, const std::string& val) {
        while (*key) body.push_back((uint8_t)*key++);
        body.push_back('\0');
        body.insert(body.end(), val.begin(), val.end());
        body.push_back('\0');
    };
    append_param("user", user_);
    append_param("database", database_);
    append_param("client_encoding", "UTF8");
    body.push_back('\0'); // param list sonu

    // Startup mesajı: type byte YOK, sadece len + body
    uint32_t total = 4 + (uint32_t)body.size();
    uint8_t lb[4];
    write_u32_be(lb, total);
    send_bytes(lb, 4);
    send_bytes(body.data(), body.size());
}

// ── Auth ──────────────────────────────────────────────────────────────────────

static std::string pg_parse_error(const std::vector<uint8_t>& body) {
    const uint8_t* p = body.data();
    const uint8_t* end = p + body.size();
    while (p < end && *p != '\0') {
        char ft = (char)*p++;
        std::string fv;
        while (p < end && *p != '\0') fv += (char)*p++;
        if (p < end) p++;
        if (ft == 'M') return fv;
    }
    return "unknown error";
}

void PostgresClient::do_auth() {
    while (true) {
        auto msg = read_message();

        if (msg.type == 'R') {
            if (msg.body.size() < 4)
                throw std::runtime_error("db postgres: malformed auth message");
            uint32_t auth_type = read_u32_be(msg.body.data());

            if (auth_type == 0) {
                // AuthenticationOk — devam
            } else if (auth_type == 3) {
                // Cleartext
                std::vector<uint8_t> body(password_.begin(), password_.end());
                body.push_back('\0');
                send_message('p', body);
            } else if (auth_type == 5) {
                // MD5
                if (msg.body.size() < 8)
                    throw std::runtime_error("db postgres: missing MD5 salt");
                uint8_t salt[4];
                memcpy(salt, msg.body.data() + 4, 4);
                std::string pwd = pg_md5_password(password_, user_, salt);
                std::vector<uint8_t> body(pwd.begin(), pwd.end());
                body.push_back('\0');
                send_message('p', body);
            } else {
                throw std::runtime_error("db postgres: unsupported auth method " + std::to_string(auth_type)
                    + " — use md5 or trust in pg_hba.conf");
            }
        } else if (msg.type == 'E') {
            throw std::runtime_error("db postgres: " + pg_parse_error(msg.body));
        } else if (msg.type == 'Z') {
            // ReadyForQuery — auth tamamlandı
            return;
        }
        // 'S' (ParameterStatus), 'K' (BackendKeyData), 'N' (Notice) → yoksay
    }
}

// ── Query ─────────────────────────────────────────────────────────────────────

std::vector<DbRow> PostgresClient::query(const std::string& sql) {
    if (sock_ == PG_SOCK_INVALID) {
        try { do_connect(); } catch (...) {}
        if (sock_ == PG_SOCK_INVALID)
            throw std::runtime_error("db postgres: not connected");
    }

    try {
        return simple_query(sql);
    } catch (const std::runtime_error&) {
        // Bir kez reconnect dene
        try { do_connect(); } catch (...) { throw; }
        return simple_query(sql);
    }
}

std::vector<DbRow> PostgresClient::simple_query(const std::string& sql) {
    // Query mesajı: 'Q' + sql + '\0'
    std::vector<uint8_t> body(sql.begin(), sql.end());
    body.push_back('\0');
    send_message('Q', body);

    std::vector<DbRow> rows;
    struct ColInfo { std::string name; uint8_t type_code; };
    std::vector<ColInfo> columns;

    affected_rows_  = 0;
    last_insert_id_ = 0;

    // SQL INSERT mi? (lastval() için)
    bool is_insert = false;
    {
        const char* p = sql.c_str();
        while (*p && isspace((unsigned char)*p)) p++;
        char upper6[7] = {};
        for (int i = 0; i < 6 && p[i]; i++) upper6[i] = (char)toupper((unsigned char)p[i]);
        is_insert = (strcmp(upper6, "INSERT") == 0);
    }

    while (true) {
        PgMsg msg;
        try { msg = read_message(); }
        catch (...) { pg_close_sock(sock_); sock_ = PG_SOCK_INVALID; throw; }

        if (msg.type == 'T') {
            // RowDescription
            columns.clear();
            if (msg.body.size() < 2) continue;
            uint16_t ncols = read_u16_be(msg.body.data());
            const uint8_t* p = msg.body.data() + 2;
            const uint8_t* end = msg.body.data() + msg.body.size();
            for (int i = 0; i < (int)ncols && p < end; i++) {
                // col name (null-term)
                std::string name;
                while (p < end && *p) name += (char)*p++;
                if (p < end) p++; // null
                // tableOID(4) + colAttr(2) = 6 bytes
                if (p + 6 > end) break;
                p += 6;
                // typeOID (4 bytes)
                if (p + 4 > end) break;
                int32_t type_oid = read_i32_be(p); p += 4;
                // typeSize(2) + typeMod(4) + format(2) = 8 bytes
                if (p + 8 <= end) p += 8;
                columns.push_back({std::move(name), oid_to_type(type_oid)});
            }
        } else if (msg.type == 'D') {
            // DataRow
            if (msg.body.size() < 2) continue;
            uint16_t ncols = read_u16_be(msg.body.data());
            const uint8_t* p = msg.body.data() + 2;
            const uint8_t* end = msg.body.data() + msg.body.size();
            DbRow row;
            for (int i = 0; i < (int)ncols && p + 4 <= end; i++) {
                int32_t field_len = read_i32_be(p); p += 4;
                std::string val;
                if (field_len >= 0 && p + field_len <= end) {
                    val = std::string((const char*)p, field_len);
                    p += field_len;
                }
                uint8_t tc = (i < (int)columns.size()) ? columns[i].type_code : 0xFE;
                std::string name = (i < (int)columns.size()) ? columns[i].name : "col" + std::to_string(i);
                bool is_null = (field_len < 0);
                if (is_null) tc = pg_type::NUL;
                row.push_back({name, DbValue{val, tc, is_null}});
            }
            rows.push_back(std::move(row));
        } else if (msg.type == 'C') {
            // CommandComplete: "INSERT 0 5", "SELECT 3", "UPDATE 2" ...
            std::string tag((const char*)msg.body.data(),
                            msg.body.empty() ? 0 : msg.body.size() - 1);
            auto sp = tag.rfind(' ');
            if (sp != std::string::npos) {
                try { affected_rows_ = std::stoll(tag.substr(sp + 1)); } catch (...) {}
            }
        } else if (msg.type == 'Z') {
            // ReadyForQuery — bitti
            break;
        } else if (msg.type == 'E') {
            std::string err = "db postgres: " + pg_parse_error(msg.body);
            // ReadyForQuery'ye kadar drain et
            while (true) {
                try { auto m = read_message(); if (m.type == 'Z') break; } catch (...) { break; }
            }
            throw std::runtime_error(err);
        }
        // 'N' (Notice), 'I' (EmptyQuery) → yoksay
    }

    // INSERT sonrası lastval() ile son ID'yi al
    if (is_insert && affected_rows_ > 0) {
        try {
            auto lv = do_lastval();
            if (!lv.empty() && !lv[0].empty())
                last_insert_id_ = std::stoll(lv[0][0].second.str);
        } catch (...) {
            last_insert_id_ = 0;
        }
    }

    return rows;
}

// Lastval() için internal sorgu — reconnect döngüsü yok
std::vector<DbRow> PostgresClient::do_lastval() {
    std::string sql = "SELECT lastval()";
    std::vector<uint8_t> body(sql.begin(), sql.end());
    body.push_back('\0');
    send_message('Q', body);

    std::vector<DbRow> rows;
    while (true) {
        PgMsg msg;
        try { msg = read_message(); } catch (...) { break; }
        if (msg.type == 'D' && msg.body.size() >= 6) {
            const uint8_t* p = msg.body.data() + 2;
            const uint8_t* end = msg.body.data() + msg.body.size();
            if (p + 4 <= end) {
                int32_t flen = read_i32_be(p); p += 4;
                if (flen >= 0 && p + flen <= end) {
                    DbRow row;
                    row.push_back({"lastval", DbValue{std::string((const char*)p, flen), pg_type::INT}});
                    rows.push_back(row);
                }
            }
        } else if (msg.type == 'Z') break;
    }
    return rows;
}

} // namespace look
