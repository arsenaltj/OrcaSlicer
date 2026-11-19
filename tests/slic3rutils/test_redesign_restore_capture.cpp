#include "slic3r/GUI/Redesign/RedesignRestorePixels.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <stdexcept>
#include <string>

using namespace Slic3r::GUI::RestoreCapture;

TEST_CASE("Capture bounds reject overflow and invalid physical extents", "[RestoreCapture]")
{
    Bounds output;
    CHECK(make_bounds(-300, -200, 20, 30, output));
    CHECK(output == Bounds{-300, -200, -280, -170});
    CHECK_FALSE(make_bounds(std::numeric_limits<int>::max(), 0, 1, 1, output));
    CHECK_FALSE(make_bounds(0, std::numeric_limits<int>::max(), 1, 1, output));
    CHECK_FALSE(make_bounds(0, 0, 0, 1, output));
    std::size_t bytes = 7;
    CHECK_FALSE(rgba_bytes({}, bytes));
    CHECK(bytes == 7);
    CHECK_FALSE(rgba_bytes({std::numeric_limits<int>::min(), 0, std::numeric_limits<int>::max(), 1}, bytes));
    CHECK_FALSE(rgba_bytes({0, 0, 2, 2}, bytes, 15));
    CHECK(rgba_bytes({-2, -2, 0, 0}, bytes, 16));
    CHECK(bytes == 16);
}

TEST_CASE("Batch capture budgets count the union bitmap and retained output capacity", "[RestoreCapture]")
{
    Plan plan;
    REQUIRE(plan.include({{0, 0, 4, 4}, {1, 1, 3, 3}}, 0, 80));
    CHECK(plan.output_bytes == 64);
    CHECK(plan.bitmap_bytes == 16);
    CHECK_FALSE(plan.include({{3, 0, 7, 4}, {3, 1, 5, 3}}, 0, 80));
    CHECK(plan.error == PlanError::BudgetExceeded);
    CHECK(plan.controls == 1);
    CHECK_FALSE(plan.include({{0, 0, 1, 1}, {0, 0, 1, 1}}));
    Plan capacity;
    CHECK_FALSE(capacity.include({{0, 0, 1, 1}, {0, 0, 1, 1}}, 256, 256));
    Plan empty;
    CHECK_FALSE(empty.include({{0, 0, 4, 4}, {}}));
    CHECK(empty.error == PlanError::InvalidRegion);
}

TEST_CASE("Memory crops preserve screen rows and fill clipped control pixels", "[RestoreCapture]")
{
    const auto width = GENERATE(1, 4, 9);
    const auto height = GENERATE(1, 3, 7);
    const auto shift_x = GENERATE(-3, 0, 3);
    const auto shift_y = GENERATE(-2, 0, 2);
    const Bounds capture{-3, -2, 5, 4};
    const Bounds client{shift_x - 4, shift_y - 3, shift_x - 4 + width, shift_y - 3 + height};
    const auto visible = intersect(capture, client);
    if (!contains(capture, visible)) return;
    std::vector<unsigned char> source(8 * 6 * 4);
    for (int y = -2; y < 4; ++y) for (int x = -3; x < 5; ++x) {
        const auto offset = ((3 - y) * 8 + x + 3) * 4;
        source[offset] = static_cast<unsigned char>(x + 21);
        source[offset + 1] = static_cast<unsigned char>(y + 31);
        source[offset + 2] = 77;
    }
    std::vector<unsigned char> output;
    REQUIRE(copy_region(source.data(), source.size(), capture, {client, visible}, 0x00332211, output));
    REQUIRE(output.size() == std::size_t(width * height * 4));
    // This oracle works in screen coordinates rather than the production
    // routine's source/destination row offsets.
    for (int y = client.top; y < client.bottom; ++y) for (int x = client.left; x < client.right; ++x) {
        const auto offset = ((client.bottom - 1 - y) * width + x - client.left) * 4;
        const bool covered = x >= visible.left && x < visible.right && y >= visible.top && y < visible.bottom;
        CHECK(output[offset] == (covered ? static_cast<unsigned char>(x + 21) : 0x11));
        CHECK(output[offset + 1] == (covered ? static_cast<unsigned char>(y + 31) : 0x22));
        CHECK(output[offset + 2] == (covered ? 77 : 0x33));
        CHECK(output[offset + 3] == 0);
    }
    const auto unchanged = output;
    CHECK_FALSE(copy_region(source.data(), 1, capture, {client, visible}, 0, output));
    CHECK(output == unchanged);
    CHECK_FALSE(copy_region(source.data(), source.size(), capture, {client, {}}, 0, output));
    CHECK(output == unchanged);
}

