#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace Slic3r {

// Streaming SHA-256 with fixed memory usage. Windows uses its system provider;
// initialization failure and other platforms retain the OpenSSL implementation.
// The backend does not participate in the digest or its lowercase hex encoding.
class Sha256Digest {
public:
    Sha256Digest();
    ~Sha256Digest();
    Sha256Digest(const Sha256Digest&) = delete;
    Sha256Digest& operator=(const Sha256Digest&) = delete;

    bool update(const void* data, size_t size);
    // Finalization consumes the context. Failure returns an empty string;
    // subsequent updates/finalizations fail rather than publish a partial hash.
    std::string final_hex();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Slic3r
