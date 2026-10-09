#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/GUI/AI/ModelGeneration/ModelGenerationPresentation.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelGenerationLegacyState.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelLibraryThumbnail.hpp"
#include "slic3r/GUI/AIModelOutputDirectory.hpp"
#include "slic3r/GUI/AIModelGenerationHttpError.hpp"
#include "test_utils.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelGenerationImageInput.hpp"

#include <boost/nowide/cstdlib.hpp>
#include <boost/filesystem/fstream.hpp>
#include <nlohmann/json.hpp>
#include <wx/image.h>
#include <wx/imagpng.h>
#include <wx/imagjpeg.h>
#include <wx/log.h>
#include <wx/spinctrl.h>

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>

using Slic3r::GUI::AIModelGenerationClient;
using namespace Slic3r::GUI::ModelGenerationPresentation;

TEST_CASE("Hidden print settings retain client defaults without a page",
          "[ModelGenerationPresentation][ModelGenerationLegacyState]")
{
    const Slic3r::GUI::ModelGenerationLegacyState state;
    const AIModelGenerationClient::ImagePrintSettings defaults;
    CHECK(state.print_width_mm == defaults.width_mm);
    CHECK(state.nozzle_mm == defaults.nozzle_mm);
    CHECK(state.line_width_mm == defaults.line_width_mm);
    CHECK(state.minimum_feature_mm == defaults.minimum_feature_mm);
}

#ifdef __WXMSW__
TEST_CASE("Historical print numbers retain native precision and range semantics",
          "[ModelGenerationPresentation][ModelGenerationLegacyState]")
{
    // The linked wx implementation supplies the independent numeric oracle.
    // No Create(), parent, native window or GUI input is involved.
    struct NativeNumber : wxSpinCtrlDouble {
        double restored(double value, double minimum, double maximum, unsigned digits) {
            m_min = minimum;
            m_max = maximum;
            m_digits = digits;
            m_snap_to_ticks = false;
            const double adjusted = AdjustAndSnap(value);
            double stored = adjusted;
            if (!DoTextToValue(DoValueToText(adjusted), &stored)) stored = adjusted;
            return stored;
        }
    } native;
    Slic3r::GUI::ModelGenerationLegacyState state;
    const auto check = [&](double width, double nozzle, double line, double feature) {
        state.restore_print_settings(width, nozzle, line, feature);
        const double actual[] = {state.print_width_mm, state.nozzle_mm, state.line_width_mm, state.minimum_feature_mm};
        const double expected[] = {native.restored(width, 20., 2000., 1), native.restored(nozzle, .1, 2., 2),
                                   native.restored(line, .1, 3., 2), native.restored(feature, .1, 20., 2)};
        for (size_t i = 0; i < 4; ++i) {
            INFO("field=" << i << " input=" << width << "," << nozzle << "," << line << "," << feature);
            if (std::isnan(expected[i])) CHECK(std::isnan(actual[i]));
            else CHECK(actual[i] == expected[i]); // Exact restored double preserves history comparisons/JSON.
        }
    };
    check(160., .4, .4, .8);
    check(20., .1, .1, .1);
    check(2000., 2., 3., 20.);
    check(-1e100, -1., -1., -1.);
    check(1e100, 10., 10., 100.);
    for (int i = 0; i < 2001; ++i)
        check(20.05 + i * .99, .105 + i * .00094, .105 + i * .0014, .105 + i * .00994);
    for (double special : {std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()})
        check(special, special, special, special);
    check(123.456, .456, .456, .876); // Increments must not snap to 10/.1/.05 ticks.
    check(160., .4, .4, .8); // A later restore must replace all four old values.
}
#endif

TEST_CASE("Restoring old target color counts keeps all supported counts and recovers from invalid history",
          "[ModelGenerationPresentation][ModelGenerationLegacyState]")
{
    Slic3r::GUI::ModelGenerationLegacyState state;
    CHECK(state.palette_color_count == AIModelGenerationClient::JobStatus{}.palette_color_count);
    for (size_t count = Slic3r::AI::kMinTargetPaletteColors;
         count <= Slic3r::AI::kMaxTargetPaletteColors; ++count) {
        DYNAMIC_SECTION("Restoring " << count << " target colors") {
            state.palette_source = 2;
            state.restore_palette_color_count(count);
            CHECK(state.palette_color_count == count);
            CHECK(state.palette_source == 2);
            for (size_t invalid : {size_t(0), Slic3r::AI::kMaxTargetPaletteColors + 1,
                                   std::numeric_limits<size_t>::max()}) {
                state.restore_palette_color_count(invalid);
                CHECK(state.palette_color_count == Slic3r::AI::kLegacyDefaultTargetPaletteColors);
                state.restore_palette_color_count(count);
                CHECK(state.palette_color_count == count);
            }
        }
    }
}

