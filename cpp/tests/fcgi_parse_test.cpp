// FastCGI wire-parse table + fuzz test — seam (look/fcgi_parse.h).
// Network-facing PRE-DISPATCH code: nginx/Apache → FCGI records, or a direct network connection via LOOK_FCGI_BIND.
// A malformed record (excessive name/value-length, truncated, overflowing total) →
// CLEAN REJECT (partial/empty), NO CRASH/OOB. ASan+UBSan are mandatory for this file.
//   Build (ASan): g++ -std=c++17 -Iinclude -fsanitize=address,undefined \
//                   -fno-sanitize-recover=all tests/fcgi_parse_test.cpp -o /tmp/fpt
//
// POSITIVE CONTROL: when built with -DFCGI_POSCTL the "buggy" variant without clamps is called and
// on the same malformed input it MUST blow up with an ASan OOB read (proves the guard carries load).
#include "look/fcgi_parse.h"
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
using namespace look;

static int fails = 0;
#define CHK(cond, msg) do { bool _c=(cond); printf("  %-58s %s\n", msg, _c?"OK":"FAIL"); if(!_c) fails++; } while(0)

// ── fcgi_read_len tablo ───────────────────────────────────────────────────────
static void test_read_len() {
    printf("fcgi_read_len (1-bayt / 4-bayt / sınır):\n");
    { uint8_t b[]={0x00}; const uint8_t* p=b; CHK(fcgi_read_len(p,b+1)==0 && p==b+1, "0x00 -> 0 (1 bayt)"); }
    { uint8_t b[]={0x7F}; const uint8_t* p=b; CHK(fcgi_read_len(p,b+1)==127 && p==b+1, "0x7F -> 127 (1 bayt)"); }
    // high bit set → 4 bytes: 0x80000001 & 0x7FFFFFFF = 1
    { uint8_t b[]={0x80,0x00,0x00,0x01}; const uint8_t* p=b; CHK(fcgi_read_len(p,b+4)==1 && p==b+4, "0x80000001 -> 1 (4 bayt)"); }
    { uint8_t b[]={0xFF,0xFF,0xFF,0xFF}; const uint8_t* p=b; CHK(fcgi_read_len(p,b+4)==0x7FFFFFFF && p==b+4, "0xFFFFFFFF -> 2^31-1"); }
    // high bit set BUT no 4 bytes → 0, p does not advance (OOB prevented)
    { uint8_t b[]={0x80,0x00}; const uint8_t* p=b; CHK(fcgi_read_len(p,b+2)==0 && p==b, "0x80.. kesik (2 bayt) -> 0, p sabit"); }
    { const uint8_t* p=(const uint8_t*)"x"; CHK(fcgi_read_len(p,p)==0, "p==end -> 0"); }
}

// ── fcgi_parse_params tablo ───────────────────────────────────────────────────
static std::vector<uint8_t> enc_short(const std::string& n, const std::string& v) {
    std::vector<uint8_t> r;
    r.push_back((uint8_t)n.size()); r.push_back((uint8_t)v.size());
    r.insert(r.end(), n.begin(), n.end());
    r.insert(r.end(), v.begin(), v.end());
    return r;
}

static void test_parse_params() {
    printf("fcgi_parse_params (meşru + kötü-biçimli):\n");
    // legitimate: two params
    { auto a=enc_short("REQUEST_METHOD","GET"); auto b=enc_short("SCRIPT_NAME","/index.lk");
      a.insert(a.end(),b.begin(),b.end());
      std::map<std::string,std::string> m; fcgi_parse_params(a.data(),a.size(),m);
      CHK(m.size()==2 && m["REQUEST_METHOD"]=="GET" && m["SCRIPT_NAME"]=="/index.lk", "iki param düzgün"); }
    // empty content
    { std::map<std::string,std::string> m; fcgi_parse_params(nullptr,0,m); CHK(m.empty(),"boş içerik -> 0 param"); }
    // nlen exceeds the content → CUT, no crash
    { std::vector<uint8_t> a={0xFF,0x02,'X','Y'};  // nlen=255, vlen=2, only 2 bytes left
      std::map<std::string,std::string> m; fcgi_parse_params(a.data(),a.size(),m);
      CHK(m.empty(), "nlen içeriği aşar -> partial, param yok"); }
    // vlen exceeds the content → CUT (0x7F = single-byte 127, NO high bit)
    { std::vector<uint8_t> a={0x01,0x7F,'k','v'};  // nlen=1, vlen=127, only 2 bytes left
      std::map<std::string,std::string> m; fcgi_parse_params(a.data(),a.size(),m);
      CHK(m.empty(), "vlen içeriği aşar -> param yok"); }
    // 4-byte huge nlen (high bit) → total overflows, CUT (OOB bait)
    { std::vector<uint8_t> a={0x7F,0xFF,0xFF,0xFF, 0x00, 'a','b','c'};
      std::map<std::string,std::string> m; fcgi_parse_params(a.data(),a.size(),m);
      CHK(m.empty(), "dev 4-bayt nlen -> taşar, param yok (OOB yok)"); }
    // the first param is valid, the second corrupt → the first is kept, cut at the second
    { auto a=enc_short("A","1"); std::vector<uint8_t> bad={0xFF,0x00}; a.insert(a.end(),bad.begin(),bad.end());
      std::map<std::string,std::string> m; fcgi_parse_params(a.data(),a.size(),m);
      CHK(m.size()==1 && m["A"]=="1", "geçerli+bozuk -> ilki tutulur"); }
    // REGRESSION (found bug): truncated 4-byte length field → fcgi_read_len does not
    // advance p → INFINITE LOOP without the guard (worker DoS). The test returning WITHOUT HANGING
    // = the fix is in place. (Without this case the test would hang forever.)
    { std::vector<uint8_t> a={0x80,0x00}; // high bit set but NO 4 bytes
      std::map<std::string,std::string> m; fcgi_parse_params(a.data(),a.size(),m);
      CHK(m.empty(), "kesik uzunluk alanı -> DUR (sonsuz döngü YOK)"); }
    { std::vector<uint8_t> a={0x01,'k',0x80,0xFF}; // valid param then truncated length
      std::map<std::string,std::string> m; fcgi_parse_params(a.data(),a.size(),m);
      CHK(true, "geçerli+kesik-uzunluk -> asılmaz"); }
}

