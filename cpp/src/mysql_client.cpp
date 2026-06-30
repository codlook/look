#include "look/mysql_client.h"
#include <cstring>
#include <sstream>
#include <algorithm>
#include <thread>
#include <chrono>

#ifdef _WIN32
  #pragma comment(lib, "ws2_32.lib")
#endif

namespace look {

// ── Pure C++ SHA1 — platform bagimsiz ────────────────────────────────────────

std::vector<uint8_t> MySQLClient::sha1(const std::vector<uint8_t>& data) {
    uint32_t h[5] = {0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0};

    std::vector<uint8_t> msg(data);
    uint64_t bit_len = (uint64_t)data.size() * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; i--) msg.push_back((uint8_t)(bit_len >> (i * 8)));

    auto rot = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };

    for (size_t i = 0; i < msg.size(); i += 64) {
        uint32_t w[80];
        for (int j = 0; j < 16; j++)
            w[j] = ((uint32_t)msg[i+j*4]<<24)|((uint32_t)msg[i+j*4+1]<<16)|
                   ((uint32_t)msg[i+j*4+2]<<8)|(uint32_t)msg[i+j*4+3];
        for (int j = 16; j < 80; j++) {
            uint32_t v = w[j-3]^w[j-8]^w[j-14]^w[j-16];
            w[j] = rot(v, 1);
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
        for (int j = 0; j < 80; j++) {
            uint32_t f, k;
            if      (j<20) { f=(b&c)|((~b)&d); k=0x5A827999; }
            else if (j<40) { f=b^c^d;          k=0x6ED9EBA1; }
            else if (j<60) { f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDC; }
            else           { f=b^c^d;          k=0xCA62C1D6; }
            uint32_t t = rot(a,5)+f+e+k+w[j];
            e=d; d=c; c=rot(b,30); b=a; a=t;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
    }

    std::vector<uint8_t> r(20);
    for (int i = 0; i < 5; i++) {
        r[i*4]=(h[i]>>24)&0xFF; r[i*4+1]=(h[i]>>16)&0xFF;
        r[i*4+2]=(h[i]>>8)&0xFF; r[i*4+3]=h[i]&0xFF;
    }
    return r;
}

// ── Constructor / Destructor ──────────────────────────────────────────────────

MySQLClient::MySQLClient() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) == 0)
        wsock_init_ = true;
#endif
}

MySQLClient::~MySQLClient() {
    disconnect();
#ifdef _WIN32
    if (wsock_init_) WSACleanup();
#endif
}

void MySQLClient::disconnect() {
    if (sock_ != SOCK_INVALID) {
        close_sock(sock_);
        sock_ = SOCK_INVALID;
    }
}

// ── Socket timeout ────────────────────────────────────────────────────────────

void MySQLClient::set_socket_timeout(int ms) {
    if (sock_ == SOCK_INVALID) return;
#ifdef _WIN32
    DWORD timeout = (DWORD)ms;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
#else
    struct timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

// ── Connection ────────────────────────────────────────────────────────────────

void MySQLClient::connect(const std::string& host, int port,
                           const std::string& user, const std::string& password,
                           const std::string& database) {
    cfg_.host=host; cfg_.port=port; cfg_.user=user;
    cfg_.password=password; cfg_.database=database;
    do_connect();
}

void MySQLClient::connect(const DbConfig& cfg) {
    cfg_ = cfg;
    do_connect();
}

void MySQLClient::do_connect() {
    if (sock_ != SOCK_INVALID) disconnect();

    sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock_ == SOCK_INVALID)
        throw std::runtime_error("db: socket creation failed");

    // Non-blocking mode
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(sock_, FIONBIO, &mode);
#else
    int flags = fcntl(sock_, F_GETFL, 0);
    fcntl(sock_, F_SETFL, flags | O_NONBLOCK);
#endif

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)cfg_.port);

    struct addrinfo* res = nullptr;
    struct addrinfo  hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(cfg_.host.c_str(), nullptr, &hints, &res) != 0 || !res) {
        disconnect();
        throw std::runtime_error("db: cannot resolve host: " + cfg_.host);
    }
    addr.sin_addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
    freeaddrinfo(res);

    ::connect(sock_, (struct sockaddr*)&addr, sizeof(addr));

    // Wait for connect with timeout
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock_, &wfds);
    struct timeval tv;
    tv.tv_sec  = cfg_.connect_timeout_ms / 1000;
    tv.tv_usec = (cfg_.connect_timeout_ms % 1000) * 1000;