TEST_CASE("Pre-encoded history field writes identical JSON and rejects ambiguous input",
          "[ModelGenerationPresentation][BeautyPersistence]")
{
    ScopedTemporaryFile output(".json");
    const nlohmann::json workbench = {
        {"geometry_id", "geometry/1"},
        {"puzzle", {{"piece_runs", nlohmann::json::array({{0, 12}, {3, 27}})}}},
        {"note", "雪人 \\\" 保存"}
    };
    const nlohmann::json metadata = {
        {"ai_image_path", "images/scene \\\"one\\\".png"},
        {"finishing", {{"beauty_puzzle", true}}},
        {"schema_version", 4},
        {"use_printable_colors", false}
    };
    auto expected = metadata;
    expected["beauty_workbench"] = workbench;
    REQUIRE(write_json_with_preencoded_field(output.path(), metadata,
                                             "beauty_workbench", workbench.dump()));
    boost::filesystem::ifstream stream(output.path());
    const std::string actual((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    CHECK(actual == expected.dump());
    CHECK(read_json(output.path()) == expected);
    CHECK_FALSE(write_json_with_preencoded_field(output.path(), expected,
                                                 "beauty_workbench", workbench.dump()));
    CHECK_FALSE(write_json_with_preencoded_field(output.path(), metadata, "beauty_workbench", ""));
    CHECK_FALSE(write_json_with_preencoded_field(output.path(), nlohmann::json::array(),
                                                 "beauty_workbench", workbench.dump()));
    boost::filesystem::ifstream unchanged(output.path());
    CHECK(std::string((std::istreambuf_iterator<char>(unchanged)), std::istreambuf_iterator<char>()) == actual);

}

TEST_CASE("previewable unchecked models never acquire a structural verdict", "[ModelGenerationPresentation]")
{
    AIModelGenerationClient::ModelQuality quality;
    quality.status = "pass";
    CHECK(model_check_stage(true, false, false, quality) == ModelCheckStage::Unchecked);
    quality.available = true;
    CHECK(model_check_stage(false, false, false, quality) == ModelCheckStage::NeedsModel);
}

TEST_CASE("a failed or retrying model check hides an earlier successful verdict", "[ModelGenerationPresentation]")
{
    AIModelGenerationClient::ModelQuality quality;
    quality.available = true;
    quality.status = "pass";
    CHECK(model_check_stage(true, false, true, quality) == ModelCheckStage::Failed);
    CHECK(model_check_stage(true, true, true, quality) == ModelCheckStage::Checking);
    CHECK(model_check_stage(true, false, false, quality) == ModelCheckStage::Passed);
}

TEST_CASE("model check results require a recognized structural outcome", "[ModelGenerationPresentation]")
{
    AIModelGenerationClient::ModelQuality quality;
    quality.available = true;
    for (const auto* outcome : {"", "unavailable", "unknown"}) {
        quality.status = outcome;
        CHECK(model_check_stage(true, false, false, quality) == ModelCheckStage::Failed);
    }
    quality.status = "review";
    CHECK(model_check_stage(true, false, false, quality) == ModelCheckStage::Review);
    quality.status = "reject";
    CHECK(model_check_stage(true, false, false, quality) == ModelCheckStage::Rejected);
}

TEST_CASE("workspace navigation keeps the model destination available without a model", "[ModelGenerationPresentation]")
{
    auto view = WorkspaceView::Image;
    for (auto action : {WorkspaceAction::ShowLibrary, WorkspaceAction::ShowImage, WorkspaceAction::Prepare}) {
        const auto next = workspace_destination(action, view, false);
        if (action == WorkspaceAction::ShowLibrary) CHECK(next == WorkspaceView::Library);
        if (action == WorkspaceAction::ShowImage) CHECK(next == WorkspaceView::Image);
        if (action == WorkspaceAction::Prepare) CHECK(next == view);
        view = next;
    }
    CHECK(workspace_destination(WorkspaceAction::ShowModel, WorkspaceView::Library, false) == WorkspaceView::Model);
    CHECK(workspace_destination(WorkspaceAction::ShowModel, WorkspaceView::Library, true) == WorkspaceView::Model);
}

TEST_CASE("workspace projection keeps active work visible without owning a task", "[ModelGenerationPresentation]")
{
    for (auto view : {WorkspaceView::Image, WorkspaceView::Library, WorkspaceView::Model}) {
        const auto state = workspace_presentation(view, true, true, true, false, false, true);
        CHECK(state.view == view);
        CHECK_FALSE(state.welcome);
        CHECK(state.journey);
        CHECK(state.model_enabled);
        CHECK(state.prepare_enabled);
    }
    const auto empty = workspace_presentation(WorkspaceView::Model, false, false, false, false, false, false);
    CHECK(empty.view == WorkspaceView::Model);
    CHECK_FALSE(empty.welcome);
    CHECK_FALSE(empty.model_enabled);
    CHECK_FALSE(empty.prepare_enabled);
    CHECK_FALSE(workspace_presentation(WorkspaceView::Library, false, false, false, false, false, true).welcome);
    CHECK(workspace_presentation(WorkspaceView::Image, false, false, false, false, false, true).welcome);
    const auto waiting = workspace_presentation(WorkspaceView::Model, true, false, true, false, false, true);
    CHECK(waiting.view == WorkspaceView::Model);
    CHECK(waiting.journey);
    CHECK_FALSE(waiting.model_enabled);
    CHECK_FALSE(waiting.welcome);
}

TEST_CASE("Design wait shows elapsed time without enough history", "[ModelGenerationPresentation][DesignGenerationTiming]")
{
    DesignGenerationWait wait;
    wait.synchronize("job-a", -1, 0, 100);
    CHECK(wait.message(112) == wxString::FromUTF8("已等待 12 秒"));
}

TEST_CASE("Design estimate switches to overtime instead of a false completion", "[ModelGenerationPresentation][DesignGenerationTiming]")
{
    DesignGenerationWait wait;
    wait.synchronize("job-a", 15, 60, 100);
    CHECK(wait.message(100) == wxString::FromUTF8("预计还需约 45 秒"));
    CHECK(wait.message(145) == wxString::FromUTF8("生成时间超出预计，已等待 60 秒"));
    CHECK(wait.message(153) == wxString::FromUTF8("生成时间超出预计，已等待 68 秒"));
}

TEST_CASE("Polling and returning to a page preserve the design clock", "[ModelGenerationPresentation][DesignGenerationTiming]")
{
    DesignGenerationWait wait;
    wait.synchronize("", -1, 0, 100);
    wait.synchronize("job-a", 1, 60, 105);
    CHECK(wait.elapsed_seconds(105) == 5);
    wait.synchronize("job-a", 4, 60, 110);
    CHECK(wait.elapsed_seconds(110) == 10);
    CHECK(wait.elapsed_seconds(140) == 40);
}

TEST_CASE("Ending or replacing a design resets its wait state", "[ModelGenerationPresentation][DesignGenerationTiming]")
{
    DesignGenerationWait wait;
    wait.synchronize("job-a", 40, 60, 100);
    wait.synchronize("job-b", 2, 0, 110);
    CHECK(wait.elapsed_seconds(110) == 2);
    wait.clear();
    CHECK_FALSE(wait.active());
    CHECK(wait.message(120).empty());
    wait.synchronize("", -1, 0, 130);
    CHECK(wait.elapsed_seconds(131) == 1);
}

TEST_CASE("sidecar restart authentication is recoverable without retrying provider failures",
          "[ModelGenerationPresentation][SidecarRecovery]")
{
    CHECK(is_transient_sidecar_poll_error("AI sidecar is not reachable."));
    CHECK(is_transient_sidecar_poll_error("AI sidecar request timed out."));
    CHECK(is_transient_sidecar_poll_error("A valid OrcaSlicer AI session is required."));
    CHECK(is_transient_sidecar_poll_error("Model generation request failed with HTTP 401."));
    CHECK_FALSE(is_transient_sidecar_poll_error("Model job not found."));
    CHECK_FALSE(is_transient_sidecar_poll_error("Tripo authentication failed."));
    CHECK_FALSE(is_transient_sidecar_poll_error("Model generation request failed with HTTP 400."));
}


TEST_CASE("Unavailable status queries do not declare the saved generation failed",
          "[ModelGenerationPresentation][SidecarRecovery]")
{
    using Slic3r::GUI::model_generation_http_error;
    const unsigned status = GENERATE(408u, 429u, 500u, 502u, 503u, 504u, 599u);
    const std::string structured = R"({"error":{"message":"Saved-job lookup is temporarily unavailable."}})";
    // A GET failure is an unavailable observation, even when it has a JSON message.
    const auto query_error = model_generation_http_error(structured, {}, status, true);
    CHECK(is_transient_sidecar_poll_error(query_error));
    CHECK(query_error.find(std::to_string(status)) != std::string::npos);
    // A returned provider/preflight rejection remains distinct from a failed GET.
    CHECK(model_generation_http_error(structured, {}, status) ==
          "Saved-job lookup is temporarily unavailable.");
    CHECK_FALSE(is_transient_sidecar_poll_error(model_generation_http_error(structured, {}, status)));
}

TEST_CASE("Ambiguous HTTP submissions resume queries without hiding definite rejections",
          "[ModelGenerationPresentation][SidecarRecovery]")
{
    using Slic3r::GUI::model_generation_http_error;
    for (unsigned status : {408u, 429u, 500u, 501u, 502u, 503u, 504u, 599u}) {
        INFO("HTTP response without an authoritative job: " << status);
        CHECK(is_transient_sidecar_poll_error(model_generation_http_error({}, {}, status)));
    }
    for (unsigned status : {400u, 403u, 404u, 409u, 422u, 499u, 600u}) {
        INFO("Definite or unsupported response: " << status);
        CHECK_FALSE(is_transient_sidecar_poll_error(model_generation_http_error({}, {}, status)));
        CHECK_FALSE(is_transient_sidecar_poll_error(model_generation_http_error({}, {}, status, true)));
    }
    const std::string missing = R"({"error":{"message":"Model job not found."}})";
    CHECK(model_generation_http_error(missing, {}, 404, true) == "Model job not found.");
    CHECK_FALSE(is_transient_sidecar_poll_error(model_generation_http_error(missing, {}, 404, true)));
    const std::string provider = R"({"error":"Tripo authentication failed."})";
    CHECK_FALSE(is_transient_sidecar_poll_error(model_generation_http_error(provider, {}, 500)));
    CHECK(is_transient_sidecar_poll_error(model_generation_http_error({}, {}, 401, true)));
    CHECK_FALSE(is_transient_sidecar_poll_error("Model generation request failed with HTTP 503. Provider denied it."));
    CHECK_FALSE(is_transient_sidecar_poll_error("Model generation request failed with HTTP 50x."));
}

TEST_CASE("Structured submission errors retain recovery codes without changing status query recovery",
          "[ModelGenerationPresentation][ModelGenerationSubmissionState][SidecarRecovery]")
{
    using Slic3r::GUI::model_generation_http_error;
    const std::string code = GENERATE(std::string("provider_upload_unavailable"),
        std::string("provider_upload_timeout"), std::string("provider_upload_rate_limited"),
        std::string("provider_upload_failed"), std::string("provider_ambiguous"));
    const nlohmann::json response = {{"error", {{"code", code}, {"message", "Provider request did not complete."}}}};
    CHECK(model_generation_http_error(response.dump(), {}, 503) == code + ": Provider request did not complete.");
    const auto query_error = model_generation_http_error(response.dump(), {}, 503, true);
    CHECK(query_error == "AI sidecar request failed with HTTP 503.");
    CHECK(is_transient_sidecar_poll_error(query_error));
    CHECK(model_generation_http_error(response.dump(), {}, 401) == "A valid OrcaSlicer AI session is required.");
}

TEST_CASE("Malformed structured error fields preserve useful diagnostics without throwing",
          "[ModelGenerationPresentation][SidecarRecovery]")
{
    using Slic3r::GUI::model_generation_http_error;
    for (const nlohmann::json& code : {nlohmann::json(42), nlohmann::json(nullptr),
            nlohmann::json(std::string(65, 'x')), nlohmann::json("provider_upload_failed:\n")}) {
        const nlohmann::json response = {{"error", {{"code", code}, {"message", "Upload was rejected."}}}};
        CHECK(model_generation_http_error(response.dump(), {}, 400) == "Upload was rejected.");
    }
    for (const nlohmann::json& message : {nlohmann::json(42), nlohmann::json(nullptr), nlohmann::json("")}) {
        const nlohmann::json response = {{"error", {{"code", "provider_upload_failed"}, {"message", message}}}};
        CHECK(model_generation_http_error(response.dump(), {}, 400) ==
              "provider_upload_failed: Model generation request failed.");
    }
    CHECK(model_generation_http_error(R"({"error":"Legacy provider rejection."})", {}, 400) ==
          "Legacy provider rejection.");
}

namespace {

class ScopedEnvironmentValue
{
public:
    ScopedEnvironmentValue(const char* name, const char* value) : m_name(name)
    {
        if (const char* previous = boost::nowide::getenv(name)) m_previous = previous;
        const int result = value != nullptr ? boost::nowide::setenv(name, value, 1)
                                             : boost::nowide::unsetenv(name);
        if (result != 0) throw std::runtime_error("Unable to set test environment variable");
    }
    ~ScopedEnvironmentValue()
    {
        if (m_previous) boost::nowide::setenv(m_name.c_str(), m_previous->c_str(), 1);
        else boost::nowide::unsetenv(m_name.c_str());
    }

private:
    std::string m_name;
    std::optional<std::string> m_previous;
};

class ScopedWorkingDirectory
{
public:
    explicit ScopedWorkingDirectory(const boost::filesystem::path& directory)
        : m_previous(boost::filesystem::current_path())
    {
        boost::filesystem::current_path(directory);
    }
    ~ScopedWorkingDirectory()
    {
        boost::system::error_code ignored;
        boost::filesystem::current_path(m_previous, ignored);
    }

private:
    boost::filesystem::path m_previous;
};

nlohmann::json write_design_fixture(const boost::filesystem::path& directory)
{
    boost::filesystem::create_directories(directory);
    if (wxImage::FindHandler(wxBITMAP_TYPE_PNG) == nullptr) wxImage::AddHandler(new wxPNGHandler());
    wxImage image(64, 64);
    image.SetRGB(wxRect(0, 0, 64, 64), 24, 128, 200);
    REQUIRE(image.SaveFile((directory / "preview.png").wstring(), wxBITMAP_TYPE_PNG));
    boost::filesystem::copy_file(directory / "preview.png", directory / "input.png");
    nlohmann::json record = {
        {"version", 1}, {"id", directory.filename().string()}, {"source", "image"},
        {"state", "awaiting_confirmation"}, {"user_prompt", "cat with a blue scarf"},
        {"input_path", "input.png"}, {"preview_path", "preview.png"},
        {"raw_preview_path", "preview.png"}, {"updated_at", 1789132000.25}
    };
    REQUIRE(write_json(directory / "job.json", record));
    return record;
}

} // namespace

TEST_CASE("Local image selection accepts decodable PNG and JPEG within the existing limits",
          "[ModelGenerationPresentation][ImageSelection]")
{
    ScopedTemporaryDir temporary("orca-image-selection");
    if (wxImage::FindHandler(wxBITMAP_TYPE_PNG) == nullptr) wxImage::AddHandler(new wxPNGHandler());
    if (wxImage::FindHandler(wxBITMAP_TYPE_JPEG) == nullptr) wxImage::AddHandler(new wxJPEGHandler());

    const auto png = temporary.path() / "reference.png";
    const auto jpeg = temporary.path() / "reference.jpeg";
    const auto small_image_path = temporary.path() / "small.png";
    const auto truncated = temporary.path() / "truncated.png";
    const auto oversized = temporary.path() / "oversized.png";
    wxImage image(64, 64);
    image.SetRGB(wxRect(0, 0, 64, 64), 24, 128, 200);
    REQUIRE(image.SaveFile(png.wstring(), wxBITMAP_TYPE_PNG));
    REQUIRE(image.SaveFile(jpeg.wstring(), wxBITMAP_TYPE_JPEG));
    image.Rescale(63, 64);
    REQUIRE(image.SaveFile(small_image_path.wstring(), wxBITMAP_TYPE_PNG));
    const unsigned char signature[] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    {
        boost::filesystem::ofstream stream(truncated, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(signature), sizeof(signature));
    }
    boost::filesystem::copy_file(png, oversized);
    boost::filesystem::resize_file(oversized, 20 * 1024 * 1024 + 1);

    CHECK(is_supported_image(png));
    CHECK(is_supported_image(jpeg));
    CHECK_FALSE(is_supported_image(small_image_path));
    wxLogNull quiet;
    CHECK_FALSE(is_supported_image(truncated));
    CHECK_FALSE(is_supported_image(oversized));
    CHECK_FALSE(is_supported_image(temporary.path() / "missing.png"));
}

TEST_CASE("History listing defers decoding without weakening design recovery validation",
          "[ModelGenerationPresentation][DesignHistory]")
{
    ScopedTemporaryDir temporary("orca-history-listing");
    const std::string id = "33333333-3333-4333-8333-333333333333";
    const auto directory = temporary.path() / id;
    write_design_fixture(directory);
    // A recognizable but truncated image may be shown as an unavailable
    // thumbnail. Reopening it must still fail strict validation.
    const unsigned char signature[] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    {
        boost::filesystem::ofstream file(directory / "preview.png", std::ios::binary);
        file.write(reinterpret_cast<const char*>(signature), sizeof(signature));
    }
    REQUIRE(read_design_history_entry(temporary.path(), id, false).has_value());
    wxLogNull quiet;
    CHECK_FALSE(read_design_history_entry(temporary.path(), id).has_value());
    boost::filesystem::remove(directory / "preview.png");
    CHECK_FALSE(read_design_history_entry(temporary.path(), id, false).has_value());
}

TEST_CASE("History thumbnails preserve aspect and source colors at each display scale",
          "[ModelGenerationPresentation][HistoryThumbnails]")
{
    using namespace Slic3r::GUI;
    ScopedTemporaryDir temporary("orca-history-thumbnail");
    if (wxImage::FindHandler(wxBITMAP_TYPE_PNG) == nullptr) wxImage::AddHandler(new wxPNGHandler());
    if (wxImage::FindHandler(wxBITMAP_TYPE_JPEG) == nullptr) wxImage::AddHandler(new wxJPEGHandler());
    wxImage source(128, 64);
    source.SetRGB(wxRect(0, 0, 128, 64), 32, 128, 224);
    const auto png = temporary.path() / boost::filesystem::path(L"历史设计.png");
    const auto jpeg = temporary.path() / "design.jpg";
    REQUIRE(source.SaveFile(png.wstring(), wxBITMAP_TYPE_PNG));
    REQUIRE(source.SaveFile(jpeg.wstring(), wxBITMAP_TYPE_JPEG));
    std::atomic<bool> cancelled {false};
    ModelLibraryThumbnailCache cache;
    for (const int edge : {96, 144, 192}) {
        const auto thumbnail = cache.load(png, {}, edge, cancelled);
        REQUIRE(thumbnail.IsOk());
        CHECK(thumbnail.GetWidth() == edge);
        CHECK(thumbnail.GetHeight() == edge / 2);
        CHECK(thumbnail.GetRed(edge / 2, edge / 4) == 32);
        CHECK(thumbnail.GetBlue(edge / 2, edge / 4) == 224);
    }
    const auto jpeg_thumbnail = cache.load(jpeg, {}, 96, cancelled);
    REQUIRE(jpeg_thumbnail.IsOk());
    CHECK(jpeg_thumbnail.GetWidth() == 96);
    CHECK(jpeg_thumbnail.GetHeight() == 48);
    cancelled = true;
    CHECK_FALSE(cache.load(png, jpeg, 96, cancelled).IsOk());
}

TEST_CASE("Late history thumbnails cannot address a replacement page or delay its first images",
          "[ModelGenerationPresentation][HistoryThumbnails]")
{
    struct Receipt { uint64_t revision; size_t index; std::string image; };
    std::vector<std::string> cards {"current-left", "current-right"};
    std::vector<Receipt> receipts {{4, 0, "old-left"}, {4, 1, "old-right"},
        {5, 0, "new-left"}, {5, 99, "removed-card"}, {6, 0, "other-page"},
        {5, 1, "new-right"}, {4, 0, "late-old-left"}};
    size_t publishes = 0;
    const auto deferred = Slic3r::GUI::publish_library_thumbnail_batch(std::move(receipts),
        5, cards.size(), 2, [&](const Receipt& receipt) {
            cards.at(receipt.index) = receipt.image;
            ++publishes;
        });
    CHECK(cards == std::vector<std::string>{"new-left", "new-right"});
    CHECK(publishes == 2);
    CHECK(deferred.empty());
}

TEST_CASE("Deferred history thumbnails keep order across UI ticks and a later filter change",
          "[ModelGenerationPresentation][HistoryThumbnails]")
{
    struct Receipt { uint64_t revision; size_t index; };
    std::vector<size_t> published;
    std::vector<Receipt> receipts {{7, 0}, {7, 1}, {7, 2}, {7, 3}, {7, 4}};
    auto publish = [&](const Receipt& receipt) { published.push_back(receipt.index); };
    auto deferred = Slic3r::GUI::publish_library_thumbnail_batch(std::move(receipts), 7, 5, 2, publish);
    CHECK(published == std::vector<size_t>{0, 1});
    REQUIRE(deferred.size() == 3);
    CHECK(deferred[0].index == 2);
    CHECK(deferred[1].index == 3);
    CHECK(deferred[2].index == 4);
    deferred = Slic3r::GUI::publish_library_thumbnail_batch(std::move(deferred), 7, 5, 2, publish);
    CHECK(published == std::vector<size_t>{0, 1, 2, 3});
    REQUIRE(deferred.size() == 1);
    deferred.push_back({8, 0});
    deferred = Slic3r::GUI::publish_library_thumbnail_batch(std::move(deferred), 8, 1, 2, publish);
    CHECK(published == std::vector<size_t>{0, 1, 2, 3, 0});
    CHECK(deferred.empty());
}

TEST_CASE("History thumbnail cache invalidates stale derivatives and missing source files",
          "[ModelGenerationPresentation][HistoryThumbnails]")
{
    using namespace Slic3r::GUI;
    ScopedTemporaryDir temporary("orca-history-thumbnail-cache");
    if (wxImage::FindHandler(wxBITMAP_TYPE_PNG) == nullptr) wxImage::AddHandler(new wxPNGHandler());
    const auto source = temporary.path() / "source.png";
    const auto display = temporary.path() / "source.png.display.png";
    const auto fallback = temporary.path() / "reference.png";
    wxImage image(64, 64);
    image.SetRGB(wxRect(0, 0, 64, 64), 255, 0, 0);
    REQUIRE(image.SaveFile(source.wstring(), wxBITMAP_TYPE_PNG));
    image.SetRGB(wxRect(0, 0, 64, 64), 0, 255, 0);
    REQUIRE(image.SaveFile(display.wstring(), wxBITMAP_TYPE_PNG));
    image.SetRGB(wxRect(0, 0, 64, 64), 0, 0, 255);
    REQUIRE(image.SaveFile(fallback.wstring(), wxBITMAP_TYPE_PNG));
    const auto time = boost::filesystem::last_write_time(source);
    boost::filesystem::last_write_time(display, time + 2);
    std::atomic<bool> cancelled {false};
    ModelLibraryThumbnailCache cache;
    auto thumbnail = cache.load(source, fallback, 96, cancelled);
    REQUIRE(thumbnail.IsOk());
    CHECK(thumbnail.GetGreen(0, 0) == 255);
    boost::filesystem::last_write_time(source, time + 4);
    thumbnail = cache.load(source, fallback, 96, cancelled);
    REQUIRE(thumbnail.IsOk());
    CHECK(thumbnail.GetRed(0, 0) == 255);
    // Mutating a returned result must not mutate the cache's private image.
    thumbnail.SetRGB(wxRect(0, 0, 96, 96), 0, 0, 0);
    thumbnail = cache.load(source, fallback, 96, cancelled);
    CHECK(thumbnail.GetRed(0, 0) == 255);
    boost::filesystem::remove(source);
    thumbnail = cache.load(source, fallback, 96, cancelled);
    REQUIRE(thumbnail.IsOk());
    CHECK(thumbnail.GetBlue(0, 0) == 255);
    boost::filesystem::remove(fallback);
    CHECK_FALSE(cache.load(source, fallback, 96, cancelled).IsOk());
}

TEST_CASE("Oversized and corrupt history images cannot allocate a full thumbnail decode",
          "[ModelGenerationPresentation][HistoryThumbnails]")
{
    using namespace Slic3r::GUI;
    ScopedTemporaryFile image(".png");
    // 8192 x 8192 is above the bounded worker's pixel limit. Only a header
    // exists, so this also verifies that listing never needs a full decode.
    const unsigned char header[] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10,
        0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 32, 0, 0, 0, 32, 0};
    {
        boost::filesystem::ofstream file(image.path(), std::ios::binary);
        file.write(reinterpret_cast<const char*>(header), sizeof(header));
    }
    std::atomic<bool> cancelled {false};
    bool png = false;
    CHECK(library_image_dimensions(image.path(), png) == wxSize(8192, 8192));
    CHECK(png);
    CHECK(is_library_image_file(image.path()));
    CHECK_FALSE(load_library_thumbnail(image.path(), 96, cancelled).IsOk());
    CHECK_FALSE(load_library_thumbnail(image.path(), 0, cancelled).IsOk());
    CHECK_FALSE(load_library_thumbnail(image.path(), 65536, cancelled).IsOk());
    {
        boost::filesystem::ofstream file(image.path(), std::ios::binary);
        file << "invalid thumbnail";
    }
    CHECK_FALSE(is_library_image_file(image.path()));
    CHECK_FALSE(load_library_thumbnail(image.path(), 96, cancelled).IsOk());
}

TEST_CASE("Design history preserves separate image versions before any model exists",
          "[ModelGenerationPresentation][DesignHistory]")
{
    ScopedTemporaryDir temporary("orca-design-history");
    const std::string first = "11111111-1111-4111-8111-111111111111";
    const std::string second = "22222222-2222-4222-8222-222222222222";
    auto record = write_design_fixture(temporary.path() / first);
    write_design_fixture(temporary.path() / second);
    const auto original = read_design_history_entry(temporary.path(), first);
    REQUIRE(original);
    CHECK(original->job_id == first);
    CHECK(original->prompt == "cat with a blue scarf");
    CHECK(original->state == "awaiting_confirmation");
    CHECK(original->input_path == temporary.path() / first / "input.png");
    CHECK(read_design_history_entry(temporary.path(), second).has_value());
    CHECK(has_persisted_generation_assets(temporary.path(), first));
    record["updated_at"] = 1789233000.875;
    REQUIRE(write_json(temporary.path() / first / "job.json", record));
    const auto restored = read_design_history_entry(temporary.path(), first);
    REQUIRE(restored);
    CHECK(restored->generated_at == original->generated_at);
}

TEST_CASE("Stopped and failed designs retain raw images without becoming confirmed jobs",
          "[ModelGenerationPresentation][DesignHistory]")
{
    ScopedTemporaryDir temporary("orca-stopped-design");
    const std::string id = "11111111-1111-4111-8111-111111111111";
    const auto directory = temporary.path() / id;
    auto record = write_design_fixture(directory);
    record["preview_path"] = nullptr;
    record["input_path"] = nullptr;
    for (const char* state : {"stopped", "failed"}) {
        record["state"] = state;
        REQUIRE(write_json(directory / "job.json", record));
        const auto entry = read_design_history_entry(temporary.path(), id);
        REQUIRE(entry);
        CHECK(entry->state == state);
        CHECK(entry->input_path.empty());
        CHECK(entry->preview_path == directory / "preview.png");
        CHECK(has_persisted_generation_assets(temporary.path(), id));
    }
    record["state"] = "preprocessing";
    REQUIRE(write_json(directory / "job.json", record));
    CHECK_FALSE(read_design_history_entry(temporary.path(), id));
}

TEST_CASE("Design history rejects foreign paths invalid images and malformed records",
          "[ModelGenerationPresentation][DesignHistory]")
{
    ScopedTemporaryDir temporary("orca-design-record-validation");
    const std::string id = "11111111-1111-4111-8111-111111111111";
    const std::string sibling = "22222222-2222-4222-8222-222222222222";
    const auto directory = temporary.path() / id;
    const auto record = write_design_fixture(directory);
    write_design_fixture(temporary.path() / sibling);
    for (const std::string& value : std::vector<std::string>{"../" + sibling + "/preview.png", "/missing.png", "missing.png"}) {
        auto invalid = record;
        invalid["preview_path"] = value;
        invalid["raw_preview_path"] = value;
        REQUIRE(write_json(directory / "job.json", invalid));
        CHECK_FALSE(read_design_history_entry(temporary.path(), id));
    }
    for (const auto& invalid_value : std::vector<nlohmann::json>{7, nullptr, nlohmann::json::array()}) {
        auto invalid = record;
        invalid["state"] = invalid_value;
        REQUIRE(write_json(directory / "job.json", invalid));
        CHECK_FALSE(read_design_history_entry(temporary.path(), id));
    }
    for (const nlohmann::json& patch : std::vector<nlohmann::json>{
             {{"id", sibling}}, {{"version", 2}}, {{"source", "local_finishing"}},
             {{"user_prompt", false}}, {{"user_prompt", std::string(65536, 'x')}}}) {
        auto invalid = record;
        invalid.update(patch);
        REQUIRE(write_json(directory / "job.json", invalid));
        CHECK_FALSE(read_design_history_entry(temporary.path(), id));
    }
    REQUIRE(write_json(directory / "job.json", record));
    boost::filesystem::ofstream(directory / "preview.png", std::ios::binary) << "not an image";
    CHECK_FALSE(read_design_history_entry(temporary.path(), id));
    CHECK_FALSE(has_persisted_generation_assets(temporary.path(), id));
    CHECK_FALSE(read_design_history_entry(temporary.path(), "../" + id));
}

TEST_CASE("Existing model history takes precedence over design entries and remains preserved",
          "[ModelGenerationPresentation][DesignHistory]")
{
    ScopedTemporaryDir temporary("orca-model-before-design");
    const std::string id = "11111111-1111-4111-8111-111111111111";
    const auto directory = temporary.path() / id;
    write_design_fixture(directory);
    boost::filesystem::ofstream(directory / "model-vertex-color.obj") << "v 0 0 0\n";
    CHECK_FALSE(read_design_history_entry(temporary.path(), id));
    CHECK(has_persisted_generation_assets(temporary.path(), id));
    boost::filesystem::remove(directory / "job.json");
    CHECK(has_persisted_generation_assets(temporary.path(), id));
}

TEST_CASE("AI model history follows the Orca installation directory across launch locations",
          "[ModelGenerationPresentation][AIModelOutputDirectory]")
{
    namespace fs = boost::filesystem;
    ScopedTemporaryDir temporary("orca-model-output");
    const fs::path installation = temporary.path() / "Orca installation";
    const fs::path first_launch = temporary.path() / "first launch";
    const fs::path second_launch = temporary.path() / "second launch";
    fs::create_directories(first_launch);
    fs::create_directories(second_launch);
    for (const char* override_value : {static_cast<const char*>(nullptr), ""}) {
        DYNAMIC_SECTION((override_value == nullptr ? "unset override" : "empty override")) {
            ScopedEnvironmentValue environment("ORCASLICER_AI_OUTPUT_DIR", override_value);
            ScopedWorkingDirectory working_directory(first_launch);
            const Slic3r::GUI::AIModelOutputDirectory first(installation);
            fs::current_path(second_launch);
            const Slic3r::GUI::AIModelOutputDirectory second(installation);
            CHECK(first.root() == installation / "models");
            CHECK(second.root() == first.root());
            CHECK_FALSE(fs::exists(first.root()));
            CHECK_FALSE(fs::exists(first_launch / "generated_models"));
        }
    }
}

TEST_CASE("AI model history preserves an explicit absolute output directory",
          "[ModelGenerationPresentation][AIModelOutputDirectory]")
{
    namespace fs = boost::filesystem;
    ScopedTemporaryDir temporary("orca-model-output");
    const fs::path custom_output = temporary.path() / "custom model output";
    ScopedEnvironmentValue environment("ORCASLICER_AI_OUTPUT_DIR", custom_output.string().c_str());
    const Slic3r::GUI::AIModelOutputDirectory directory(temporary.path() / "other user data");
    CHECK(directory.root() == custom_output);
    CHECK_FALSE(fs::exists(directory.root()));
}

TEST_CASE("AI model output remains fixed after working directory and environment changes",
          "[ModelGenerationPresentation][AIModelOutputDirectory]")
{
    namespace fs = boost::filesystem;
    ScopedTemporaryDir temporary("orca-model-output");
    const fs::path parent_start = temporary.path() / "parent start";
    const fs::path child_start = temporary.path() / "bootstrap directory";
    fs::create_directories(parent_start);
    fs::create_directories(child_start);
    ScopedWorkingDirectory working_directory(parent_start);
    ScopedEnvironmentValue environment("ORCASLICER_AI_OUTPUT_DIR", "relative output/../model assets");
    const Slic3r::GUI::AIModelOutputDirectory directory(temporary.path() / "user data");
    // current_path() reflects platform path aliases (for example /var -> /private/var on macOS).
    const fs::path expected = fs::current_path() / "model assets";
    REQUIRE(directory.root().is_absolute());
    REQUIRE(directory.root() == expected);

    fs::current_path(child_start);
    ScopedEnvironmentValue changed_environment("ORCASLICER_AI_OUTPUT_DIR", "different output");
    boost::process::environment child_environment;
    directory.configure_child_environment(child_environment);
    CHECK(directory.root() == expected);
    CHECK(fs::path(child_environment["ORCASLICER_AI_OUTPUT_DIR"].to_string()) == expected);
    CHECK_FALSE(fs::exists(child_start / "model assets"));
}

TEST_CASE("AI model output adds only its explicit path to the child environment",
          "[ModelGenerationPresentation][AIModelOutputDirectory]")
{
    ScopedTemporaryDir temporary("orca-model-output");
    ScopedEnvironmentValue environment("ORCASLICER_AI_OUTPUT_DIR", nullptr);
    ScopedEnvironmentValue unrelated("ORCA_MODEL_OUTPUT_TEST_UNRELATED", "not-for-child");
    const Slic3r::GUI::AIModelOutputDirectory directory(temporary.path());
    boost::process::environment child_environment;
    child_environment["ORCA_MODEL_OUTPUT_TEST_EXISTING"] = "preserved";
    directory.configure_child_environment(child_environment);
    CHECK(child_environment["ORCA_MODEL_OUTPUT_TEST_EXISTING"].to_string() == "preserved");
    CHECK(child_environment["ORCASLICER_AI_OUTPUT_DIR"].to_string() == directory.root().string());
    CHECK(child_environment.find("ORCA_MODEL_OUTPUT_TEST_UNRELATED") == child_environment.end());
}

#ifdef _WIN32
TEST_CASE("AI model paths retain Unicode in the Windows child environment",
          "[ModelGenerationPresentation][AIModelOutputDirectory]")
{
    ScopedTemporaryDir temporary("orca-model-output");
    ScopedEnvironmentValue environment("ORCASLICER_AI_OUTPUT_DIR", nullptr);
    ScopedEnvironmentValue unrelated("ORCA_MODEL_OUTPUT_TEST_UNRELATED", "not-for-child");
    const boost::filesystem::path installation = temporary.path() / L"\u7528\u6237 \u6a21\u578b";
    const Slic3r::GUI::AIModelOutputDirectory directory(installation);
    boost::process::environment inherited;
    inherited["ORCA_MODEL_OUTPUT_TEST_EXISTING"] = "preserved";
    boost::process::wenvironment launch_environment(inherited);
    directory.configure_child_environment(launch_environment);
    CHECK(launch_environment[L"ORCASLICER_AI_OUTPUT_DIR"].to_string() ==
          (installation / "models").wstring());
    CHECK(launch_environment[L"ORCA_MODEL_OUTPUT_TEST_EXISTING"].to_string() == L"preserved");
    CHECK(launch_environment.find(L"ORCA_MODEL_OUTPUT_TEST_UNRELATED") == launch_environment.end());
}
#endif

TEST_CASE("Install-local model storage imports old history without replacing saved versions",
          "[ModelGenerationPresentation][AIModelOutputDirectory]")
{
    namespace fs = boost::filesystem;
    ScopedTemporaryDir temporary("orca-install-model-history");
    ScopedEnvironmentValue environment("ORCASLICER_AI_OUTPUT_DIR", nullptr);
    const fs::path legacy = temporary.path() / "user/generated_models";
    const Slic3r::GUI::AIModelOutputDirectory directory(temporary.path() / "install");
    fs::create_directories(legacy / "downloads");
    fs::create_directories(legacy / "11111111-1111-4111-8111-111111111111");
    fs::ofstream(legacy / "downloads/painted.glb") << "old-saved-colors";
    fs::ofstream(legacy / "11111111-1111-4111-8111-111111111111/job.json") << "{\"artifact_path\":\"model.glb\"}";
    fs::ofstream(legacy / "11111111-1111-4111-8111-111111111111/model.glb") << "original-model";
    fs::create_directories(directory.root() / "downloads");
    fs::ofstream(directory.root() / "downloads/painted.glb") << "new-install-version";
    std::string error;
    REQUIRE(directory.migrate_legacy_history(legacy, error));
    CHECK(error.empty());
    CHECK(fs::file_size(directory.root() / "downloads/painted.glb") == std::string("new-install-version").size());
    CHECK(fs::file_size(legacy / "downloads/painted.glb") == std::string("old-saved-colors").size());
    CHECK(fs::exists(directory.root() / "11111111-1111-4111-8111-111111111111/model.glb"));
    CHECK(fs::exists(directory.root() / "11111111-1111-4111-8111-111111111111/job.json"));
    REQUIRE(directory.migrate_legacy_history(legacy, error));
    CHECK(fs::exists(legacy / "11111111-1111-4111-8111-111111111111/model.glb"));
    CHECK(fs::is_directory(directory.root() / "exports"));
    CHECK(fs::is_directory(directory.root() / "projects"));
}

TEST_CASE("Model history initialization reports an unusable install directory and retains originals",
          "[ModelGenerationPresentation][AIModelOutputDirectory]")
{
    namespace fs = boost::filesystem;
    ScopedTemporaryDir temporary("orca-install-model-failure");
    ScopedEnvironmentValue environment("ORCASLICER_AI_OUTPUT_DIR", nullptr);
    const fs::path legacy = temporary.path() / "user/generated_models";
    fs::create_directories(legacy);
    fs::ofstream(legacy / "model.glb") << "preserved-original";
    fs::ofstream(temporary.path() / "blocked-install") << "a-file";
    const Slic3r::GUI::AIModelOutputDirectory directory(temporary.path() / "blocked-install");
    std::string error;
    CHECK_FALSE(directory.migrate_legacy_history(legacy, error));
    CHECK_FALSE(error.empty());
    CHECK(fs::file_size(legacy / "model.glb") == std::string("preserved-original").size());
}

TEST_CASE("model-generation progress maps service phases to stable UI milestones",
          "[ModelGenerationPresentation]")
{
    AIModelGenerationClient::JobStatus status;
    REQUIRE(status.palette_color_count == Slic3r::AI::kLegacyDefaultTargetPaletteColors);

    status.state = "recommending_palette";
    status.progress = 5;
    REQUIRE(display_progress(status) == 3);
    status.progress = 10;
    REQUIRE(display_progress(status) == 10);

    status.state = "awaiting_palette_confirmation";
    REQUIRE(display_progress(status) == 10);
    status.state = "preprocessing";
    status.progress = 15;
    REQUIRE(display_progress(status) == 25);
    status.state = "awaiting_confirmation";
    REQUIRE(display_progress(status) == 35);

    status.state.clear();
    status.phase = "generating";
    status.progress = 70;
    REQUIRE(display_progress(status) == 78);
    status.phase = "converting";
    status.progress = 95;
    REQUIRE(display_progress(status) == 90);
    status.phase = "downloading_artifact";
    REQUIRE(display_progress(status) == 92);
    status.phase = "checking_model";
    status.progress = 99;
    REQUIRE(display_progress(status) == 97);
    status.phase = "checking_visual";
    REQUIRE(display_progress(status) == 98);

    status.phase.clear();
    status.state = "ready";
    REQUIRE(display_progress(status) == 100);
}

TEST_CASE("Server palette corrections preserve previews without overwriting user edits", "[ModelGenerationPresentation]")
{
    const std::vector<std::string> palette = {"#F4F4F0", "#1F1B1C", "#F2C9AE"};
    const AIModelGenerationClient::PaletteRoles submitted = {
        {"primary", palette[2]}, {"structure", palette[1]}, {"light", palette[0]}};
    const AIModelGenerationClient::PaletteRoles corrected = {
        {"primary", palette[0]}, {"structure", palette[1]}, {"light", palette[2]}};
    auto current = submitted;
    synchronize_palette_roles(palette, current, palette, submitted, palette, corrected);
    CHECK(current == corrected);
    synchronize_palette_roles(palette, current, palette, corrected, palette, corrected);
    CHECK(current == corrected);

    auto edited_palette = palette;
    edited_palette[0] = "#FFFFFF";
    current = submitted;
    synchronize_palette_roles(edited_palette, current, palette, submitted, palette, corrected);
    CHECK(current == submitted);
    synchronize_palette_roles(palette, current, palette, submitted, edited_palette, corrected);
    CHECK(current == submitted);

    auto edited_roles = submitted;
    std::swap(edited_roles["structure"], edited_roles["primary"]);
    current = edited_roles;
    synchronize_palette_roles(palette, current, palette, submitted, palette, corrected);
    CHECK(current == edited_roles);
    synchronize_palette_roles(palette, current, palette, edited_roles, palette, {});
    CHECK(current == edited_roles);
}

TEST_CASE("automatic printable palette roles remain deterministic and distinct",
          "[ModelGenerationPresentation]")
{
    const std::vector<std::string> palette {
        "#000000", "#FFFFFF", "#FF0000", "#00FF00", "#0066FF", "#9933CC"
    };
    for (size_t count = 1; count <= palette.size(); ++count) {
        DYNAMIC_SECTION(count << " colors receive a complete stable role prefix") {
            const std::vector<std::string> active(palette.begin(), palette.begin() + count);
            const AIModelGenerationClient::PaletteRoles roles = automatic_palette_roles(active);
            REQUIRE(roles.size() == count);
            for (size_t index = 0; index < PALETTE_ROLE_IDS.size(); ++index)
                CHECK(roles.count(PALETTE_ROLE_IDS[index]) == (index < count ? 1 : 0));
            std::set<std::string> assigned;
            for (const auto& [role, color] : roles) {
                CHECK(std::find(active.begin(), active.end(), color) != active.end());
                assigned.insert(color);
            }
            CHECK(assigned.size() == count);
            CHECK(automatic_palette_roles(active) == roles);
        }
    }

    const std::vector<std::string> legacy_palette(palette.begin(), palette.begin() + 4);
    const AIModelGenerationClient::PaletteRoles roles = automatic_palette_roles(legacy_palette);
    REQUIRE(roles.at("structure") == "#000000");
    REQUIRE(roles.at("light") == "#FFFFFF");
    REQUIRE(roles.at("primary") == "#00FF00");
    REQUIRE(roles.at("accent") == "#FF0000");
    REQUIRE(same_palette_color("#aBc123", "#AbC123"));
    REQUIRE_FALSE(same_palette_color("#ABC123", "#ABC124"));
    CHECK(automatic_palette_roles({"#000000", "#FFFFFF", "#FF0000", "#00FF00",
                                   "#0066FF", "#9933CC", "#00FFFF"}).empty());
    CHECK(automatic_palette_roles({"#000000", "invalid"}).empty());
    CHECK(automatic_palette_roles({"#000000", "#000000"}).empty());
}

TEST_CASE("style families retain legacy styles in a compact secondary choice",
          "[ModelGenerationPresentation]")
{
    CHECK(style_selection("portrait_sketch") == 2);
    CHECK(style_selection("ink_relief") == 2);
    CHECK(style_selection("sculpture") == 0);
    CHECK(style_selection("realistic") == 1);
    for (const std::string style : {"portrait_sketch", "cartoon", "low_poly", "relief", "ink_relief", "diorama", "custom"}) {
        CHECK(is_supported_style(style));
        CHECK(style_selection(style) == 2);
        CHECK(selected_style(2, stylized_style_selection(style)) == style);
    }
    CHECK(selected_style(0, 5) == "sculpture");
    CHECK(selected_style(1, 5) == "realistic");
    CHECK(selected_style(2, -1) == "cartoon");
    CHECK(style_uses_printable_colors("portrait_sketch"));
    CHECK(style_uses_printable_colors("ink_relief"));
}

TEST_CASE("Multiple preencoded fields preserve key order and reject collisions before writing", "[ModelGenerationPresentation][BeautyPersistence]")
{
    ScopedTemporaryFile output(".json");
    const nlohmann::json metadata {{"middle", true}, {"z", 17}};
    const std::map<std::string, std::string> encoded {
        {"", nlohmann::json("empty-key").dump()},
        {"beauty_puzzle_draft", nlohmann::json{{"future", "雪\n\"draft\""}}.dump()},
        {"beauty_workbench", "null"}, {"zz", "[1,2,3]"}
    };
    auto expected=metadata;
    for(const auto& item:encoded)expected[item.first]=nlohmann::json::parse(item.second);
    REQUIRE(write_json_with_preencoded_fields(output.path(),metadata,encoded));
    const auto bytes=[&] {boost::filesystem::ifstream input(output.path(),std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(input)),{});};
    CHECK(bytes()==expected.dump());
    auto collision=encoded;collision.emplace("middle","false");
    CHECK_FALSE(write_json_with_preencoded_fields(output.path(),metadata,collision));
    CHECK(bytes()==expected.dump());
    auto missing=encoded;missing["beauty_puzzle_draft"].clear();
    CHECK_FALSE(write_json_with_preencoded_fields(output.path(),metadata,missing));
    CHECK(bytes()==expected.dump());
    CHECK_FALSE(write_json_with_preencoded_fields(output.path(),nlohmann::json::array(),encoded));
    CHECK(bytes()==expected.dump());
    REQUIRE(write_json_with_preencoded_fields(output.path(),metadata,{}));
    CHECK(bytes()==metadata.dump());

}


