#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/TextureToColor/ColorUtils.hpp"
#include <fstream>
#include <string>

// Supplementary factual reference data from Sharma, Wu and Dalal (2005),
// DOI 10.1002/col.20070. Retrieved from the authors' implementation-notes page:
// https://hajim.rochester.edu/ece/sites/gsharma/ciede2000/
TEST_CASE("color differences match every published Sharma reference pair", "[ColorDifference]")
{
    using namespace Slic3r::tex2color::color_utils;
    std::ifstream data(std::string(TEST_DATA_DIR) + "/ciede2000_sharma.txt");
    REQUIRE(data.is_open());
    ColorDouble a, b;
    double expected;
    size_t rows = 0;
    while (data >> a[0] >> a[1] >> a[2] >> b[0] >> b[1] >> b[2] >> expected) {
        ++rows;
        CAPTURE(rows, a, b, expected);
        CHECK_THAT(calc_lab_color_difference_by_ciede2000(a,b), Catch::Matchers::WithinAbs(expected, .00005));
        CHECK_THAT(calc_lab_color_difference_by_ciede2000(b,a), Catch::Matchers::WithinAbs(expected, .00005));
    }
    CHECK(data.eof());
    CHECK(rows == 34);
}

TEST_CASE("color difference RGB encodings use the same conversion and metric", "[ColorDifference]")
{
    using namespace Slic3r::tex2color::color_utils;
    for (const RGB a : {RGB{0,0,0},RGB{255,255,255},RGB{190,20,100},RGB{12,170,190}}) {
        const RGB b {34,73,160};
        ColorDouble af, bf;
        for(size_t c=0;c<3;++c) { af[c]=double(a[c])/255.; bf[c]=double(b[c])/255.; }
        CHECK_THAT(calc_rgb_color_difference_by_ciede2000_srgb01(af,bf),
            Catch::Matchers::WithinAbs(calc_rgb_color_difference_by_ciede2000(a,b),1e-10));
        CHECK_THAT(calc_rgb_color_difference_by_ciede2000(a,a),Catch::Matchers::WithinAbs(0,1e-12));
    }
}

TEST_CASE("color differences wrap the mean hue below a full turn", "[ColorDifference]")
{
    using namespace Slic3r::tex2color::color_utils;
    const ColorDouble a {50,3.1368345115760294,6.261846084332455};
    const ColorDouble b {50,45.27944980884872,-87.85644254380878};
    // Sharma equation 14: hue difference > 180 degrees and hue sum >= 360.
    const double expected = 35.55764775369227;
    CHECK_THAT(calc_lab_color_difference_by_ciede2000(a,b),Catch::Matchers::WithinAbs(expected,1e-10));
    CHECK_THAT(calc_lab_color_difference_by_ciede2000(b,a),Catch::Matchers::WithinAbs(expected,1e-10));
}

#include "libslic3r/TextureToColor/ColorDistancePrepared.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <limits>
#include <random>
#include <vector>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>

