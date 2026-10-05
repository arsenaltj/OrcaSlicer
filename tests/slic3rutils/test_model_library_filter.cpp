#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/AI/ModelGeneration/ModelLibraryFilter.hpp"
using namespace Slic3r::GUI;
namespace {
struct Entry {
    wxString title, search_text;
    std::string job_id, provider_task_id, provider_conversion_task_id;
    bool design_only {false};
    std::string ai_image_path;
};
}
TEST_CASE("Library filtering keeps source identity and disjoint asset categories", "[ModelLibraryFilter]")
{
    const std::vector<Entry> entries {
        {"same", "same", "design", "", "", true},
        {"same", "same", "model", "Task-A", "", false},
        {"same", "same", "finish-version", "", "", false}};
    CHECK(filter_model_library(entries, ModelLibraryCategory::All, "") == std::vector<size_t>{0,1,2});
    CHECK(filter_model_library(entries, ModelLibraryCategory::Design, "") == std::vector<size_t>{0});
    CHECK(filter_model_library(entries, ModelLibraryCategory::Model, "") == std::vector<size_t>{1});
    CHECK(filter_model_library(entries, ModelLibraryCategory::Finishing, "") == std::vector<size_t>{2});
    CHECK(filter_model_library(entries, ModelLibraryCategory::Design, "Task-A").empty());
}
TEST_CASE("Library search finds untruncated names and all task identifiers", "[ModelLibraryFilter]")
{
    const std::vector<Entry> entries {{"short...", wxString::FromUTF8("a long name ending in 企鹅"),
        "local-123", "Service-ABC", "Convert-XYZ", false}};
    for (const wxString query : {wxString::FromUTF8("企鹅"), wxString(" local-123 "),
        wxString("service-abc"), wxString("CONVERT-xyz")})
        CHECK(filter_model_library(entries, ModelLibraryCategory::All, query) == std::vector<size_t>{0});
    CHECK(filter_model_library(entries, ModelLibraryCategory::All, "not present").empty());
    CHECK(filter_model_library(entries, ModelLibraryCategory::All, "   ") == std::vector<size_t>{0});
}
TEST_CASE("Filtering a later page resets to matching source records without rewriting the snapshot", "[ModelLibraryFilter]")
{
    std::vector<Entry> entries;
    for (size_t i=0;i<30;++i) entries.push_back({"model", "model", std::to_string(i), "", "", false});
    entries[27].provider_task_id="unique-task";
    const auto filtered=filter_model_library(entries,ModelLibraryCategory::All,"unique-task");
    REQUIRE(filtered.size()==1);
    CHECK(entries[filtered.front()].job_id=="27");
    CHECK(entries.size()==30);
    CHECK(filter_model_library(entries,ModelLibraryCategory::All,"").size()==30);
}

TEST_CASE("My images and the image drawer share model source images without duplicating saved versions", "[ModelLibraryFilter]")
{
    const std::vector<Entry> entries {
        {"design", "design", "design", "", "", true, "design.png"},
        {"model", "model", "model", "Task-A", "", false, "model.png"},
        {"saved", "saved", "finish-version", "", "", false, "model.png"},
        {"local", "local", "local", "", "", false, ""}};
    CHECK(filter_model_library(entries, ModelLibraryCategory::Design, "", true) == std::vector<size_t>{0,1});
    CHECK(filter_model_library(entries, ModelLibraryCategory::Design, "task-a", true) == std::vector<size_t>{1});
    CHECK(filter_model_library(entries, ModelLibraryCategory::Design, "") == std::vector<size_t>{0,1});
    CHECK(filter_model_library(entries, ModelLibraryCategory::Design, "task-a") == std::vector<size_t>{1});
    CHECK_FALSE(entries[1].design_only);
    CHECK(entries[1].job_id == "model");
    CHECK(filter_model_library(entries, ModelLibraryCategory::All, "") == std::vector<size_t>{0,1,2,3});
}

TEST_CASE("Local import copies remain models rather than accepted finishing versions", "[ModelLibraryFilter]")
{
    const std::string imported = "finish-import-22f4ad96-4bb2-43d8-9556-1c7cb3a8a810";
    const std::string saved = "finish-3f5da697-87fb-4ea1-868a-6dfb40c2c1c8";
    const std::vector<Entry> entries {
        {"design", "design", "design", "", "", true, "design.png"},
        {"model", "model", "generated-task", "Task-A", "", false, "model.png"},
        {"local", "local", imported, "", "", false, ""},
        {"saved", "saved", saved, "", "", false, "model.png"}};
    CHECK(model_library_asset_kind(true, "design") == ModelLibraryAssetKind::Design);
    CHECK(model_library_asset_kind(false, "generated-task") == ModelLibraryAssetKind::Model);
    CHECK(model_library_asset_kind(false, imported) == ModelLibraryAssetKind::LocalImport);
    CHECK(model_library_asset_kind(false, saved) == ModelLibraryAssetKind::Finishing);
    CHECK(filter_model_library(entries, ModelLibraryCategory::Model, "") == std::vector<size_t>{1,2});
    CHECK(filter_model_library(entries, ModelLibraryCategory::Finishing, "") == std::vector<size_t>{3});
    CHECK(filter_model_library(entries, ModelLibraryCategory::All, "") == std::vector<size_t>{0,1,2,3});
    CHECK(filter_model_library(entries, ModelLibraryCategory::Model, wxString::FromUTF8(imported)) ==
        std::vector<size_t>{2});
    CHECK(entries[2].job_id == imported);
    CHECK(entries[2].provider_task_id.empty());
}

TEST_CASE("Local copies with a reused task image do not add duplicate image cards", "[ModelLibraryFilter]")
{
    const std::vector<Entry> entries {
        {"model", "model", "original-task", "Task-A", "", false, "original.png"},
        {"local", "local", "finish-import-copy", "", "", false, "original.png"},
        {"saved", "saved", "finish-version", "", "", false, "original.png"}};
    CHECK(filter_model_library(entries, ModelLibraryCategory::Model, "") == std::vector<size_t>{0,1});
    CHECK(filter_model_library(entries, ModelLibraryCategory::Design, "") == std::vector<size_t>{0});
    CHECK(filter_model_library(entries, ModelLibraryCategory::All, "", true) == std::vector<size_t>{0});
    CHECK(filter_model_library(entries, ModelLibraryCategory::All, "task-a", true) == std::vector<size_t>{0});
    CHECK(filter_model_library(entries, ModelLibraryCategory::All, "finish-import-copy", true).empty());
    CHECK(entries[1].ai_image_path == "original.png");
    CHECK(entries[1].provider_task_id.empty());
}