TEST_CASE("Beauty workbench requires a loaded local model and distinguishes temporary blockers", "[ModelGenerationPresentation]")
{
    using namespace Slic3r::GUI::ModelGenerationPresentation;
    CHECK(workbench_access(false, false, false, false) == WorkbenchAccess::NeedsModel);
    CHECK(workbench_access(false, true, false, false) == WorkbenchAccess::NeedsModel);
    CHECK(workbench_access(true, false, false, false) == WorkbenchAccess::NeedsModel);
    CHECK(workbench_access(false, true, true, true) == WorkbenchAccess::Loading);
    CHECK(workbench_access(true, true, false, true) == WorkbenchAccess::Busy);
    CHECK(workbench_access(true, true, false, false) == WorkbenchAccess::Available);
}

TEST_CASE("Image replacement validates bytes and preserves the old image on failure", "[ModelGenerationPresentation][ImageInput]")
{
    using namespace Slic3r::GUI;
    ScopedTemporaryDir temporary("orca-image-input");
    if (!wxImage::FindHandler(wxBITMAP_TYPE_PNG)) wxImage::AddHandler(new wxPNGHandler());
    wxImage source(96, 64);
    source.SetRGB(wxRect(0, 0, 96, 64), 32, 128, 224);
    source.InitAlpha();
    source.SetAlpha(0, 0, 17);
    const auto path = temporary.path() / boost::filesystem::path(L"中文透明图片.PNG");
    REQUIRE(source.SaveFile(path.wstring(), wxBITMAP_TYPE_PNG));
    wxImage output;
    REQUIRE(load_model_input_image(path, output) == ImageInputError::None);
    CHECK(output.GetWidth() == 96);
    CHECK(output.GetAlpha(0, 0) == 17);
    const auto fake = temporary.path() / "fake.jpg";
    boost::filesystem::copy_file(path, fake);
    CHECK(load_model_input_image(fake, output) == ImageInputError::ExtensionMismatch);
    CHECK(output.GetWidth() == 96);
    CHECK(output.GetBlue(0, 0) == 224);
    const auto webp = temporary.path() / "fake.webp";
    boost::filesystem::copy_file(path, webp);
    CHECK(load_model_input_image(webp, output) == ImageInputError::Unsupported);
    CHECK(load_model_input_image(temporary.path() / "missing.png", output) == ImageInputError::Unreadable);
    const auto corrupt = temporary.path() / "broken.png";
    { boost::filesystem::ofstream file(corrupt, std::ios::binary); file << "not an image"; }
    CHECK(load_model_input_image(corrupt, output) == ImageInputError::Corrupt);
    CHECK(output.GetAlpha(0, 0) == 17);
}

