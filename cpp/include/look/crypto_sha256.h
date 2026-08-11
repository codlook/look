// crypto_sha256.h — saf SHA-256 / HMAC-SHA256 / PBKDF2-HMAC-SHA256 (dikişi aç).
// Bu üçlü JWT (HS256), DKIM, session imzası, auth::hash yollarını besler. Saf + kendine-yeten
// (yalnız std) → RFC known-answer test edilebilir (NIST SHA-256 · RFC 4231 HMAC · RFC 7914 PBKDF2).
// extra_stdlib.cpp bunları using-alias ile forward eder; tek tanım → drift yok.
// NOT: postgres_client.cpp SCRAM için PARALEL bir pbkdf2/sha256 (array<32>, tek-block) taşır —
// birleştirme adayı; KAT vektörleri ortak oracle'dır (bkz. crypto_sha256_test.cpp).
#pragma once
#include <cstdint>
#include <vector>
#include <string>

namespace look {

inline std::vector<uint8_t> sha256(const std::vector<uint8_t>& data) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                   0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};

    std::vector<uint8_t> msg(data);
    uint64_t bit_len=(uint64_t)data.size()*8;
    msg.push_back(0x80);
    while(msg.size()%64!=56) msg.push_back(0);
    for(int i=7;i>=0;i--) msg.push_back((uint8_t)(bit_len>>(i*8)));

    auto rotr=[](uint32_t x,int n){return(x>>n)|(x<<(32-n));};
    for(size_t i=0;i<msg.size();i+=64){
        uint32_t w[64];
        for(int j=0;j<16;j++)
            w[j]=((uint32_t)msg[i+j*4]<<24)|((uint32_t)msg[i+j*4+1]<<16)|
                 ((uint32_t)msg[i+j*4+2]<<8)|(uint32_t)msg[i+j*4+3];
        for(int j=16;j<64;j++){
            uint32_t s0=rotr(w[j-15],7)^rotr(w[j-15],18)^(w[j-15]>>3);
            uint32_t s1=rotr(w[j-2],17)^rotr(w[j-2],19)^(w[j-2]>>10);
            w[j]=w[j-16]+s0+w[j-7]+s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for(int j=0;j<64;j++){
            uint32_t S1=rotr(e,6)^rotr(e,11)^rotr(e,25);
            uint32_t ch=(e&f)^((~e)&g);
            uint32_t t1=hh+S1+ch+K[j]+w[j];
            uint32_t S0=rotr(a,2)^rotr(a,13)^rotr(a,22);
            uint32_t maj=(a&b)^(a&c)^(b&c);
            uint32_t t2=S0+maj;
            hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
    }
    std::vector<uint8_t> r(32);
    for(int i=0;i<8;i++){r[i*4]=(h[i]>>24)&0xFF;r[i*4+1]=(h[i]>>16)&0xFF;r[i*4+2]=(h[i]>>8)&0xFF;r[i*4+3]=h[i]&0xFF;}
    return r;
}

inline std::vector<uint8_t> hmac_sha256(
    const std::vector<uint8_t>& key, const std::vector<uint8_t>& msg)
{
    static const size_t BS = 64;
    std::vector<uint8_t> k = key;
    if (k.size() > BS) k = sha256(k);
    k.resize(BS, 0);
    std::vector<uint8_t> o_key(BS), i_key(BS);
    for(size_t i=0;i<BS;i++){o_key[i]=k[i]^0x5c;i_key[i]=k[i]^0x36;}
    std::vector<uint8_t> inner(i_key);
    inner.insert(inner.end(), msg.begin(), msg.end());
    auto ih = sha256(inner);
    std::vector<uint8_t> outer(o_key);
    outer.insert(outer.end(), ih.begin(), ih.end());
    return sha256(outer);
}

// PBKDF2-HMAC-SHA256 (RFC 8018). Platform bağımsız; büyük-endian block index.
inline std::vector<uint8_t> pbkdf2_sha256(
    const std::string& password, const std::vector<uint8_t>& salt,
    uint32_t iterations, uint32_t dk_len)
{
    std::vector<uint8_t> pwd(password.begin(), password.end());
    std::vector<uint8_t> dk;
    uint32_t blocks = (dk_len + 31) / 32;
    for (uint32_t block = 1; block <= blocks; block++) {
        std::vector<uint8_t> sb = salt;
        sb.push_back((block>>24)&0xFF); sb.push_back((block>>16)&0xFF);
        sb.push_back((block>>8)&0xFF);  sb.push_back(block&0xFF);
        auto u = hmac_sha256(pwd, sb);
        auto f = u;
        for (uint32_t i = 1; i < iterations; i++) {
            u = hmac_sha256(pwd, u);
            for (size_t j = 0; j < f.size(); j++) f[j] ^= u[j];
        }
        dk.insert(dk.end(), f.begin(), f.end());
    }
    dk.resize(dk_len);
    return dk;
}

} // namespace look