namespace {
namespace prepared_chroma_test {
namespace colors = Slic3r::tex2color::color_utils;
namespace prepared = Slic3r::tex2color::color_utils::detail;
using Lab = std::array<double, 3>;

std::uint64_t bits(double value)
{
    static_assert(sizeof(double) == sizeof(std::uint64_t), "Bitwise oracle needs an IEEE-sized double");
    std::uint64_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

void require_qualified_environment()
{
#if defined(__FAST_MATH__) || defined(_M_FP_FAST) || \
    (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__ > 0)
    REQUIRE_FALSE(prepared::has_default_lab_chroma_environment());
#else
    if (prepared::has_default_lab_chroma_environment()) return;
#endif
    double sentinel = -17.125;
    CHECK_FALSE(prepared::prepare_lab_chroma({50.,3.,4.}, sentinel));
    CHECK(bits(sentinel) == bits(-17.125));
    SKIP("Prepared kernel unsupported in this FP environment; scalar oracle remains enabled.");
}

void require_direction(const Lab& first, const Lab& second, double first_chroma, double second_chroma)
{
    // The scalar's original function body is independent of the prepared
    // implementation. Never route this expected value through a shared helper.
    const double expected = colors::calc_lab_color_difference_by_ciede2000(first, second);
    const double actual = prepared::ciede2000_prepared(first, second, first_chroma, second_chroma);
    CAPTURE(first, second, first_chroma, second_chroma, expected, actual);
    REQUIRE(std::isfinite(expected));
    CHECK(bits(actual) == bits(expected));
}

void require_pair(const Lab& first, const Lab& second)
{
    double first_chroma = -17., second_chroma = -23.;
    REQUIRE(prepared::prepare_lab_chroma(first, first_chroma));
    REQUIRE(prepared::prepare_lab_chroma(second, second_chroma));
    // Check the exact original expression as well as its downstream result.
    CHECK(bits(first_chroma) == bits(std::sqrt(first[1] * first[1] + first[2] * first[2])));
    CHECK(bits(second_chroma) == bits(std::sqrt(second[1] * second[1] + second[2] * second[2])));
    require_direction(first, second, first_chroma, second_chroma);
    // Mathematical symmetry is not a promise of floating-point bit symmetry.
    // Each direction is compared with its own unmodified scalar evaluation.
    require_direction(second, first, second_chroma, first_chroma);
}

double random_component(std::mt19937_64& random, double scale)
{
    // Fixed integer-to-binary-fraction mapping avoids a library-specific
    // uniform_real_distribution. These are general Lab values, not RGB input.
    const auto signed_sample = std::int64_t(random() & ((std::uint64_t(1) << 32) - 1)) - (std::int64_t(1) << 31);
    return double(signed_sample) / 2147483648.0 * scale;
}
} // namespace prepared_chroma_test
} // namespace

TEST_CASE("Prepared Lab chroma preserves every scalar Sharma distance bit", "[ColorDifference][PreparedLabChroma]")
{
    namespace comparison = prepared_chroma_test;
    comparison::require_qualified_environment();
    std::ifstream data(std::string(TEST_DATA_DIR) + "/ciede2000_sharma.txt");
    REQUIRE(data.is_open());
    comparison::Lab first, second;
    double published;
    std::size_t rows = 0;
    while (data >> first[0] >> first[1] >> first[2] >> second[0] >> second[1] >> second[2] >> published) {
        CAPTURE(rows, published);
        comparison::require_pair(first, second);
        comparison::require_pair(first, first);
        comparison::require_pair(second, second);
        ++rows;
    }
    CHECK(data.eof());
    CHECK(rows == 34);
    // Existing regression for |h1'-h2'| > pi and h1'+h2' >= 2*pi.
    comparison::require_pair({50,3.1368345115760294,6.261846084332455},
                             {50,45.27944980884872,-87.85644254380878});
}

TEST_CASE("Prepared Lab chroma retains gray signed-zero and hue-boundary bits", "[ColorDifference][PreparedLabChroma]")
{
    namespace comparison = prepared_chroma_test;
    comparison::require_qualified_environment();
    const double infinity = std::numeric_limits<double>::infinity();
    const double tiny = std::numeric_limits<double>::denorm_min();
    const std::array<double, 8> lightness {0., tiny, 1., std::nextafter(50., -infinity),
        50., std::nextafter(50., infinity), 99., 100.};
    std::vector<comparison::Lab> gray;
    for (const double value : lightness)
        for (const double a : {0., -0.})
            for (const double b : {0., -0.}) gray.push_back({value,a,b});
    for (const auto& first : gray)
        for (const auto& second : gray) comparison::require_pair(first, second);

    std::vector<comparison::Lab> axes;
    for (const double perturbation : {0., -0., tiny, -tiny, 1e-12, -1e-12, 1e-6, -1e-6})
        for (const double sign : {-1., 1.}) {
            axes.push_back({50., sign * 25., perturbation});
            axes.push_back({50., perturbation, sign * 25.});
        }
    for (const auto& first : axes) {
        comparison::require_pair(first, {50.,0.,-0.});
        for (const auto& second : axes) comparison::require_pair(first, second);
    }
    // No test infers exact h' from a rounded angle. Axis values and their
    // neighboring coordinates exercise both sides of the actual scalar guards.
    for (const double magnitude : {tiny, std::numeric_limits<double>::min(), 1e-160, 1e-12, 1e-6, 1., 100., 1e6}) {
        comparison::require_pair({50.,magnitude,-magnitude}, {50.,-magnitude,magnitude});
        comparison::require_pair({0.,magnitude,0.}, {100.,0.,-magnitude});
        comparison::require_pair({50.,magnitude,-magnitude}, {50.,magnitude,-magnitude});
    }
}

TEST_CASE("Prepared Lab chroma matches general finite Lab pairs without a color-space assumption", "[ColorDifference][PreparedLabChroma]")
{
    namespace comparison = prepared_chroma_test;
    comparison::require_qualified_environment();
    std::mt19937_64 random(0x6d706c6162636163ULL);
    for (const double scale : {1e-6, 1., 200., 1e6})
        for (unsigned iteration = 0; iteration < 1024; ++iteration) {
            const comparison::Lab first {50. + comparison::random_component(random, 150.),
                comparison::random_component(random, scale), comparison::random_component(random, scale)};
            const comparison::Lab second {50. + comparison::random_component(random, 150.),
                comparison::random_component(random, scale), comparison::random_component(random, scale)};
            comparison::require_pair(first, second);
            if (iteration % 32 == 0) {
                comparison::require_pair(first, first);
                auto adjacent = first;
                adjacent[1] = std::nextafter(adjacent[1], std::numeric_limits<double>::infinity());
                comparison::require_pair(first, adjacent);
            }
        }
}

TEST_CASE("Prepared Lab chroma can be shared across independent query opponents and workers", "[ColorDifference][PreparedLabChroma]")
{
    namespace comparison = prepared_chroma_test;
    comparison::require_qualified_environment();
    namespace prepared = Slic3r::tex2color::color_utils::detail;
    namespace colors = Slic3r::tex2color::color_utils;
    std::mt19937_64 random(0x7072657061726564ULL);
    std::vector<comparison::Lab> points;
    for (unsigned index = 0; index < 256; ++index)
        points.push_back({50. + comparison::random_component(random, 150.),
            comparison::random_component(random, 200.), comparison::random_component(random, 200.)});
    points.push_back({50.,0.,-0.});
    std::vector<double> chroma(points.size(), -17.);
    for (std::size_t index = 0; index < points.size(); ++index)
        REQUIRE(prepared::prepare_lab_chroma(points[index], chroma[index]));
    constexpr std::size_t query_count = 4096;
    for (const int workers : {1, 4}) {
        std::vector<std::array<double, 2>> actual(query_count);
        tbb::task_arena arena(workers);
        arena.execute([&] {
            tbb::parallel_for(tbb::blocked_range<std::size_t>(0, query_count),
                [&](const tbb::blocked_range<std::size_t>& range) {
                    for (auto query = range.begin(); query < range.end(); ++query) {
                        const auto first = (query * 17 + 3) % points.size();
                        const auto second = (query * 83 + 11) % points.size();
                        actual[query][0] = prepared::ciede2000_prepared(points[first], points[second], chroma[first], chroma[second]);
                        actual[query][1] = prepared::ciede2000_prepared(points[second], points[first], chroma[second], chroma[first]);
                    }
                });
        });
        // Catch assertions stay on the caller thread; all workers only write
        // their own output slots and read shared immutable Lab/chroma arrays.
        for (std::size_t query = 0; query < query_count; ++query) {
            const auto first = (query * 17 + 3) % points.size();
            const auto second = (query * 83 + 11) % points.size();
            const auto expected_forward = colors::calc_lab_color_difference_by_ciede2000(points[first], points[second]);
            const auto expected_reverse = colors::calc_lab_color_difference_by_ciede2000(points[second], points[first]);
            CAPTURE(workers, query, first, second);
            CHECK(comparison::bits(actual[query][0]) == comparison::bits(expected_forward));
            CHECK(comparison::bits(actual[query][1]) == comparison::bits(expected_reverse));
        }
    }
}

TEST_CASE("Invalid prepared Lab chroma leaves the caller's previous value intact", "[ColorDifference][PreparedLabChroma]")
{
    namespace prepared = Slic3r::tex2color::color_utils::detail;
    namespace comparison = prepared_chroma_test;
    for (const double invalid : {std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()})
        for (std::size_t channel = 0; channel < 3; ++channel) {
            comparison::Lab value {50.,1.,-2.};
            value[channel] = invalid;
            double chroma = -17.125;
            CHECK_FALSE(prepared::prepare_lab_chroma(value, chroma));
            CHECK(comparison::bits(chroma) == comparison::bits(-17.125));
        }
    for (const comparison::Lab value : {comparison::Lab{50.,1e200,0.}, {50.,0.,-1e200},
            {50.,std::numeric_limits<double>::max(),std::numeric_limits<double>::max()}}) {
        double chroma = -17.125;
        CHECK_FALSE(prepared::prepare_lab_chroma(value, chroma));
        CHECK(comparison::bits(chroma) == comparison::bits(-17.125));
    }
}
