#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/AI/ModelGeneration/PortraitResidualProposal.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitResidualRequest.hpp"

using namespace Slic3r::GUI::PortraitResidual;
using Json = nlohmann::json;

namespace {
Identity id()
{
    return {std::string(64, 'a'), std::string(64, 'b'), 64, std::string(64, 'c'),
        std::string(64, 'd'), std::string(64, 'e'), std::string(64, 'f')};
}

Proposal record(const std::string& unit, const std::string& kind, size_t face)
{
    Proposal result;
    result.unit_id = unit; result.kind = kind; result.status = "PROPOSED";
    result.subject_id="person-a";result.parent_region=kind;
    result.confidence = .95; result.view_support = 2; result.source_faces = {face};
    result.leaf_keys = {{face, 1, 0}};
    return result;
}

Document document()
{
    Document result;
    result.identity = id(); result.status = "READY"; result.remaining_triangle_budget = 15;
    result.request_sha256=std::string(64,'1');
    result.proposals = {record("skin", "skin", 4), record("hair", "hair", 5)};
    result.statistics = {{"units", 2}, {"proposed", 2}};
    return result;
}
}

TEST_CASE("Portrait residual proposal round trips with local identity", "[PortraitResidualProposal]")
{
    const auto value = document();
    const auto encoded = value.encode();
    CHECK(encoded.at("schema") == schema);
    const auto restored = Document::decode(encoded, &value.identity);
    CHECK(restored.encode() == encoded);
    CHECK(restored.actionable().size() == 2);
}

TEST_CASE("Portrait residual identity drift fails closed", "[PortraitResidualProposal]")
{
    auto encoded = document().encode();
    encoded["source_sha256"] = std::string(64, '0');
    auto expected_source = id();
    CHECK_THROWS(Document::decode(encoded, &expected_source));
    auto expected = id(); expected.runtime_sha256 = std::string(64, '0');
    CHECK_THROWS(Document::decode(document().encode(), &expected));
}

TEST_CASE("Mixed face proposals require leaves and preserve accepted boundaries", "[PortraitResidualProposal]")
{
    auto value = document();
    value.proposals.front().source_faces = {4, 6};
    value.proposals.front().leaf_keys = {{4, 1, 0}};
    CHECK_NOTHROW(value.validate());
    value.proposals.front().rejected_faces = {4};
    CHECK_THROWS(value.validate());
}

TEST_CASE("Overlapping actionable proposals are rejected", "[PortraitResidualProposal]")
{
    auto value = document();
    value.proposals.push_back(record("overlap", "cloth", 4));
    CHECK_THROWS(value.validate());
}

TEST_CASE("Subdivide and preserve proposals are never actionable", "[PortraitResidualProposal]")
{
    auto value = document();
    auto cut = record("cut", "subdivide", 6); cut.additional_triangles = 20; value.proposals.push_back(cut);
    auto keep = record("keep", "preserve", 7); value.proposals.push_back(keep);
    value.proposals[0].status = "PROPOSED";
    CHECK(value.actionable().size() == 2);
    CHECK_FALSE(value.proposals[2].actionable());
    CHECK_FALSE(value.proposals[3].actionable());
}

TEST_CASE("Rejected records cannot become actionable", "[PortraitResidualProposal]")
{
    auto value = document();
    auto rejected = record("frozen", "preserve", 1); rejected.status = "REJECTED";
    rejected.reasons = {"FROZEN_SHAPE_LOCK"}; value.rejected.push_back(rejected);
    CHECK_NOTHROW(value.validate());
    CHECK(value.actionable().size() == 2);
}

TEST_CASE("Leaf keys are bounded to the depth four domain", "[PortraitResidualProposal]")
{
    auto value = document();
    value.proposals.front().leaf_keys = {{4, 5, 0}};
    CHECK_THROWS(value.encode());
    value = document();
    value.proposals.front().leaf_keys = {{6, 1, 0}};
    CHECK_THROWS(value.encode());
}

TEST_CASE("Residual proposals reject ancestor overlap and hard conflicts", "[PortraitResidualProposal]")
{
    auto value=document();value.proposals.front().leaf_keys={{4,0,0},{4,1,0}};
    CHECK_THROWS(value.validate());
    value=document();value.proposals.front().reasons={"CROSS_SUBJECT"};CHECK_THROWS(value.validate());
    value=document();value.proposals.front().confidence=.5;CHECK_THROWS(value.validate());
    value=document();value.proposals.front().parent_region="hair";CHECK_THROWS(value.validate());
}

TEST_CASE("Residual application binds to the exact source unit palette and edit version", "[PortraitResidualProposal]")
{
    auto value=document();value.proposals.resize(1);
    auto& record=value.proposals.front();record.target_rgb=std::array<float,3>{.8f,.6f,.5f};record.target_slot=0;
    auto request=value.encode();
    request["units"]=Json::array({{{"unit_id",record.unit_id},{"subject_id",record.subject_id},
        {"parent_region",record.parent_region},{"proposal",record.kind},{"face_ids",record.source_faces},
        {"leaf_keys",record.leaf_keys},{"target_rgb",*record.target_rgb},{"target_slot",0}}});
    CHECK_NOTHROW(value.bind_to_request(request,value.request_sha256));
    CHECK_THROWS(value.bind_to_request(request,std::string(64,'2')));
    record.target_slot=1;CHECK_THROWS(value.bind_to_request(request,value.request_sha256));
}

TEST_CASE("Residual requests preserve frozen roots and separate unsupported observations", "[PortraitResidualProposal]")
{
    Slic3r::GUI::PortraitShapeDetails details;
    details.base_colors.resize(3,{.8f,.6f,.5f,1.f});
    details.parent_samples=Json::array({{0,"person-a","face",.95,100,3},
        {1,"person-a","face",.95,20,1},{2,"person-a","face",.95,50,2}});
    const std::vector<std::array<float,3>> palette{{.8f,.6f,.5f},{.1f,.1f,.1f}};
    const auto units=parent_units(details,palette,{}, {},{2},{{1},{0,2},{1}});
    REQUIRE(units.size()==2);
    CHECK(units.at(0).at("face_ids")==Json::array({0}));
    CHECK(units.at(0).at("view_support")==3);
    CHECK(units.at(1).at("view_support")==1);
}

TEST_CASE("Residual regions follow connected parent topology and preserve blocked gaps", "[PortraitResidualProposal]")
{
    Slic3r::GUI::PortraitShapeDetails details;
    details.base_colors.resize(5,{.8f,.6f,.5f,1.f});
    for(size_t face=0;face<5;++face)
        details.parent_samples.push_back({face,"person-a","face",.95,50,2});
    const std::vector<std::array<float,3>> palette{{.8f,.6f,.5f}};
    const std::vector<std::vector<uint32_t>> neighbors{{1},{0,2},{1,3},{2},{}};
    const auto units=parent_units(details,palette,{}, {},{2},neighbors);
    REQUIRE(units.size()==3);
    CHECK(units.at(0).at("face_ids")==Json::array({0,1}));
    CHECK(units.at(1).at("face_ids")==Json::array({3}));
    CHECK(units.at(2).at("face_ids")==Json::array({4}));
    CHECK_THROWS(parent_units(details,palette,{}, {},{},{}));
}
