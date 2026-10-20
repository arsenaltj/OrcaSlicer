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
    context->state.process_bytes(data, size);
    return 1;
}

inline int MD5_Final(unsigned char* digest, MD5_CTX* context)
{
    if (digest == nullptr || context == nullptr) return 0;
    boost::uuids::detail::md5::digest_type words{};
    context->state.get_digest(words);
    std::memcpy(digest, words, sizeof(words));
    return 1;
}
#endif