TEST_CASE("Image input rejects excessive bytes and pixels before decoding", "[ModelGenerationPresentation][ImageInput]")
{
    using namespace Slic3r::GUI;
    ScopedTemporaryDir temporary("orca-image-limits");
    if (!wxImage::FindHandler(wxBITMAP_TYPE_PNG)) wxImage::AddHandler(new wxPNGHandler());
    wxImage output, small_image(63, 64);
    const auto path = temporary.path() / "input.png";
    REQUIRE(small_image.SaveFile(path.wstring(), wxBITMAP_TYPE_PNG));
    CHECK(load_model_input_image(path, output) == ImageInputError::TooSmall);
    { boost::filesystem::ofstream file(path, std::ios::binary);
      file.seekp(MAX_INPUT_IMAGE_BYTES); file.put('x'); }
    CHECK(load_model_input_image(path, output) == ImageInputError::TooLarge);
    // A plausible huge IHDR must be refused before allocating its pixel buffer.
    const unsigned char header[] = {137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,32,0,0,0,32,0};
    { boost::filesystem::ofstream file(path, std::ios::binary);
      file.write(reinterpret_cast<const char*>(header), sizeof(header)); }
    CHECK(load_model_input_image(path, output) == ImageInputError::TooManyPixels);
    CHECK_FALSE(output.IsOk());
}

