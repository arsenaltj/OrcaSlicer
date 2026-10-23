#include <catch2/catch_test_macros.hpp>
#include "orca_mobile_slicing_sdk/platform/MobileDigest.hpp"
#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace {
std::string md5_hex(const std::string& input, size_t chunk)
{
    MD5_CTX context;
    REQUIRE(MD5_Init(&context) == 1);
    for (size_t offset = 0; offset < input.size(); offset += chunk)
        REQUIRE(MD5_Update(&context, input.data() + offset, std::min(chunk, input.size() - offset)) == 1);
    std::array<unsigned char, 16> digest{};
    REQUIRE(MD5_Final(digest.data(), &context) == 1);
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
}

TEST_CASE("Android MD5 emits standard digest bytes across streaming chunk sizes", "[MobileDigest]")
{
    const std::array<std::pair<std::string, std::string>, 7> vectors{{
        {"", "d41d8cd98f00b204e9800998ecf8427e"},
        {"a", "0cc175b9c0f1b6a831c399e269772661"},
        {"abc", "900150983cd24fb0d6963f7d28e17f72"},
        {"message digest", "f96b697d7cb7938d525a2f31aaf161d0"},
        {"abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"},
        {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", "d174ab98d277d9f5a5611c2c9f419d9f"},
        {"12345678901234567890123456789012345678901234567890123456789012345678901234567890", "57edf4a22be3c955ac49da2e2107b67a"},
    }};
    for (const auto& [input, expected] : vectors)
        for (size_t chunk : {size_t(1), size_t(7), size_t(64)})
            CHECK(md5_hex(input, chunk) == expected);
}

TEST_CASE("Android MD5 validates null arguments and accepts empty input", "[MobileDigest]")
{
    MD5_CTX context; std::array<unsigned char, 16> digest{};
    CHECK(MD5_Init(nullptr) == 0);
    REQUIRE(MD5_Init(&context) == 1);
    CHECK(MD5_Update(nullptr, "a", 1) == 0);
    CHECK(MD5_Update(&context, nullptr, 1) == 0);
    CHECK(MD5_Update(&context, nullptr, 0) == 1);
    CHECK(MD5_Final(nullptr, &context) == 0);
    CHECK(MD5_Final(digest.data(), nullptr) == 0);
}
