#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "../../deps_src/stb_dxt/stb_dxt.h"
#include "../../src/slic3r/GUI/FontTextureCompression.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <future>
#include <utility>
#include <vector>

namespace {
unsigned alpha_at(const unsigned char* block, unsigned pixel)
{
    unsigned values[8] = {block[0], block[1]};
    if (values[0] > values[1]) {
        for (unsigned i = 1; i <= 6; ++i)
            values[i + 1] = ((7 - i) * values[0] + i * values[1]) / 7;
    } else {
        for (unsigned i = 1; i <= 4; ++i)
            values[i + 1] = ((5 - i) * values[0] + i * values[1]) / 5;
        values[6] = 0; values[7] = 255;
    }
    uint64_t indices = 0;
    for (unsigned i = 0; i < 6; ++i) indices |= uint64_t(block[2 + i]) << (8 * i);
    return values[(indices >> (3 * pixel)) & 7];
}
}

TEST_CASE("Font DXT5 preserves alpha coverage and fits partial edge blocks", "[FontTextureCompression]")
{
    const auto width = GENERATE(1, 3, 4, 7, 32);
    const auto height = GENERATE(1, 3, 4, 9);
    std::vector<unsigned char> pixels(size_t(width) * height * 4, 255);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            pixels[(y * width + x) * 4 + 3] = (x + y) % 2 ? 255 : 0;
    const auto original = pixels;
    const size_t bytes = size_t((width + 3) / 4) * ((height + 3) / 4) * 16;
    std::vector<unsigned char> encoded(bytes + 32, 0xa5);
    int written = 0;
    rygCompress(encoded.data(), pixels.data(), width, height, 1, written);
    REQUIRE(size_t(written) == bytes);
    CHECK(pixels == original);
    CHECK(std::all_of(encoded.begin() + bytes, encoded.end(), [](auto b) { return b == 0xa5; }));
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const size_t block = size_t(y / 4) * ((width + 3) / 4) + x / 4;
            CHECK(alpha_at(encoded.data() + block * 16, (y % 4) * 4 + x % 4) == pixels[(y * width + x) * 4 + 3]);
        }
}

TEST_CASE("Concurrent font and bed compression produce identical blocks", "[FontTextureCompression]")
{
    std::atomic<bool> start{false};
    std::vector<std::future<std::vector<unsigned char>>> jobs;
    for (int i = 0; i < 8; ++i)
        jobs.emplace_back(std::async(std::launch::async, [&] {
            std::vector<unsigned char> pixels(64);
            for (size_t j = 0; j < pixels.size(); ++j) pixels[j] = static_cast<unsigned char>(j * 3);
            std::vector<unsigned char> result(16);
            while (!start.load()) std::this_thread::yield();
            stb_compress_dxt_block(result.data(), pixels.data(), 1, STB_DXT_NORMAL);
            return result;
        }));
    start.store(true);
    const auto expected = jobs.front().get();
    for (size_t i = 1; i < jobs.size(); ++i) CHECK(jobs[i].get() == expected);
}

TEST_CASE("Banded font DXT5 is byte-identical to serial compression", "[FontTextureCompression]")
{
    for (const auto [width, height] : {std::pair{1024, 1025}, std::pair{1001, 1100}}) {
        std::vector<unsigned char> pixels(size_t(width) * height * 4);
        for (size_t i = 0; i < pixels.size(); ++i)
            pixels[i] = static_cast<unsigned char>((i * 37 + i / 17) & 0xff);
        const auto original = pixels;
        const size_t size = size_t((width + 3) / 4) * ((height + 3) / 4) * 16;
        std::vector<unsigned char> serial(size + 16, 0xa5);
        std::vector<unsigned char> banded(size + 16, 0xa5);
        int serial_size = 0, banded_size = 0;
        rygCompress(serial.data(), pixels.data(), width, height, 1, serial_size);
        Slic3r::GUI::compress_font_dxt5(banded.data(), pixels.data(), width, height, banded_size);
        REQUIRE(size_t(serial_size) == size);
        REQUIRE(banded_size == serial_size);
        CHECK(banded == serial);
        CHECK(pixels == original);
    }
}

#include "../../deps_src/imgui/imgui.h"
#include <atomic>
#include <cstring>
#include <filesystem>
#include <thread>