#ifdef _WIN32
    int sel = select(0, nullptr, &wfds, nullptr, &tv);
#else
    int sel = select((int)sock_ + 1, nullptr, &wfds, nullptr, &tv);
#endif
    if (sel <= 0) {
        disconnect();
        throw std::runtime_error("db: connection timeout to " + cfg_.host + ":" + std::to_string(cfg_.port));
    }

    int err = 0;
#ifdef _WIN32
    int len = sizeof(err);
    getsockopt(sock_, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
#else
    socklen_t len = sizeof(err);
    getsockopt(sock_, SOL_SOCKET, SO_ERROR, &err, &len);
#endif
    if (err != 0) {
        disconnect();
        throw std::runtime_error("db: cannot connect to " + cfg_.host + ":" + std::to_string(cfg_.port));
    }

    // Blocking mode geri al
#ifdef _WIN32
    u_long bmode = 0;
    ioctlsocket(sock_, FIONBIO, &bmode);
#else
    int bflags = fcntl(sock_, F_GETFL, 0);
    fcntl(sock_, F_SETFL, bflags & ~O_NONBLOCK);
#endif

    set_socket_timeout(cfg_.query_timeout_ms);
    do_handshake(cfg_.user, cfg_.password, cfg_.database);
    // NO_BACKSLASH_ESCAPES modunu devre dışı bırak — aksi hâlde escape() güvensiz olur
    try { query("SET SESSION sql_mode = REPLACE(@@SESSION.sql_mode, 'NO_BACKSLASH_ESCAPES', '')"); }
    catch (...) {}
}

// ── Ping ──────────────────────────────────────────────────────────────────────

bool MySQLClient::ping() {
    if (sock_ == SOCK_INVALID) return false;
    try {
        std::vector<uint8_t> pkt = { 0x0E };
        send_packet(pkt, 0);
        uint8_t seq;
        auto resp = read_packet(seq);
        return !resp.empty() && resp[0] == 0x00;
    } catch (...) { return false; }
}

// ── ensure_connected ──────────────────────────────────────────────────────────

void MySQLClient::ensure_connected() {
    if (sock_ == SOCK_INVALID || !ping()) {
        for (int attempt = 0; attempt < cfg_.max_reconnect; ++attempt) {
            if (attempt > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.reconnect_delay_ms));
            try { do_connect(); return; } catch (...) {}
        }
        throw std::runtime_error("db: connection lost and reconnect failed after " +
                                 std::to_string(cfg_.max_reconnect) + " attempts");
    }
}

// ── Packet I/O ────────────────────────────────────────────────────────────────

bool MySQLClient::recv_bytes(uint8_t* buf, size_t len) {
    size_t received = 0;
    while (received < len) {
        int r = recv(sock_, (char*)(buf + received), (int)(len - received), 0);
        if (r <= 0) { disconnect(); return false; }
        received += r;
    }
    return true;
}

void MySQLClient::send_bytes(const uint8_t* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int r = send(sock_, (const char*)(buf + sent), (int)(len - sent), 0);
        if (r <= 0) { disconnect(); throw std::runtime_error("db: send failed — connection lost"); }
        sent += r;
    }
}

std::vector<uint8_t> MySQLClient::read_packet(uint8_t& seq) {
    uint8_t hdr[4];
    if (!recv_bytes(hdr, 4))
        throw std::runtime_error("db: connection lost or query timeout");
    uint32_t len = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16);
    seq = hdr[3];
    std::vector<uint8_t> payload(len);
    if (len > 0 && !recv_bytes(payload.data(), len))
        throw std::runtime_error("db: connection lost reading payload");
    return payload;
}

void MySQLClient::send_packet(const std::vector<uint8_t>& data, uint8_t seq) {
    uint32_t len = (uint32_t)data.size();
    uint8_t hdr[4] = {(uint8_t)len,(uint8_t)(len>>8),(uint8_t)(len>>16),seq};
    send_bytes(hdr, 4);
    if (!data.empty()) send_bytes(data.data(), data.size());
}

// ── MySQL authentication ───────────────────────────────────────────────────────

