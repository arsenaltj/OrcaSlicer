#include <catch2/catch_test_macros.hpp>
#include "libslic3r/Sha256Digest.hpp"
#include <openssl/evp.h>
#include <algorithm>
#include <array>
#include <vector>

using Slic3r::Sha256Digest;

TEST_CASE("Streaming SHA256 preserves published digests and independent contexts", "[Sha256Digest]")
{
    Sha256Digest empty, abc, million;
    REQUIRE(abc.update("a", 1));
    const std::string block(1000, 'a');
    for (int i = 0; i < 1000; ++i) REQUIRE(million.update(block.data(), block.size()));
    REQUIRE(abc.update("bc", 2));
    REQUIRE(empty.update(nullptr, 0));
    CHECK(empty.final_hex() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(abc.final_hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(million.final_hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("Streaming SHA256 matches legacy OpenSSL across padding and model block boundaries", "[Sha256Digest]")
{
    std::vector<unsigned char> input(73729);
    for (size_t i = 0; i < input.size(); ++i) input[i] = static_cast<unsigned char>((i * 131u + (i >> 8)) & 255u);
    for (size_t size : {size_t(0), size_t(55), size_t(56), size_t(63), size_t(64), size_t(65), size_t(36864), input.size()}) {
        std::array<unsigned char, EVP_MAX_MD_SIZE> reference {};
        unsigned int length = 0;
        REQUIRE(EVP_Digest(input.data(), size, reference.data(), &length, EVP_sha256(), nullptr) == 1);
        REQUIRE(length == 32);
        std::string expected;
        constexpr char digits[] = "0123456789abcdef";
        for (size_t i = 0; i < length; ++i) { expected += digits[reference[i] >> 4]; expected += digits[reference[i] & 15]; }
        for (size_t chunk : {size_t(1), size_t(63), size_t(64), size_t(65), size_t(36864)}) {
            Sha256Digest digest;
            for (size_t offset = 0; offset < size; offset += chunk)
                REQUIRE(digest.update(input.data() + offset, std::min(chunk, size - offset)));
            CHECK(digest.final_hex() == expected);
        }
    }
}

TEST_CASE("Failed or finalized SHA256 streams cannot publish a partial digest", "[Sha256Digest]")
{
    Sha256Digest finished;
    REQUIRE(finished.update("abc", 3));
    REQUIRE_FALSE(finished.final_hex().empty());
    CHECK_FALSE(finished.update("d", 1));
    CHECK_FALSE(finished.update(nullptr, 0));
    CHECK(finished.final_hex().empty());
    Sha256Digest invalid;
    REQUIRE(invalid.update("abc", 3));
    CHECK_FALSE(invalid.update(nullptr, 1));
    CHECK_FALSE(invalid.update("d", 1));
    CHECK(invalid.final_hex().empty());
}