TEST_CASE("JPEG input honors camera orientation without modifying the original", "[ModelGenerationPresentation][ImageInput]")
{
    using namespace Slic3r::GUI;
    ScopedTemporaryDir temporary("orca-image-orientation");
    if (!wxImage::FindHandler(wxBITMAP_TYPE_JPEG)) wxImage::AddHandler(new wxJPEGHandler());
    wxImage source(96, 64);
    source.SetRGB(wxRect(0, 0, 96, 64), 200, 100, 30);
    const auto path = temporary.path() / "camera.jpg";
    REQUIRE(source.SaveFile(path.wstring(), wxBITMAP_TYPE_JPEG));
    boost::filesystem::ifstream input(path, std::ios::binary);
    const std::vector<unsigned char> original((std::istreambuf_iterator<char>(input)), {});
    input.close();
    // EXIF IFD0 orientation=6 (90 degrees clockwise), little endian TIFF.
    const std::vector<unsigned char> exif = {255,225,0,34,'E','x','i','f',0,0,'I','I',42,0,8,0,0,0,1,0,18,1,3,0,1,0,0,0,6,0,0,0,0,0,0,0};
    auto camera = original; camera.insert(camera.begin()+2, exif.begin(), exif.end());
    { boost::filesystem::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char*>(camera.data()), camera.size()); }
    wxImage output;
    REQUIRE(load_model_input_image(path, output) == ImageInputError::None);
    CHECK(output.GetWidth() == 64);
    CHECK(output.GetHeight() == 96);
    CHECK(boost::filesystem::file_size(path) == camera.size());
    boost::filesystem::ifstream after(path, std::ios::binary);
    CHECK(std::vector<unsigned char>((std::istreambuf_iterator<char>(after)), {}) == camera);
    after.close();
    camera.resize(camera.size()-2);
    { boost::filesystem::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char*>(camera.data()), camera.size()); }
    CHECK(load_model_input_image(path, output) == ImageInputError::Corrupt);
    CHECK(output.GetWidth() == 64);
}