namespace {
struct FakeGdi {
    using DC = int; using Bitmap = int; using Object = int;
    int failure{0}, copies{0}, flushes{0};
    unsigned char pixel[4]{};
    std::string releases;
    DC screen() { return failure == 1 ? 0 : 1; }
    DC memory(DC) { return failure == 2 ? 0 : 2; }
    Bitmap bitmap(DC, int, int, void** bits) {
        *bits = failure == 4 ? nullptr : pixel;
        return failure == 3 ? 0 : 3;
    }
    Object select(DC, Object value) {
        if (value == 4) { releases += "restore,"; return 3; }
        return failure == 5 ? 0 : 4;
    }
    bool valid_selection(Object value) { return value != 0; }
    bool copy(DC, DC, const Bounds&) {
        ++copies;
        if (failure == 8) throw std::runtime_error("copy");
        return failure != 6;
    }
    bool flush() { ++flushes; return failure != 7; }
    void delete_bitmap(Bitmap) { releases += "bitmap,"; }
    void delete_dc(DC) { releases += "memory,"; }
    void release_screen(DC) { releases += "screen,"; }
};
}

TEST_CASE("One capture attempt releases each acquired GDI resource in reverse order", "[RestoreCapture]")
{
    const auto failure = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    const std::string expected[]{"restore,bitmap,memory,screen,", "", "screen,", "memory,screen,",
        "bitmap,memory,screen,", "bitmap,memory,screen,", "restore,bitmap,memory,screen,", "restore,bitmap,memory,screen,"};
    FakeGdi api; api.failure = failure;
    {
        ScreenBitmap<FakeGdi> bitmap(api);
        const bool prepared = bitmap.prepare({0, 0, 1, 1});
        CHECK(prepared == (failure == 0 || failure >= 6));
        CHECK(bitmap.data() == nullptr);
        CHECK_FALSE(bitmap.prepare({0, 0, 1, 1}));
        if (prepared) {
            CHECK(bitmap.copy() == (failure != 6));
            CHECK_FALSE(bitmap.copy());
            CHECK(bitmap.data() == nullptr);
            CHECK(bitmap.flush() == (failure == 0));
            CHECK_FALSE(bitmap.flush());
            CHECK((bitmap.data() != nullptr) == (failure == 0));
        }
    }
    CHECK(api.releases == expected[failure]);
    CHECK(api.copies == (failure == 0 || failure >= 6 ? 1 : 0));
    CHECK(api.flushes == (failure == 0 || failure == 7 ? 1 : 0));
}

TEST_CASE("A capture exception also restores the selected GDI object", "[RestoreCapture]")
{
    FakeGdi api; api.failure = 8;
    {
        ScreenBitmap<FakeGdi> bitmap(api);
        REQUIRE(bitmap.prepare({0, 0, 1, 1}));
        CHECK_THROWS_AS(bitmap.copy(), std::runtime_error);
        CHECK_FALSE(bitmap.copy());
    }
    CHECK(api.releases == "restore,bitmap,memory,screen,");
    CHECK(api.flushes == 0);
}

TEST_CASE("Native snapshots require the same live host geometry and DPI", "[RestoreCapture]")
{
    const FrameIdentity identity{1, 2, 3, 100, 200, 96, 96};
    CHECK(identity.matches(identity));
    auto changed = identity; changed.parent = 4; CHECK_FALSE(identity.matches(changed));
    changed = identity; changed.root = 4; CHECK_FALSE(identity.matches(changed));
    changed = identity; changed.window = 0; CHECK_FALSE(identity.matches(changed));
    changed = identity; changed.width = 101; CHECK_FALSE(identity.matches(changed));
    changed = identity; changed.height = 0; CHECK_FALSE(identity.matches(changed));
    changed = identity; changed.dpi_y = 144; CHECK_FALSE(identity.matches(changed));
    changed = identity; changed.dpi_x = 0; CHECK_FALSE(changed.matches(changed));
}

