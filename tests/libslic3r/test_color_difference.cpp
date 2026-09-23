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