// ── header decoding ──────────────────────────────────────────────────────────────
static void test_header() {
    printf("fcgi_decode_header:\n");
    const uint8_t FCGI_PARAMS_T=4, FCGI_BEGIN_T=1;  // enums of fcgi.h (without pulling in the socket header)
    uint8_t h[8]={1, FCGI_PARAMS_T, 0x00,0x05, 0x01,0x00, 0x07, 0};
    auto d=fcgi_decode_header(h);
    CHK(d.version==1 && d.type==FCGI_PARAMS_T && d.req_id==5 && d.content_len==256 && d.padding_len==7,
        "header alanları BE çözülür");
    uint8_t br[8]={1, FCGI_BEGIN_T, 0,1, 0,8, 0,0};
    CHK(fcgi_decode_header(br).content_len==8, "BEGIN_REQUEST content_len=8");
    uint8_t body[8]={0,1, 0x01, 0,0,0,0,0}; // flags byte2 = KEEP_CONN
    CHK(fcgi_begin_keep_conn(body,8)==true,  "KEEP_CONN bayrağı set");
    uint8_t body2[8]={0,1, 0x00, 0,0,0,0,0};
    CHK(fcgi_begin_keep_conn(body2,8)==false,"KEEP_CONN bayrağı clear");
}

#ifdef FCGI_POSCTL
// the "buggy" variant without clamps — a PLAUSIBLE PAST regression. On malformed input it does an OOB read;
// it MUST blow up under ASan. This proves the safe parser's clamp carries load.
static void parse_params_UNSAFE(const uint8_t* p, size_t len,
                                std::map<std::string,std::string>& out) {
    const uint8_t* end = p + len;
    while (p < end) {
        uint32_t nlen = fcgi_read_len(p, end);
        uint32_t vlen = fcgi_read_len(p, end);
        /* MISSING: if (p+nlen+vlen>end) break;  ← guard REMOVED */
        std::string name (p, p + nlen); p += nlen;   // OOB read (nlen exceeds the buffer)
        std::string value(p, p + vlen); p += vlen;
        out[name] = value;
    }
}
#endif

int main() {
#ifdef FCGI_POSCTL
    // POSITIVE CONTROL: the buggy variant does an OOB read on a malformed record → an ASan abort is expected.
    std::vector<uint8_t> evil={0xFF,0x02,'X','Y'};  // nlen=255 ama 2 bayt var
    std::map<std::string,std::string> m; parse_params_UNSAFE(evil.data(),evil.size(),m);
    printf("POZİTİF-KONTROL: buggy varyant PATLAMADI — guard testi ETKİSİZ (BEKLENMEDİK)\n");
    return 0;  // if it survives the ASan abort, flags that the test is toothless
#else
    printf("FastCGI wire-parse tablo + fuzz (seam: look/fcgi_parse.h):\n\n");
    test_read_len();
    test_parse_params();
    test_header();

    // ── FUZZ: random/adversarial bytes → NO CRASH (caught by ASan/UBSan) ──
    printf("fuzz (deterministik PRNG, 200K iterasyon, ASan temiz olmalı):\n");
    uint64_t s=0x9E3779B97F4A7C15ull;
    auto rnd=[&]{ s^=s<<13; s^=s>>7; s^=s<<17; return s; };
    for (int it=0; it<200000; ++it) {
        size_t n = rnd() % 64;
        std::vector<uint8_t> buf(n);
        for (auto& b : buf) b = (uint8_t)(rnd() & 0xFF);
        std::map<std::string,std::string> m;
        fcgi_parse_params(buf.data(), buf.size(), m);   // must not be OOB/UB
        if (buf.size()>=8) (void)fcgi_decode_header(buf.data());
    }
    // large single-param OOB bait
    { std::vector<uint8_t> big={0x7F,0xFF,0xFF,0xFF}; big.push_back(0x00);
      std::map<std::string,std::string> m; fcgi_parse_params(big.data(),big.size(),m); }
    printf("  200K fuzz + OOB yem: ÇÖKME YOK\n");

    printf(fails ? "\n%d FAIL\n" : "\nTÜM VAKALAR GEÇTİ (fcgi wire-parse sağlam, kötü-biçimli record temiz-red)\n", fails);
    return fails ? 1 : 0;
#endif
}
