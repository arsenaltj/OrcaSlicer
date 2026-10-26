#pragma once

#ifdef ORCA_MOBILE_ANDROID
#include <boost/uuid/detail/md5.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>

struct MD5_CTX {
    boost::uuids::detail::md5 state;
};

inline int MD5_Init(MD5_CTX* context)
{
    if (context == nullptr) return 0;
    context->state = boost::uuids::detail::md5{};
    return 1;
}

inline int MD5_Update(MD5_CTX* context, const void* data, size_t size)
{
    if (context == nullptr || (data == nullptr && size != 0)) return 0;
    if (size != 0) context->state.process_bytes(data, size);
    return 1;
}

inline int MD5_Final(unsigned char* digest, MD5_CTX* context)
{
    if (digest == nullptr || context == nullptr) return 0;
    boost::uuids::detail::md5::digest_type words{};
    context->state.get_digest(words);
    // Boost 1.84 exposes big-endian digest words; emit the standard MD5 byte stream.
    for (size_t word = 0; word < 4; ++word)
        for (size_t byte = 0; byte < 4; ++byte)
            digest[word * 4 + byte] = static_cast<unsigned char>(words[word] >> (24 - byte * 8));
    return 1;
}
#endif