TEST_CASE("EXIF transforms keep mirrored and rotated corner identities", "[ModelGenerationPresentation][ImageInput]")
{
    using namespace Slic3r::GUI;
    wxImage source(3, 2);
    source.SetRGB(wxRect(0, 0, 3, 2), 0, 0, 0);
    source.SetRGB(0, 0, 11, 0, 0); source.SetRGB(2, 0, 22, 0, 0);
    source.SetRGB(0, 1, 33, 0, 0); source.SetRGB(2, 1, 44, 0, 0);
    const std::array<int,8> top_left {11,22,44,33,11,33,44,22};
    for (int orientation=1; orientation<=8; ++orientation) {
        DYNAMIC_SECTION("orientation " << orientation) {
            const auto result=orient_input_image(source,orientation);
            CHECK(result.GetRed(0,0)==top_left[orientation-1]);
            CHECK(result.GetWidth()==(orientation>=5?2:3));
        }
    }
    CHECK(input_jpeg_orientation({255,216,255,225,255,255}) == 1);
}

TEST_CASE("Finishing preview owns the next action until accepted or returned to editing", "[ModelGenerationPresentation][WorkbenchFlow]")
{
    using namespace Slic3r::GUI::ModelGenerationPresentation;
    CHECK(finishing_stage(true, false, false, true) == FinishingStage::Editing);
    CHECK(finishing_stage(true, true, false, true) == FinishingStage::Processing);
    CHECK(finishing_stage(false, false, true, true) == FinishingStage::Preview);
    CHECK(finishing_stage(true, false, true, false) == FinishingStage::Preview);
    // Returning from preview exposes the retained dirty draft, never a saved action.
    CHECK(finishing_stage(true, false, false, true) != FinishingStage::Saved);
    CHECK(finishing_stage(false, false, false, false) == FinishingStage::Preparing);
    CHECK(finishing_stage(false, false, false, false, true) == FinishingStage::Failed);
    CHECK(finishing_stage(true, false, false, false) == FinishingStage::Saved);
}

