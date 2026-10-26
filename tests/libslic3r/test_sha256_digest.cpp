#include <catch2/catch_test_macros.hpp>
#include "libslic3r/Sha256Digest.hpp"
#include <algorithm>
#include <array>
#include <vector>
#include <utility>

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

TEST_CASE("Streaming SHA256 matches reference vectors across padding and model block boundaries", "[Sha256Digest]")
{
    std::vector<unsigned char> input(73729);
    for (size_t i = 0; i < input.size(); ++i) input[i] = static_cast<unsigned char>((i * 131u + (i >> 8)) & 255u);
    // Independent Node.js/OpenSSL reference for byte i = (i * 131 + (i >> 8)) & 255.
    const std::array<std::pair<size_t, const char*>, 8> cases{{
        {0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {55, "5d05a4435b96f43e53e5a582bbb0b8d840a71c023e7468d6604d1c9ae6511c4c"},
        {56, "0851bc0b733318bd14db8098dec9a591c02ee1e72295d3ba54560e56342430fb"},
        {63, "3d825c09f5fe7358fe955248d4bfc207298e5e74c426ccba7c93ac3992d70143"},
        {64, "949f78c7321c5fa8a90f3d236c471950df72d869abc1d36e985cfce9a3ac98b9"},
        {65, "7978b7d18ecfdd9652aba9457bdd4f51dca470d6edfdd08055e2c244b5ae00cf"},
        {36864, "c5aa688cd63ef908b01b6ad67bd8d940109e31ce99a1732240b26dbfe6a48999"},
        {73729, "d68d41bcdd0b458ecb9f8f1cc0e7f3de8a8949be17ffc7fc6fae246a6655be4d"},
    }};
    for (const auto& [size, expected] : cases) {
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
