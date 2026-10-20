#include "Sha256Digest.hpp"
#include <algorithm>
#include <iomanip>
#include <memory>
#include <sstream>

#ifdef ORCA_MOBILE_ANDROID
#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace Slic3r {
namespace {
constexpr std::array<uint32_t, 64> K = {
  0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
  0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
  0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
  0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
  0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
  0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
  0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
  0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
inline uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
}
struct Sha256Digest::Impl {
    std::array<uint32_t, 8> h{0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    std::array<unsigned char, 64> buffer{};
    size_t buffered{0};
    uint64_t total{0};
    bool finalized{false};
    void transform(const unsigned char* block) {
        std::array<uint32_t,64> w{};
        for(size_t i=0;i<16;++i) w[i]=(uint32_t(block[i*4])<<24)|(uint32_t(block[i*4+1])<<16)|(uint32_t(block[i*4+2])<<8)|block[i*4+3];
        for(size_t i=16;i<64;++i){uint32_t s0=rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3);uint32_t s1=rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+s0+w[i-7]+s1;}
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],x=h[7];
        for(size_t i=0;i<64;++i){uint32_t S1=rotr(e,6)^rotr(e,11)^rotr(e,25);uint32_t ch=(e&f)^((~e)&g);uint32_t t1=x+S1+ch+K[i]+w[i];uint32_t S0=rotr(a,2)^rotr(a,13)^rotr(a,22);uint32_t maj=(a&b)^(a&c)^(b&c);uint32_t t2=S0+maj;x=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=x;
    }
};
Sha256Digest::Sha256Digest():m_impl(std::make_unique<Impl>()){}
Sha256Digest::~Sha256Digest()=default;
bool Sha256Digest::update(const void* data,size_t size){if(!m_impl||m_impl->finalized||(data==nullptr&&size))return false;const auto* p=static_cast<const unsigned char*>(data);m_impl->total+=size;while(size){size_t n=std::min(size,64-m_impl->buffered);std::memcpy(m_impl->buffer.data()+m_impl->buffered,p,n);m_impl->buffered+=n;p+=n;size-=n;if(m_impl->buffered==64){m_impl->transform(m_impl->buffer.data());m_impl->buffered=0;}}return true;}
std::string Sha256Digest::final_hex(){if(!m_impl||m_impl->finalized)return {};m_impl->finalized=true;uint64_t bits=m_impl->total*8;size_t n=m_impl->buffered;m_impl->buffer[n++]=0x80;while(n!=56){if(n==64){m_impl->transform(m_impl->buffer.data());n=0;}m_impl->buffer[n++]=0;}for(int i=7;i>=0;--i)m_impl->buffer[n++]=static_cast<unsigned char>(bits>>(i*8));m_impl->transform(m_impl->buffer.data());std::ostringstream out;out<<std::hex<<std::setfill('0');for(uint32_t v:m_impl->h)out<<std::setw(8)<<v;return out.str();}
}
#else
#include <openssl/evp.h>
#include <openssl/err.h>
namespace Slic3r { struct Sha256Digest::Impl { EVP_MD_CTX* ctx{EVP_MD_CTX_new()}; ~Impl(){EVP_MD_CTX_free(ctx);}}; Sha256Digest::Sha256Digest():m_impl(std::make_unique<Impl>()){EVP_DigestInit_ex(m_impl->ctx,EVP_sha256(),nullptr);} Sha256Digest::~Sha256Digest()=default;bool Sha256Digest::update(const void* d,size_t s){return m_impl&&m_impl->ctx&&EVP_DigestUpdate(m_impl->ctx,d,s)==1;}std::string Sha256Digest::final_hex(){if(!m_impl||!m_impl->ctx)return {};unsigned char out[EVP_MAX_MD_SIZE];unsigned int len=0;if(EVP_DigestFinal_ex(m_impl->ctx,out,&len)!=1)return {};std::ostringstream ss;ss<<std::hex<<std::setfill('0');for(unsigned int i=0;i<len;++i)ss<<std::setw(2)<<(unsigned)out[i];return ss.str();}}
#endif