std::vector<uint8_t> MySQLClient::native_password(const std::string& password,
                                                    const std::string& challenge) {
    if (password.empty()) return {};
    std::vector<uint8_t> pwd(password.begin(), password.end());
    auto stage1 = sha1(pwd);
    auto stage2 = sha1(stage1);
    std::vector<uint8_t> combined(challenge.begin(), challenge.end());
    combined.insert(combined.end(), stage2.begin(), stage2.end());
    auto stage3 = sha1(combined);
    std::vector<uint8_t> token(20);
    for (int i = 0; i < 20; i++) token[i] = stage1[i] ^ stage3[i];
    return token;
}

void MySQLClient::do_handshake(const std::string& user,
                                const std::string& password,
                                const std::string& database) {
    uint8_t seq;
    auto pkt = read_packet(seq);
    const uint8_t* p = pkt.data();
    const uint8_t* end = p + pkt.size();

    if (*p == 0xFF) { p++; uint16_t e=read_u16(p); throw std::runtime_error("db: server error "+std::to_string(e)); }

    p++; // protocol version
    while (p < end && *p) p++; p++; // server version
    p += 4; // thread id

    std::string challenge(p, p + 8); p += 8;
    // filler(1) + capability_flags_1(2) + charset(1) + status_flags(2) + capability_flags_2(2)
    p++;           // filler
    read_u16(p);   // capability_flags_1 (advances p by 2)
    p += 3;        // charset + status_flags
    read_u16(p);   // capability_flags_2 (advances p by 2)
    // p is now at auth_plugin_data_len
    uint8_t auth_len = *p++; // read auth_plugin_data_len
    p += 10;       // skip 10 reserved bytes

    int part2_len = ((int)auth_len - 8) > 13 ? (int)auth_len - 8 : 13;
    if (p + part2_len <= end) { challenge += std::string(p, p + part2_len - 1); p += part2_len; }

    auto token = native_password(password, challenge);
    uint32_t client_flags = 0x000FA685;
    if (!database.empty()) client_flags |= 0x00000008;

    std::vector<uint8_t> auth_pkt;
    auth_pkt.push_back(client_flags&0xFF); auth_pkt.push_back((client_flags>>8)&0xFF);
    auth_pkt.push_back((client_flags>>16)&0xFF); auth_pkt.push_back((client_flags>>24)&0xFF);
    auth_pkt.insert(auth_pkt.end(), {0xFF,0xFF,0xFF,0x00});
    auth_pkt.push_back(45);
    auth_pkt.insert(auth_pkt.end(), 23, 0);
    auth_pkt.insert(auth_pkt.end(), user.begin(), user.end()); auth_pkt.push_back(0);
    auth_pkt.push_back((uint8_t)token.size());
    auth_pkt.insert(auth_pkt.end(), token.begin(), token.end());
    if (!database.empty()) { auth_pkt.insert(auth_pkt.end(), database.begin(), database.end()); auth_pkt.push_back(0); }
    const char* plugin = "mysql_native_password";
    auth_pkt.insert(auth_pkt.end(), plugin, plugin + strlen(plugin)); auth_pkt.push_back(0);

    send_packet(auth_pkt, 1);
    auto resp = read_packet(seq);
    if (resp.empty() || resp[0] == 0xFF) {
        std::string msg = "db: authentication failed";
        if (resp.size() > 3) {
            size_t off = 3;
            if (resp[off] == '#') off += 6;
            msg = "db: " + std::string(resp.begin() + off, resp.end());
        }
        throw std::runtime_error(msg);
    }
}

// ── Query ─────────────────────────────────────────────────────────────────────