#ifdef __WXMSW__
#include "slic3r/GUI/Redesign/RedesignPaintPanel.hpp"
#include <wx/frame.h>
#include <wx/init.h>
#include <memory>
using namespace Slic3r::GUI;

namespace {
RedesignRestoreSurface& restore_surface(wxWindow* window)
{
    prepare_redesign_restore_background(window);
    DWORD_PTR context = 0;
    REQUIRE(::GetWindowSubclass(window->GetHandle(), redesign_restore_background_proc,
        reinterpret_cast<UINT_PTR>(redesign_restore_background_proc), &context));
    return *reinterpret_cast<RedesignRestoreSurface*>(context);
}
}

TEST_CASE("Native snapshot hosts invalidate after reparenting resizing and destruction", "[RestoreCapture][UiLifecycle]")
{
    wxInitializer init; REQUIRE(init.IsOk());
    auto home = std::make_unique<wxFrame>(nullptr, wxID_ANY, "capture-home");
    auto other = std::make_unique<wxFrame>(nullptr, wxID_ANY, "capture-other");
    auto* child = new wxPanel(home.get(), wxID_ANY, wxDefaultPosition, {40, 30});
    const auto identity = redesign_native_frame_identity(child);
    REQUIRE(identity.matches(identity));
    child->SetClientSize(50, 30);
    CHECK_FALSE(identity.matches(redesign_native_frame_identity(child)));
    child->SetClientSize(40, 30);
    REQUIRE(identity.matches(redesign_native_frame_identity(child)));
    REQUIRE(child->Reparent(other.get()));
    CHECK_FALSE(identity.matches(redesign_native_frame_identity(child)));
    wxWeakRef<wxWindow> weak(child);
    other.reset();
    CHECK(weak.get() == nullptr);
}

TEST_CASE("Hiding a page discards its descendants while minimizing preserves their retained pixels", "[RestoreCapture][UiLifecycle]")
{
    wxInitializer init; REQUIRE(init.IsOk());
    auto frame = std::make_unique<wxFrame>(nullptr, wxID_ANY, "capture-visibility");
    auto* parent = new wxPanel(frame.get());
    auto* child = new wxPanel(parent);
    restore_surface(parent);
    auto& state = restore_surface(child);
    state.pixels = {1, 2, 3, 4};
    REQUIRE(::SetPropW(frame->GetHandle(), L"HonorForge.Iconized", reinterpret_cast<HANDLE>(1)));
    RedesignRestoreCaptureMetrics metrics;
    retain_redesign_window_frames(frame.get(), &metrics);
    CHECK(state.pixels.size() == 4);
    CHECK(metrics.screen_copies == 0);
    CHECK(std::string(metrics.skip_reason) == "iconic");
    ::SendMessage(parent->GetHandle(), WM_SHOWWINDOW, FALSE, 0);
    CHECK(state.pixels.size() == 4);
    ::RemovePropW(frame->GetHandle(), L"HonorForge.Iconized");
    ::SendMessage(parent->GetHandle(), WM_SHOWWINDOW, FALSE, 0);
    CHECK(state.pixels.empty());
    state.pixels = {1, 2, 3, 4};
    ::SendMessage(child->GetHandle(), WM_PAINT, 0, 0);
    CHECK(state.pixels.empty());
}

TEST_CASE("Caption and system entry share one snapshot admission until presentation releases it", "[RestoreCapture][UiLifecycle]")
{
    wxInitializer init; REQUIRE(init.IsOk());
    auto frame = std::make_unique<wxFrame>(nullptr, wxID_ANY, "capture-admission");
    const auto before = redesign_restore_snapshot_count();
    retain_redesign_restore_frames_once(frame.get(), "maximize", "caption");
    retain_redesign_restore_frames_once(frame.get(), "maximize", "system_command");
    CHECK(redesign_restore_snapshot_count() == before + 1);
    release_redesign_restore_frames(frame.get());
    retain_redesign_restore_frames_once(frame.get(), "restore", "caption");
    CHECK(redesign_restore_snapshot_count() == before + 2);
    release_redesign_restore_frames(frame.get());
}
#endif
