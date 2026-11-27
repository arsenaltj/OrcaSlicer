#pragma once
#include <array>
#include <algorithm>
#include <cmath>
namespace Slic3r::AI::ColorDifference {
using XYZ=std::array<double,3>;using LAB=XYZ;using ColorDouble=XYZ;
inline constexpr XYZ D65_WHITE={0.95047,1.0,1.08883};
inline XYZ convert_srgb01_to_xyz(double rs, double gs, double bs) {
    auto gamma_correct = [](double v) -> double {
        v = std::clamp(v, 0.0, 1.0);
        return (v > 0.04045) ? std::pow((v + 0.055) / 1.055, 2.4) : (v / 12.92);
    };
    const double r = gamma_correct(rs);
    const double g = gamma_correct(gs);
    const double b = gamma_correct(bs);
    return {r * 0.4124564 + g * 0.3575761 + b * 0.1804375, r * 0.2126729 + g * 0.7151522 + b * 0.0721750, r * 0.0193339 + g * 0.1191920 + b * 0.9503041};
}
inline LAB convert_xyz_to_lab(const XYZ& xyz) {
    auto f = [](double t) -> double {
        const double delta = 6.0 / 29.0;
        if (t > delta * delta * delta) {
            return std::cbrt(t);
        } else {
            return t / (3.0 * delta * delta) + 4.0 / 29.0;
        }
    };

    // D65 white point
    double xn = D65_WHITE[0], yn = D65_WHITE[1], zn = D65_WHITE[2];

    double fx = f(xyz[0] / xn);
    double fy = f(xyz[1] / yn);
    double fz = f(xyz[2] / zn);

    return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}
inline LAB convert_srgb01_to_lab(const ColorDouble& srgb01) {
    return convert_xyz_to_lab(convert_srgb01_to_xyz(srgb01[0], srgb01[1], srgb01[2]));
}
inline double ciede2000(const std::array<double, 3>& lab1, const std::array<double, 3>& lab2) {
    // Parameters in the CIE L*C*h* formula
    double L1 = lab1[0], a1 = lab1[1], b1 = lab1[2];
    double L2 = lab2[0], a2 = lab2[1], b2 = lab2[2];

    // Compute C1 and C2
    double C1 = std::sqrt(a1 * a1 + b1 * b1);
    double C2 = std::sqrt(a2 * a2 + b2 * b2);
    double C_avg = (C1 + C2) / 2.0;

    // G factor (compensates for non-linearity in the mid-low chroma region)
    double C7 = C_avg * C_avg * C_avg * C_avg * C_avg * C_avg * C_avg;
    double G = 0.5 * (1.0 - std::sqrt(C7 / (C7 + 6103515625.0)));

    // a1' and a2'
    double a1_prime = (1.0 + G) * a1;
    double a2_prime = (1.0 + G) * a2;

    // C'1 and C'2
    double C1_prime = std::sqrt(a1_prime * a1_prime + b1 * b1);
    double C2_prime = std::sqrt(a2_prime * a2_prime + b2 * b2);
    double C_prime_avg = (C1_prime + C2_prime) / 2.0;

    // h'1 and h'2
    double h1_prime = std::atan2(b1, a1_prime);
    double h2_prime = std::atan2(b2, a2_prime);
    if (h1_prime < 0) {
        h1_prime += 2 * 3.14159265358979323846;
    }
    if (h2_prime < 0) {
        h2_prime += 2 * 3.14159265358979323846;
    }

    // Compute dh'
    double dh_prime;
    if (C1_prime * C2_prime == 0.0) {
        dh_prime = 0.0;
    } else if (std::abs(h1_prime - h2_prime) <= 3.14159265358979323846) {
        dh_prime = h2_prime - h1_prime;
    } else if (h2_prime <= h1_prime) {
        dh_prime = h2_prime - h1_prime + 2 * 3.14159265358979323846;
    } else {
        dh_prime = h2_prime - h1_prime - 2 * 3.14159265358979323846;
    }

    // Compute dH'
    double dH_prime = 2.0 * std::sqrt(C1_prime * C2_prime) * std::sin(dh_prime / 2.0);

    // Compute dL'
    double dL_prime = L2 - L1;

    // Compute dC'
    double dC_prime = C2_prime - C1_prime;

    // Compute h_prime_avg
    double h_prime_avg;
    if (C1_prime * C2_prime == 0.0) {
        h_prime_avg = h1_prime + h2_prime;
    } else if (std::abs(h1_prime - h2_prime) > 3.14159265358979323846) {
        const double sum = h1_prime + h2_prime;
        h_prime_avg = (sum + (sum < 2 * 3.14159265358979323846 ? 2 * 3.14159265358979323846 : -2 * 3.14159265358979323846)) / 2.0;
    } else {
        h_prime_avg = (h1_prime + h2_prime) / 2.0;
    }

    // Compute T
    double T = 1.0 - 0.17 * std::cos(h_prime_avg - 3.14159265358979323846 / 6.0) + 0.24 * std::cos(2.0 * h_prime_avg) + 0.32 * std::cos(3.0 * h_prime_avg + 3.14159265358979323846 / 30.0) -
               0.20 * std::cos(4.0 * h_prime_avg - 63.0 * 3.14159265358979323846 / 180.0);

    // Compute rotation term R_T = -R_C * sin(2*delta_theta), where delta_theta = 30 * exp(-((h_bar'-275)/25)^2)
    // h_prime_avg is in radians; convert to degrees for delta_theta; 2*delta_theta = 60 * exp(...), convert back to radians for sin
    double h_prime_avg_deg = h_prime_avg * 180.0 / 3.14159265358979323846;
    double C_prime_avg_7 = std::pow(C_prime_avg, 7);
    double R = -2.0 * std::sqrt(C_prime_avg_7 / (C_prime_avg_7 + 6103515625.0)) *
               std::sin((60.0 * 3.14159265358979323846 / 180.0) * std::exp(-std::pow((h_prime_avg_deg - 275.0) / 25.0, 2)));

    // Compute SL, SC, SH
    double L_prime_avg = (L1 + L2) / 2.0;
    double SL = 1.0 + 0.015 * std::pow(L_prime_avg - 50.0, 2) / std::sqrt(20 + std::pow(L_prime_avg - 50.0, 2));
    double SC = 1.0 + 0.045 * C_prime_avg;
    double SH = 1.0 + 0.015 * C_prime_avg * T;

    // Final color difference
    double deltaE = std::sqrt(std::pow(dL_prime / SL, 2) + std::pow(dC_prime / SC, 2) + std::pow(dH_prime / SH, 2) + R * (dC_prime / SC) * (dH_prime / SH));

    return deltaE;
}
inline double srgb01(const ColorDouble& a,const ColorDouble& b){return ciede2000(convert_srgb01_to_lab(a),convert_srgb01_to_lab(b));}
}
