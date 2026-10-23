#include "Sha256Digest.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <openssl/evp.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

namespace Slic3r {

struct Sha256Digest::Impl {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> openssl {nullptr, EVP_MD_CTX_free};
    bool valid = false, finished = false;
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    ~Impl()
    {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    }
#endif
};

Sha256Digest::Sha256Digest() : m_impl(std::make_unique<Impl>())
{
#ifdef _WIN32
    if (BCryptOpenAlgorithmProvider(&m_impl->algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
        BCryptCreateHash(m_impl->algorithm, &m_impl->hash, nullptr, 0, nullptr, 0, 0) >= 0) {
        m_impl->valid = true;
        return;
    }
    // Choose the fallback before accepting any bytes. A failed update cannot
    // restart another backend without retaining the entire input stream.
    if (m_impl->hash) { BCryptDestroyHash(m_impl->hash); m_impl->hash = nullptr; }
    if (m_impl->algorithm) { BCryptCloseAlgorithmProvider(m_impl->algorithm, 0); m_impl->algorithm = nullptr; }
#endif
    m_impl->openssl.reset(EVP_MD_CTX_new());
    m_impl->valid = m_impl->openssl && EVP_DigestInit_ex(m_impl->openssl.get(), EVP_sha256(), nullptr) == 1;
}

Sha256Digest::~Sha256Digest() = default;

bool Sha256Digest::update(const void* data, size_t size)
{
    if (!m_impl->valid || m_impl->finished) return false;
    if (!size) return true;
    if (!data) { m_impl->valid = false; return false; }
#ifdef _WIN32
    if (m_impl->hash) {
        auto bytes = static_cast<const unsigned char*>(data);
        while (size) {
            const auto count = static_cast<ULONG>(std::min(size, size_t(std::numeric_limits<ULONG>::max())));
            if (BCryptHashData(m_impl->hash, const_cast<PUCHAR>(bytes), count, 0) < 0) {
                m_impl->valid = false;
                return false;
            }
            bytes += count;
            size -= count;
        }
        return true;
    }
#endif
    m_impl->valid = EVP_DigestUpdate(m_impl->openssl.get(), data, size) == 1;
    return m_impl->valid;
}

std::string Sha256Digest::final_hex()
{
    if (!m_impl->valid || m_impl->finished) return {};
    m_impl->finished = true;
    std::array<unsigned char, 32> bytes {};
#ifdef _WIN32
    if (m_impl->hash) {
        if (BCryptFinishHash(m_impl->hash, bytes.data(), ULONG(bytes.size()), 0) < 0) return {};
    } else
#endif
    {
        unsigned int length = 0;
        if (EVP_DigestFinal_ex(m_impl->openssl.get(), bytes.data(), &length) != 1 || length != bytes.size()) return {};
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (auto byte : bytes) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}

} // namespace Slic3r
