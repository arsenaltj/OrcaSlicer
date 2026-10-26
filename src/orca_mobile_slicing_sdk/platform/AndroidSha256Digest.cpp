// Android SHA-256 backend. The shared core keeps its upstream system/OpenSSL backend.
#include "libslic3r/Sha256Digest.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace Slic3r {
namespace {
struct Sha256State {
    uint32_t h[8];
    uint64_t bits = 0;
    uint8_t block[64]{};
    size_t used = 0;
};
constexpr uint32_t K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
inline uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
void transform(Sha256State &s, const uint8_t *p)
{
    uint32_t w[64];
    for (unsigned i = 0; i < 16; ++i)
        w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
    for (unsigned i = 16; i < 64; ++i) {
        const uint32_t a = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t b = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + a + w[i - 7] + b;
    }
    uint32_t a=s.h[0], b=s.h[1], c=s.h[2], d=s.h[3], e=s.h[4], f=s.h[5], g=s.h[6], h=s.h[7];
    for (unsigned i = 0; i < 64; ++i) {
        const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t t1 = h + S1 + ch + K[i] + w[i];
        const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    s.h[0]+=a; s.h[1]+=b; s.h[2]+=c; s.h[3]+=d; s.h[4]+=e; s.h[5]+=f; s.h[6]+=g; s.h[7]+=h;
}
void init(Sha256State &s) {
    s.h[0]=0x6a09e667u; s.h[1]=0xbb67ae85u; s.h[2]=0x3c6ef372u; s.h[3]=0xa54ff53au;
    s.h[4]=0x510e527fu; s.h[5]=0x9b05688cu; s.h[6]=0x1f83d9abu; s.h[7]=0x5be0cd19u;
    s.bits=0; s.used=0;
}
void sha_update(Sha256State &s, const uint8_t *p, size_t n) {
    s.bits += uint64_t(n) * 8;
    while (n) { const size_t take=std::min(n, size_t(64-s.used)); std::memcpy(s.block+s.used,p,take); s.used+=take; p+=take; n-=take; if(s.used==64){transform(s,s.block);s.used=0;} }
}
std::array<uint8_t,32> finish(Sha256State s) {
    s.block[s.used++] = 0x80;
    if (s.used > 56) { while (s.used < 64) s.block[s.used++] = 0; transform(s,s.block); s.used=0; }
    while (s.used < 56) s.block[s.used++] = 0;
    for (unsigned i=0;i<8;++i) s.block[63-i]=uint8_t(s.bits>>(i*8));
    transform(s,s.block);
    std::array<uint8_t,32> out{};
    for(unsigned i=0;i<8;++i){out[i*4]=uint8_t(s.h[i]>>24);out[i*4+1]=uint8_t(s.h[i]>>16);out[i*4+2]=uint8_t(s.h[i]>>8);out[i*4+3]=uint8_t(s.h[i]);}
    return out;
}
}
struct Sha256Digest::Impl { Sha256State state{}; bool valid=false, finished=false; };
Sha256Digest::Sha256Digest() : m_impl(std::make_unique<Impl>()) { init(m_impl->state); m_impl->valid=true; }
Sha256Digest::~Sha256Digest() = default;
bool Sha256Digest::update(const void *data, size_t size) { if(!m_impl->valid||m_impl->finished)return false; if(!size)return true; if(!data){m_impl->valid=false;return false;} sha_update(m_impl->state, static_cast<const uint8_t*>(data), size); return true; }
std::string Sha256Digest::final_hex() { if(!m_impl->valid||m_impl->finished)return {}; m_impl->finished=true; const auto bytes=finish(m_impl->state); constexpr char hex[]="0123456789abcdef"; std::string out; out.reserve(64); for(uint8_t b:bytes){out+=hex[b>>4];out+=hex[b&15];} return out; }
} // namespace Slic3r
