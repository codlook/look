// ssrf_safe.h — SSRF private/özel-blok kararı (SAF, ağsız, tablo-test edilebilir).
// http_client.cpp'deki getaddrinfo döngüsü bu predikatları çağırır; karar mantığı
// burada tek noktada — hem ham v4 hem IPv6'ya gömülü v4 (mapped/NAT64/6to4/compat) için ORTAK.
#pragma once
#include <cstdint>

namespace look {

// IPv4 private/özel bloklar. ip = host-byte-order (ntohl sonrası).
inline bool is_private_v4(uint32_t ip) {
    if ((ip >> 24) == 127) return true;             // 127/8 loopback
    if ((ip >> 24) == 10)  return true;             // 10/8
    if ((ip >> 20) == (172*16 + 1)) return true;    // 172.16–31/12
    if ((ip >> 16) == (192*256 + 168)) return true; // 192.168/16
    if ((ip >> 16) == (169*256 + 254)) return true; // 169.254/16 (cloud metadata)
    if ((ip >> 22) == (100*4 + 1))     return true; // 100.64/10 CGNAT
    if ((ip >> 24) == 0)   return true;             // 0/8
    if ((ip >> 28) == 0xE) return true;             // 224/4 multicast
    if ((ip >> 28) == 0xF) return true;             // 240/4 reserved (255.255.255.255 dahil)
    return false;
}

inline uint32_t ssrf_rd32(const uint8_t* b) {
    return ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|b[3];
}

// IPv6 16-byte adres engelli mi? Gömülü v4 formları da is_private_v4'e delege eder.
inline bool ssrf_blocked_v6(const uint8_t b[16]) {
    bool is_lo = true;
    for (int i = 0; i < 15; ++i) if (b[i]) { is_lo = false; break; }
    if (is_lo && b[15] == 1) return true;   // ::1 loopback
    if (is_lo && b[15] == 0) return true;   // ::/128 unspecified
    if ((b[0] & 0xFE) == 0xFC) return true; // fc00::/7 unique-local
    if ((b[0] == 0xFE) && ((b[1] & 0xC0) == 0x80)) return true; // fe80::/10 link-local

    bool hi80_zero = true;
    for (int i = 0; i < 10; ++i) if (b[i]) { hi80_zero = false; break; }
    if (hi80_zero && b[10]==0xFF && b[11]==0xFF && is_private_v4(ssrf_rd32(b+12))) return true; // ::ffff:v4 mapped
    if (hi80_zero && b[10]==0 && b[11]==0 && (b[12]|b[13]|b[14]|b[15]) &&
        is_private_v4(ssrf_rd32(b+12))) return true; // ::v4 compat (deprecated, defense-in-depth)
    if (b[0]==0 && b[1]==0x64 && b[2]==0xFF && b[3]==0x9B &&
        !b[4]&&!b[5]&&!b[6]&&!b[7]&&!b[8]&&!b[9]&&!b[10]&&!b[11] &&
        is_private_v4(ssrf_rd32(b+12))) return true; // 64:ff9b::/96 NAT64
    if (b[0]==0x20 && b[1]==0x02 && is_private_v4(ssrf_rd32(b+2))) return true; // 2002::/16 6to4
    return false;
}

} // namespace look