TEST_CASE("Structural reports belong only to the same saved artifact in millimeters", "[ModelGenerationPresentation][ModelCheck]")
{
    using namespace Slic3r::GUI;
    using namespace Slic3r::GUI::ModelGenerationPresentation;
    const std::string digest(64, 'a');
    AIModelGenerationClient::ModelQuality quality;
    quality.available = true;
    quality.artifact_sha256 = digest;
    quality.units = "mm";
    quality.gate_version = "structural-v12";
    CHECK(model_quality_matches_artifact(quality, digest));
    CHECK_FALSE(model_quality_matches_artifact(quality, std::string(64, 'b')));
    quality.artifact_sha256.clear();
    CHECK_FALSE(model_quality_matches_artifact(quality, digest));
    quality.artifact_sha256 = std::string(64, 'g');
    CHECK_FALSE(model_quality_matches_artifact(quality, quality.artifact_sha256));
    quality.artifact_sha256 = digest;
    CHECK_FALSE(model_quality_matches_artifact(quality, digest.substr(1)));
    quality.gate_version = "structural-v11";
    CHECK_FALSE(model_quality_matches_artifact(quality, digest));
    quality.gate_version = "structural-v13";
    CHECK_FALSE(model_quality_matches_artifact(quality, digest));
    quality.gate_version = "structural-v12";
    quality.units = "m";
    CHECK_FALSE(model_quality_matches_artifact(quality, digest));
    quality.units = "mm";
    quality.gate_version.clear();
    CHECK_FALSE(model_quality_matches_artifact(quality, digest));
    quality.gate_version = "structural-v12";
    quality.available = false;
    CHECK_FALSE(model_quality_matches_artifact(quality, digest));
}


TEST_CASE("rejected model checks retain every error and warning without duplicate risks", "[ModelGenerationPresentation]")
{
    AIModelGenerationClient::ModelQuality quality;
    quality.available = true; quality.status = "reject";
    quality.errors = {"non_manifold_edges", "flat_or_empty_axis"};
    quality.warnings = {"non_manifold_edges", "tiny_detached_components", "localized_overhang_regions", "tiny_printable_color_regions"};
    quality.report_metrics = {{"non_manifold_edges", 237}, {"tiny_component_count", 158},
        {"significant_overhang_region_count", 7}, {"tiny_color_region_count", 9597}};
    const auto risks = model_check_risks(quality);
    REQUIRE(risks.size() == 5);
    CHECK(risks[0].blocking); CHECK(risks[1].blocking); CHECK_FALSE(risks[2].blocking);
    CHECK(risks[0].detail.Contains("237"));
    CHECK(risks[2].detail.Contains("158"));
    CHECK(risks[4].detail.Contains("9597"));
    quality.available = false;
    CHECK(model_check_risks(quality).empty());
}

TEST_CASE("unmeasured or invalid risk counts are not displayed as zero results", "[ModelGenerationPresentation]")
{
    AIModelGenerationClient::ModelQuality quality;
    quality.available = true; quality.warnings = {"repairable_boundary_edges"};
    const auto missing = model_check_risks(quality);
    REQUIRE(missing.size() == 1);
    quality.report_metrics["boundary_edges"] = -1;
    CHECK(model_check_risks(quality)[0].detail == missing[0].detail);
    quality.report_metrics["boundary_edges"] = 948;
    CHECK(model_check_risks(quality)[0].detail.Contains("948"));
    CHECK_FALSE(missing[0].detail.Contains("0"));
}

TEST_CASE("check scope preserves the actual report thresholds and unmeasured thickness", "[ModelGenerationPresentation]")
{
    AIModelGenerationClient::ModelQuality quality;
    quality.available = true; quality.units = "mm"; quality.gate_version = "structural-v12";
    quality.report_thresholds = {{"max_faces", 123456}, {"min_local_wall_thickness_mm", 1.25}};
    const auto scope = model_check_scope(quality);
    CHECK(scope.Contains("123456")); CHECK(scope.Contains("1.25"));
    CHECK(scope.Contains("structural-v12"));
    quality.local_thickness_available = true;
    CHECK(model_check_scope(quality) != scope);
    quality.report_thresholds.clear();
    CHECK_FALSE(model_check_scope(quality).Contains("1.25"));
}