std::vector<DbRow> MySQLClient::query(const std::string& sql) {
    ensure_connected();
    auto start = std::chrono::steady_clock::now();

    std::vector<uint8_t> pkt;
    pkt.push_back(0x03);
    pkt.insert(pkt.end(), sql.begin(), sql.end());

    try { send_packet(pkt, 0); } catch (...) { do_connect(); send_packet(pkt, 0); }

    uint8_t seq;
    auto resp = read_packet(seq);
    last_query_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - start).count();

    if (resp.empty()) throw std::runtime_error("db: empty response");
    const uint8_t* p = resp.data();
    const uint8_t* end = p + resp.size();

    if (resp[0] == 0xFF) {
        p++; uint16_t code=read_u16(p);
        if (p < end && *p == '#') p += 6;
        throw std::runtime_error("db: query error " + std::to_string(code) + ": " + std::string(p, end));
    }
    if (resp[0] == 0x00) {
        p++; affected_rows_=read_lenenc(p,end); last_insert_id_=read_lenenc(p,end);
        return {};
    }

    uint64_t col_count = read_lenenc(p, end);
    struct ColInfo { std::string name; uint8_t type; };
    std::vector<ColInfo> columns;
    for (uint64_t i = 0; i < col_count; i++) {
        auto col_pkt = read_packet(seq);
        const uint8_t* cp = col_pkt.data();
        const uint8_t* ce = cp + col_pkt.size();
        read_lenenc_str(cp,ce);                   // catalog
        read_lenenc_str(cp,ce);                   // schema
        read_lenenc_str(cp,ce);                   // table
        read_lenenc_str(cp,ce);                   // org_table
        std::string name = read_lenenc_str(cp,ce); // name
        read_lenenc_str(cp,ce);                   // org_name
        if (cp < ce) cp++;                        // 0x0C filler byte
        if (cp + 2 <= ce) cp += 2;               // charset (2 bytes)
        if (cp + 4 <= ce) cp += 4;               // col_length (4 bytes)
        uint8_t col_type = (cp < ce) ? *cp : 0xFE;
        columns.push_back({std::move(name), col_type});
    }
    read_packet(seq); // EOF packet

    std::vector<DbRow> rows;
    while (true) {
        auto row_pkt = read_packet(seq);
        if (row_pkt.empty() || row_pkt[0]==0xFE || row_pkt[0]==0xFF) break;
        DbRow row;
        const uint8_t* rp = row_pkt.data();
        const uint8_t* re = rp + row_pkt.size();
        for (const auto& col : columns) {
            DbValue dv;
            if (rp < re && *rp == 0xFB) {
                rp++;             // NULL marker'ı tüket
                dv.is_null = true;
            } else {
                dv.str  = read_lenenc_str(rp, re);
                dv.type = col.type;
            }
            row.push_back({col.name, std::move(dv)});
        }
        rows.push_back(row);
    }
    return rows;
}

// ── Prepared Statements (COM_STMT_PREPARE + COM_STMT_EXECUTE) ────────────────

MySQLClient::StmtMeta MySQLClient::stmt_prepare(const std::string& sql) {
    ensure_connected();
    std::vector<uint8_t> pkt;
    pkt.push_back(0x16); // COM_STMT_PREPARE
    pkt.insert(pkt.end(), sql.begin(), sql.end());
    send_packet(pkt, 0);

    uint8_t seq;
    auto resp = read_packet(seq);
    if (resp.empty()) throw std::runtime_error("db: stmt_prepare empty response");
    if (resp[0] == 0xFF) {
        const uint8_t* p = resp.data() + 1;
        const uint8_t* end = p + resp.size() - 1;
        uint16_t code; memcpy(&code, p, 2); p += 2;
        if (p < end && *p == '#') p += 6;
        throw std::runtime_error("db: " + std::string(p, end));
    }
    if (resp[0] != 0x00 || resp.size() < 12)
        throw std::runtime_error("db: malformed STMT_PREPARE response");

    const uint8_t* p = resp.data() + 1;
    uint32_t stmt_id; memcpy(&stmt_id, p, 4); p += 4;
    uint16_t num_cols;   memcpy(&num_cols,   p, 2); p += 2;
    uint16_t num_params; memcpy(&num_params, p, 2);

    // param defs + EOF
    for (int i = 0; i < num_params; i++) read_packet(seq);
    if (num_params > 0) read_packet(seq);
    // col defs + EOF
    for (int i = 0; i < num_cols; i++) read_packet(seq);
    if (num_cols > 0) read_packet(seq);

    return {stmt_id, num_cols, num_params};
}

void MySQLClient::stmt_close(uint32_t stmt_id) {
    std::vector<uint8_t> pkt;
    pkt.push_back(0x19); // COM_STMT_CLOSE
    pkt.push_back(stmt_id & 0xFF); pkt.push_back((stmt_id>>8) & 0xFF);
    pkt.push_back((stmt_id>>16) & 0xFF); pkt.push_back((stmt_id>>24) & 0xFF);
    try { send_packet(pkt, 0); } catch (...) {}
}

