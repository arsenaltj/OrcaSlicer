#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/PrinterFilamentComparison.hpp"

using namespace Slic3r;

namespace {

constexpr auto device_id = "virtual-printer-test";

nlohmann::json device_state()
{
    return nlohmann::json::parse(R"({
        "instance_uuid":"virtual-printer-test",
        "config": {
            "schema":1,
            "tools":[{"nozzle":0,"slot":2},{"nozzle":0,"slot":0},{"nozzle":1,"slot":1}],
            "slots":[
                {"material":"PETG","color":"#112233","remaining_g":50},
                {"material":"PLA","color":"#654321","remaining_g":0},
                {"material":"Pla","color":"#AABBCC","remaining_g":200}
            ]
        }
    })");
}

} // namespace

TEST_CASE("Transfer comparison resolves the explicit slot even when tools share a nozzle", "[PrinterTransferFilaments]")
{
    const auto filaments = read_virtual_printer_transfer_filaments(device_state(), device_id);
    REQUIRE(filaments.size() == 2);
    CHECK(filaments.at(0).slot == 2);
    CHECK(filaments.at(1).slot == 0);
    CHECK(filaments.count(2) == 0);
    CHECK(compare_transfer_filaments({"PLA", "#aabbcc"}, filaments.at(0).filament) == TransferFilamentMatch::Same);
}

TEST_CASE("Transfer comparison never fills in missing or foreign tool mappings", "[PrinterTransferFilaments]")
{
    auto state = device_state();
    CHECK(read_virtual_printer_transfer_filaments(state, "another-instance").empty());
    CHECK(read_virtual_printer_transfer_filaments(state, "").empty());
    CHECK(read_virtual_printer_transfer_filaments(nlohmann::json{}, device_id).empty());
    state["config"]["tools"][0].erase("slot");
    state["config"]["tools"][1]["slot"] = 21;
    CHECK(read_virtual_printer_transfer_filaments(state, device_id).empty());
    state["config"].erase("tools");
    CHECK(read_virtual_printer_transfer_filaments(state, device_id).empty());
}

TEST_CASE("Transfer comparison distinguishes a known difference from missing information", "[PrinterTransferFilaments]")
{
    const TransferFilament sliced{"PLA", "#AABBCC"};
    CHECK(compare_transfer_filaments(sliced, {" pla ", "aabbcc"}) == TransferFilamentMatch::Same);
    CHECK(compare_transfer_filaments(sliced, {"PLA", "#112233"}) == TransferFilamentMatch::Different);
    CHECK(compare_transfer_filaments(sliced, {"PETG", "#AABBCC"}) == TransferFilamentMatch::Different);
    CHECK(compare_transfer_filaments(sliced, {"", "#112233"}) == TransferFilamentMatch::Different);
    CHECK(compare_transfer_filaments(sliced, {"PLA", "invalid"}) == TransferFilamentMatch::Unverified);
    CHECK(compare_transfer_filaments(sliced, {"", "#aabbcc"}) == TransferFilamentMatch::Unverified);
    CHECK(compare_transfer_filaments(sliced, {}) == TransferFilamentMatch::Unverified);
}

TEST_CASE("Incomplete plate metadata preserves the sliced filament color and material", "[PrinterTransferFilaments]")
{
    TransferFilament sliced{"PLA", "#9a9693"};
    apply_transfer_filament_metadata(sliced, {});
    CHECK(compare_transfer_filaments(sliced, {"PLA", "#ff7748"}) == TransferFilamentMatch::Different);
    apply_transfer_filament_metadata(sliced, {" ", "invalid"});
    CHECK(compare_transfer_filaments(sliced, {"PLA", "#9a9693"}) == TransferFilamentMatch::Same);
    apply_transfer_filament_metadata(sliced, {"PETG", "#FF7748FF"});
    CHECK(compare_transfer_filaments(sliced, {"PETG", "#ff7748"}) == TransferFilamentMatch::Same);
}