TEST_CASE("model check failures provide recovery guidance without leaking service text", "[ModelGenerationPresentation]")
{
    for (const auto* error : {"quality_report_unavailable: raw storage path", "artifact_changed: English details",
                             "timeout: request failed", "unknown raw service diagnostics"}) {
        const auto message = model_check_failure_message(error);
        CHECK_FALSE(message.Contains("raw")); CHECK_FALSE(message.Contains("English"));
        CHECK_FALSE(message.Contains("unknown")); CHECK_FALSE(message.empty());
    }
}


TEST_CASE("Invalid reference files do not ask to leave an edited model", "[ModelGenerationPresentation][ImageInput]")
{
    using namespace Slic3r::GUI;
    ScopedTemporaryDir temporary("orca-reference-replacement");
    if (!wxImage::FindHandler(wxBITMAP_TYPE_PNG)) wxImage::AddHandler(new wxPNGHandler());
    wxImage reference(96, 64);
    reference.SetRGB(wxRect(0, 0, 96, 64), 32, 128, 224);
    const auto good = temporary.path() / "reference.png";
    REQUIRE(reference.SaveFile(good.wstring(), wxBITMAP_TYPE_PNG));
    const auto misnamed = temporary.path() / "misnamed.jpg";
    boost::filesystem::copy_file(good, misnamed);
    const auto corrupt = temporary.path() / "broken.png";
    { boost::filesystem::ofstream file(corrupt, std::ios::binary); file << "not a PNG"; }

    const auto expected = GENERATE(ImageInputError::Unreadable, ImageInputError::Corrupt,
                                   ImageInputError::ExtensionMismatch);
    const auto path = expected == ImageInputError::Unreadable ? temporary.path() / "missing.png" :
                      expected == ImageInputError::Corrupt ? corrupt : misnamed;
    wxImage previous(80, 64);
    previous.SetRGB(wxRect(0, 0, 80, 64), 180, 20, 40);
    previous.InitAlpha();
    previous.SetAlpha(0, 0, 52);
    bool asked_to_leave = false;
    CHECK(prepare_model_input_replacement(path, previous, [&] {
        asked_to_leave = true;
        return false;
    }) == expected);
    CHECK_FALSE(asked_to_leave);
    CHECK(previous.GetWidth() == 80);
    CHECK(previous.GetRed(0, 0) == 180);
    CHECK(previous.GetAlpha(0, 0) == 52);
}

TEST_CASE("A valid replacement publishes its preview only after edit approval", "[ModelGenerationPresentation][ImageInput]")
{
    using namespace Slic3r::GUI;
    ScopedTemporaryDir temporary("orca-reference-approval");
    if (!wxImage::FindHandler(wxBITMAP_TYPE_PNG)) wxImage::AddHandler(new wxPNGHandler());
    wxImage reference(96, 64);
    reference.SetRGB(wxRect(0, 0, 96, 64), 32, 128, 224);
    reference.InitAlpha();
    reference.SetAlpha(0, 0, 17);
    const auto path = temporary.path() / "reference.png";
    REQUIRE(reference.SaveFile(path.wstring(), wxBITMAP_TYPE_PNG));

    wxImage previous(80, 64);
    previous.SetRGB(wxRect(0, 0, 80, 64), 180, 20, 40);
    previous.InitAlpha();
    previous.SetAlpha(0, 0, 52);
    const bool approved = GENERATE(false, true);
    int decisions = 0;
    CHECK(prepare_model_input_replacement(path, previous, [&] {
        ++decisions;
        return approved;
    }) == (approved ? ImageInputError::None : ImageInputError::Cancelled));
    CHECK(decisions == 1);
    CHECK(previous.GetWidth() == (approved ? 96 : 80));
    CHECK(previous.GetBlue(0, 0) == (approved ? 224 : 40));
    CHECK(previous.GetAlpha(0, 0) == (approved ? 17 : 52));
}


TEST_CASE("Saved design downloads recover without starting another design generation",
          "[ModelGenerationPresentation][DesignPreviewRecovery]")
{
    // Cancelled/failed downloads retain the same saved result. Changing the
    // inputs instead requires a new design; restoring them permits a reload.
    const std::string job = "saved-design";
    REQUIRE(design_preview_reload_available(job, true, true, false));
    CHECK_FALSE(design_preview_reload_available(job, false, true, false));
    CHECK(design_preview_reload_available(job, true, true, false));
    // A missing task/result or an already loaded design must use the ordinary
    // workflow, never present an unavailable or duplicate reload.
    CHECK_FALSE(design_preview_reload_available("", true, true, false));
    CHECK_FALSE(design_preview_reload_available(job, true, false, false));
    CHECK_FALSE(design_preview_reload_available(job, true, true, true));
}

TEST_CASE("Style input accepts existing families and rejects unknown provider values",
          "[ModelGenerationPresentation][UiRedesign]")
{
    CHECK(is_supported_style("sculpture"));
    CHECK(is_supported_style("realistic"));
    for (const std::string invalid : {"", "multicolor", "CUSTOM", "new-style", " cartoon "}) {
        INFO(invalid);
        CHECK_FALSE(is_supported_style(invalid));
    }
}

TEST_CASE("Image history includes model results without changing legacy model precedence",
          "[ModelGenerationPresentation][ImageHistory]")
{
    ScopedTemporaryDir temporary("orca-image-history");
    const std::string id = "11111111-1111-4111-8111-111111111111";
    auto record = write_design_fixture(temporary.path() / id);
    record["state"] = "ready";
    record["style"] = "custom";
    record["custom_style"] = "paper sculpture";
    REQUIRE(write_json(temporary.path() / id / "job.json", record));
    { boost::filesystem::ofstream model(temporary.path() / id / "model.glb"); model << "saved model"; }
    REQUIRE_FALSE(read_design_history_entry(temporary.path(), id));
    const auto image = read_image_history_entry(temporary.path(), id);
    REQUIRE(image);
    CHECK(image->style == "custom");
    CHECK(image->custom_style == "paper sculpture");
    CHECK(image_history_matches(*image, " BLUE SCARF "));
    CHECK(image_history_matches(*image, "paper"));
    CHECK(image_history_matches(*image, wxString::FromUTF8("自定义")));
    CHECK_FALSE(image_history_matches(*image, "absent"));
}

TEST_CASE("Removing an image record retains all assets and legacy history across reloads",
          "[ModelGenerationPresentation][ImageHistory]")
{
    ScopedTemporaryDir temporary("orca-image-history");
    const std::string id = "11111111-1111-4111-8111-111111111111";
    const auto record = write_design_fixture(temporary.path() / id);
    REQUIRE(hide_image_history_entry(temporary.path(), id));
    CHECK(read_image_history(temporary.path()).empty());
    CHECK_FALSE(read_image_history_entry(temporary.path(), id));
    CHECK(read_design_history_entry(temporary.path(), id).has_value());
    CHECK(read_json(temporary.path() / id / "job.json") == record);
    CHECK(boost::filesystem::exists(temporary.path() / id / "input.png"));
    CHECK_FALSE(hide_image_history_entry(temporary.path(), "../" + id));
}

TEST_CASE("Image history lists every version newest first and omits model-only forks",
          "[ModelGenerationPresentation][ImageHistory]")
{
    ScopedTemporaryDir temporary("orca-image-history");
    std::vector<std::string> ids;
    for (int i = 0; i < 16; ++i) {
        const std::string id = "11111111-1111-4111-8111-1111111111" + std::to_string(10 + i);
        ids.push_back(id);
        write_design_fixture(temporary.path() / id);
        const auto entry = read_image_history_entry(temporary.path(), id);
        REQUIRE(entry);
        boost::filesystem::last_write_time(entry->raw_preview_path, 1700000000 + i);
    }
    auto entries = read_image_history(temporary.path());
    REQUIRE(entries.size() == 16);
    CHECK(entries.front().job_id == ids.back());
    CHECK(entries.back().job_id == ids.front());
    auto record = read_json(temporary.path() / ids.back() / "job.json");
    record["source_design_job_id"] = ids.front();
    REQUIRE(write_json(temporary.path() / ids.back() / "job.json", record));
    entries = read_image_history(temporary.path());
    REQUIRE(entries.size() == 15);
    CHECK(entries.front().job_id == ids[14]);
}

TEST_CASE("Image history rejects escaped or corrupt previews without discarding valid versions",
          "[ModelGenerationPresentation][ImageHistory]")
{
    ScopedTemporaryDir temporary("orca-image-history");
    const std::string id = "11111111-1111-4111-8111-111111111111";
    auto record = write_design_fixture(temporary.path() / id);
    for (const auto* key : {"preview_path", "raw_preview_path", "model_reference_path"}) record[key] = "../external.png";
    REQUIRE(write_json(temporary.path() / id / "job.json", record));
    CHECK_FALSE(read_image_history_entry(temporary.path(), id));
    CHECK_FALSE(read_image_history_entry(temporary.path(), "../" + id));
}