std::vector<DbRow> MySQLClient::stmt_execute(const StmtMeta& m,
                                              const std::vector<DbParam>& params) {
    int n = (int)params.size();
    std::vector<uint8_t> pkt;
    pkt.push_back(0x17); // COM_STMT_EXECUTE
    auto push_le32 = [&](uint32_t v) {
        for (int b=0;b<4;b++) pkt.push_back((v>>(b*8))&0xFF);
    };
    push_le32(m.id);
    pkt.push_back(0x00); // flags
    push_le32(1); // iteration count

    if (n > 0) {
        // NULL bitmap
        int nbytes = (n + 7) / 8;
        std::vector<uint8_t> nbm(nbytes, 0);
        for (int i = 0; i < n; i++)
            if (params[i].kind == DbParam::NULL_VAL) nbm[i/8] |= (1 << (i%8));
        pkt.insert(pkt.end(), nbm.begin(), nbm.end());
        pkt.push_back(0x01); // new_params_bound_flag

        // type codes (2 bytes each)
        for (int i = 0; i < n; i++) {
            uint8_t tc = 0xFE; // BLOB → string
            if (params[i].kind == DbParam::INT_VAL)   tc = 0x08; // LONGLONG
            if (params[i].kind == DbParam::FLOAT_VAL) tc = 0x05; // DOUBLE
            if (params[i].kind == DbParam::BOOL_VAL)  tc = 0x10; // TINY
            pkt.push_back(tc); pkt.push_back(0x00); // unsigned_flag=0
        }

        // values
        for (int i = 0; i < n; i++) {
            if (params[i].kind == DbParam::NULL_VAL) continue;
            switch (params[i].kind) {
                case DbParam::INT_VAL: {
                    uint64_t v; memcpy(&v, &params[i].i, 8);
                    for (int b=0;b<8;b++) pkt.push_back((v>>(b*8))&0xFF);
                    break;
                }
                case DbParam::FLOAT_VAL: {
                    uint64_t v; memcpy(&v, &params[i].d, 8);
                    for (int b=0;b<8;b++) pkt.push_back((v>>(b*8))&0xFF);
                    break;
                }
                case DbParam::BOOL_VAL:
                    pkt.push_back(params[i].b ? 1 : 0);
                    break;
                default: { // TEXT
                    const std::string& s = params[i].s;
                    uint64_t len = s.size();
                    if (len < 251)       { pkt.push_back((uint8_t)len); }
                    else if (len < 65536){ pkt.push_back(0xFC); pkt.push_back(len&0xFF); pkt.push_back((len>>8)&0xFF); }
                    else                 { pkt.push_back(0xFD); for(int b=0;b<3;b++) pkt.push_back((len>>(b*8))&0xFF); }
                    pkt.insert(pkt.end(), s.begin(), s.end());
                    break;
                }
            }
        }
    } else {
        pkt.push_back(0x01); // new_params_bound_flag (no params)
    }

    send_packet(pkt, 0);

    uint8_t seq;
    auto resp = read_packet(seq);
    if (resp.empty()) throw std::runtime_error("db: stmt_execute empty response");
    if (resp[0] == 0xFF) {
        const uint8_t* p = resp.data() + 1;
        const uint8_t* end = resp.data() + resp.size();
        uint16_t code; memcpy(&code, p, 2); p += 2;
        if (p < end && *p == '#') p += 6;
        throw std::runtime_error("db: " + std::string(p, end));
    }
    if (resp[0] == 0x00) { // OK (INSERT/UPDATE/DELETE)
        const uint8_t* p = resp.data() + 1;
        const uint8_t* end = resp.data() + resp.size();
        affected_rows_  = read_lenenc(p, end);
        last_insert_id_ = read_lenenc(p, end);
        return {};
    }

    // Binary resultset
    const uint8_t* p = resp.data();
    const uint8_t* end = p + resp.size();
    uint64_t col_count = read_lenenc(p, end);

    struct ColInfo { std::string name; uint8_t type; };
    std::vector<ColInfo> columns;
    for (uint64_t i = 0; i < col_count; i++) {
        auto col_pkt = read_packet(seq);
        const uint8_t* cp = col_pkt.data();
        const uint8_t* ce = cp + col_pkt.size();
        read_lenenc_str(cp,ce); read_lenenc_str(cp,ce); // catalog, schema
        read_lenenc_str(cp,ce); read_lenenc_str(cp,ce); // table, org_table
        std::string name = read_lenenc_str(cp,ce);
        read_lenenc_str(cp,ce); // org_name
        if (cp<ce) cp++;       // filler
        if (cp+2<=ce) cp+=2;   // charset
        if (cp+4<=ce) cp+=4;   // col_len
        uint8_t col_type = (cp<ce) ? *cp : 0xFE;
        columns.push_back({std::move(name), col_type});
    }
    read_packet(seq); // EOF

    std::vector<DbRow> rows;
    while (true) {
        auto row_pkt = read_packet(seq);
        if (row_pkt.empty() || row_pkt[0]==0xFE || row_pkt[0]==0xFF) break;

        // binary row: 0x00 header + null_bitmap
        const uint8_t* rp = row_pkt.data();
        const uint8_t* re = rp + row_pkt.size();
        if (rp < re) rp++; // skip 0x00
        int nb = ((int)col_count + 7 + 2) / 8;
        std::vector<uint8_t> nbm(nb, 0);
        for (int i=0; i<nb && rp<re; i++) nbm[i] = *rp++;

        DbRow row;
        for (size_t ci = 0; ci < col_count; ci++) {
            bool is_null = (nbm[(ci+2)/8] >> ((ci+2)%8)) & 1;
            DbValue dv; dv.type = columns[ci].type;
            if (is_null) { dv.is_null = true; }
            else {
                uint8_t ct = columns[ci].type;
                auto read_le = [&](int bytes) -> int64_t {
                    uint64_t v = 0;
                    for (int b=0; b<bytes && rp<re; b++,rp++) v |= ((uint64_t)*rp << (b*8));
                    return (int64_t)v;
                };
                switch(ct) {
                    case 0x01:           dv.str = std::to_string(read_le(1)); break;
                    case 0x02: case 0x0D: dv.str = std::to_string(read_le(2)); break;
                    case 0x03: case 0x09: dv.str = std::to_string(read_le(4)); break;
                    case 0x08:            dv.str = std::to_string(read_le(8)); break;
                    case 0x04: { float f; if(rp+4<=re){memcpy(&f,rp,4);rp+=4;} dv.str=std::to_string(f); break; }
                    case 0x05: { double d; if(rp+8<=re){memcpy(&d,rp,8);rp+=8;} dv.str=std::to_string(d); break; }
                    case 0x10:            dv.str = std::to_string(read_le(1)); break;
                    default:              dv.str = read_lenenc_str(rp, re); break;
                }
            }
            row.push_back({columns[ci].name, std::move(dv)});
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<DbRow> MySQLClient::execute(const std::string& sql, const std::vector<DbParam>& params) {
    ensure_connected();
    auto meta = stmt_prepare(sql);
    auto rows = stmt_execute(meta, params);
    stmt_close(meta.id);
    return rows;
}

// ── Escape ────────────────────────────────────────────────────────────────────

std::string MySQLClient::escape(const std::string& s) {
    std::string out; out.reserve(s.size() * 2);
    for (char c : s) {
        switch (c) {
            case '\'': out += "\\'";  break;
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case 0:    out += "\\0";  break;
            default:   out += c;
        }
    }
    return out;
}

// ── Packet readers ────────────────────────────────────────────────────────────

uint64_t MySQLClient::read_lenenc(const uint8_t*& p, const uint8_t* end) {
    if (p>=end) return 0;
    uint8_t first=*p++;
    if (first<0xFB) return first;
    if (first==0xFC&&p+2<=end){uint64_t v=p[0]|((uint64_t)p[1]<<8);p+=2;return v;}
    if (first==0xFD&&p+3<=end){uint64_t v=p[0]|((uint64_t)p[1]<<8)|((uint64_t)p[2]<<16);p+=3;return v;}
    if (first==0xFE&&p+8<=end){uint64_t v;memcpy(&v,p,8);p+=8;return v;}
    return 0;
}

std::string MySQLClient::read_lenenc_str(const uint8_t*& p, const uint8_t* end) {
    if (p>=end) return "";
    if (*p==0xFB){p++;return "";}
    uint64_t len=read_lenenc(p,end);
    if (p+len>end) len=end-p;
    std::string s(p,p+len); p+=len;
    return s;
}

uint16_t MySQLClient::read_u16(const uint8_t*& p){uint16_t v=p[0]|(p[1]<<8);p+=2;return v;}
uint32_t MySQLClient::read_u32(const uint8_t*& p){uint32_t v;memcpy(&v,p,4);p+=4;return v;}
uint64_t MySQLClient::read_u64(const uint8_t*& p){uint64_t v;memcpy(&v,p,8);p+=8;return v;}

} // namespace look