namespace {
struct FontAllocatorState {
    ImGuiMemAllocFunc allocate;
    ImGuiMemFreeFunc release;
    void* user;
    std::thread::id owner = std::this_thread::get_id();
    std::atomic<bool> wrong_thread{false};
    std::atomic<unsigned> calls{0};
};
void* tracked_font_alloc(size_t bytes, void* user)
{
    auto& state = *static_cast<FontAllocatorState*>(user);
    if (std::this_thread::get_id() != state.owner) state.wrong_thread.store(true);
    state.calls.fetch_add(1);
    return state.allocate(bytes, state.user);
}
void tracked_font_free(void* ptr, void* user)
{
    auto& state = *static_cast<FontAllocatorState*>(user);
    if (std::this_thread::get_id() != state.owner) state.wrong_thread.store(true);
    state.release(ptr, state.user);
}
struct FontAllocatorGuard {
    FontAllocatorState& state;
    explicit FontAllocatorGuard(FontAllocatorState& s, bool custom) : state(s)
    {
        if (custom) ImGui::SetAllocatorFunctions(tracked_font_alloc, tracked_font_free, &state);
    }
    ~FontAllocatorGuard() { ImGui::SetAllocatorFunctions(state.allocate, state.release, state.user); }
};
struct FontContextGuard {
    ImGuiContext* context = ImGui::CreateContext();
    ~FontContextGuard() { ImGui::DestroyContext(context); }
};
struct FontAtlasSnapshot {
    int width = 0, height = 0;
    std::vector<unsigned char> pixels;
    std::vector<uint32_t> glyphs;
};
}

TEST_CASE("Font rasterization preserves pixels and keeps custom allocators on the caller", "[FontAtlasRasterization]")
{
    const int oversample = GENERATE(1, 3);
    const int width = GENERATE(512, 4096);
    FontContextGuard context;
    FontAllocatorState state;
    ImGui::GetAllocatorFunctions(&state.allocate, &state.release, &state.user);
    const auto directory = std::filesystem::path(PROFILES_DIR).parent_path() / "fonts";
    const auto snapshot = [&](bool custom) {
        FontAtlasSnapshot result;
        const int active_before = ImGui::GetIO().MetricsActiveAllocations;
        {
            FontAllocatorGuard allocator(state, custom);
            ImFontAtlas atlas;
            atlas.Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
            atlas.TexDesiredWidth = width;
            atlas.AddCustomRectRegular(17, 19);
            ImFontConfig cfg;
            cfg.OversampleH = oversample;
            cfg.OversampleV = oversample == 3 ? 2 : 1;
            cfg.RasterizerMultiply = 1.15f;
            for (const char* name : {"HarmonyOS_Sans_SC_Regular.ttf", "HarmonyOS_Sans_SC_Bold.ttf"})
                REQUIRE(atlas.AddFontFromFileTTF((directory / name).string().c_str(), 22.0f, &cfg, atlas.GetGlyphRangesDefault()) != nullptr);
            REQUIRE(atlas.Build());
            result.width = atlas.TexWidth;
            result.height = atlas.TexHeight;
            result.pixels.assign(atlas.TexPixelsAlpha8, atlas.TexPixelsAlpha8 + size_t(atlas.TexWidth) * atlas.TexHeight);
            for (const auto* font : atlas.Fonts) {
                result.glyphs.push_back(static_cast<uint32_t>(font->Glyphs.Size));
                for (const auto& glyph : font->Glyphs) {
                    result.glyphs.push_back(glyph.Codepoint);
                    for (float value : {glyph.AdvanceX, glyph.X0, glyph.Y0, glyph.X1, glyph.Y1, glyph.U0, glyph.V0, glyph.U1, glyph.V1}) {
                        uint32_t bits;
                        std::memcpy(&bits, &value, sizeof(bits));
                        result.glyphs.push_back(bits);
                    }
                }
            }
        }
        CHECK(ImGui::GetIO().MetricsActiveAllocations == active_before);
        return result;
    };
    const auto normal = snapshot(false);
    const auto custom = snapshot(true);
    CHECK(normal.width == custom.width);
    CHECK(normal.height == custom.height);
    CHECK(normal.pixels == custom.pixels);
    CHECK(normal.glyphs == custom.glyphs);
    CHECK_FALSE(state.wrong_thread.load());
    CHECK(state.calls.load() > 0);
}
