#include "slic3r/GUI/AI/Orca/SmartSliceApplier.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace Slic3r;
using namespace Slic3r::AI::SmartSlicing;
using Slic3r::GUI::SmartSliceApplier;

TEST_CASE("smart slice applier maps recommendation fields to native config", "[SmartSlice][SmartSliceApplier]")
{
    DynamicPrintConfig base;
    base.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    base.set_key_value("wall_loops", new ConfigOptionInt(2));
    base.set_key_value("sparse_infill_density", new ConfigOptionPercent(10.0));
    base.set_key_value("sparse_infill_pattern", new ConfigOptionEnum<InfillPattern>(ipRectilinear));
    base.set_key_value("outer_wall_line_width", new ConfigOptionFloatOrPercent(0.4, false));
    base.set_key_value("outer_wall_speed", new ConfigOptionFloatsNullable(1, 40.0));
    base.set_key_value("enable_support", new ConfigOptionBool(false));
    base.set_key_value("support_threshold_angle", new ConfigOptionInt(45));
    base.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalAuto));
    base.set_key_value("wall_generator", new ConfigOptionEnum<PerimeterGeneratorType>(PerimeterGeneratorType::Classic));

    SliceRecommendation plan;
    plan.layer_height_mm = 0.12;
    plan.wall_loops = 4;
    plan.sparse_infill_density_percent = 18.0;
    plan.sparse_infill_pattern = "gyroid";
    plan.outer_wall_speed_mm_s = 25.0;
    plan.line_width_mm = 0.42;
    plan.enable_support = true;
    plan.support_type = "tree(auto)";
    plan.support_threshold_angle_deg = 46.0;
    plan.arachne_wall_generator = true;

    DynamicPrintConfig output;
    std::string diagnostic;
    REQUIRE(SmartSliceApplier::BuildConfig(base, plan, output, &diagnostic));
    CHECK(diagnostic.empty());
    CHECK(std::abs(output.opt_float("layer_height") - 0.12) < 1e-6);
    CHECK(output.opt_int("wall_loops") == 4);
    CHECK(std::abs(output.opt<ConfigOptionPercent>("sparse_infill_density")->value - 18.0) < 1e-6);
    CHECK(output.opt_enum<InfillPattern>("sparse_infill_pattern") == ipGyroid);
    CHECK(std::abs(output.opt_float_nullable("outer_wall_speed", 0) - 25.0) < 1e-6);
    CHECK(output.opt_bool("enable_support"));
    CHECK(output.opt_enum<SupportType>("support_type") == stTreeAuto);
    CHECK(output.opt_enum<PerimeterGeneratorType>("wall_generator") == PerimeterGeneratorType::Arachne);
}

TEST_CASE("smart slice applier rejects unsafe plans before mutation", "[SmartSlice][SmartSliceApplier]")
{
    DynamicPrintConfig base;
    base.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    SliceRecommendation plan;
    plan.layer_height_mm = 2.0;
    std::string diagnostic;
    DynamicPrintConfig output;
    CHECK_FALSE(SmartSliceApplier::BuildConfig(base, plan, output, &diagnostic));
    CHECK(diagnostic == "smart_slice_plan_out_of_range");

    base.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4}));
    plan.layer_height_mm = 0.2;
    plan.line_width_mm = 0.8;
    diagnostic.clear();
    CHECK_FALSE(SmartSliceApplier::BuildConfig(base, plan, output, &diagnostic));
    CHECK(diagnostic == "smart_slice_plan_physical_bounds");
}
