#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

// Optional Windows-only accelerator for the exact Python top-left/z-buffer
// raster contract. The Python implementation remains the portable fallback.
extern "C" __declspec(dllexport) int semantic_raster_abi_version() { return 1; }

extern "C" __declspec(dllexport) int raster_triangles(
    const double* triangles, const uint8_t* double_sided, int32_t face_count,
    int32_t size, int32_t* ids, double* depths, double* barycentric)
{
    if (!triangles || !double_sided || !ids || !depths || !barycentric ||
        face_count <= 0 || face_count > 2000000 || size <= 0 || size > 2048)
        return -1;
    const int64_t pixels = int64_t(size) * size;
    for (int64_t p = 0; p < pixels; ++p) {
        ids[p] = -1;
        depths[p] = -std::numeric_limits<double>::infinity();
        barycentric[p * 3] = barycentric[p * 3 + 1] = barycentric[p * 3 + 2] = 0.;
    }
    for (int32_t face = 0; face < face_count; ++face) {
        const double* t = triangles + int64_t(face) * 9;
        for (int k = 0; k < 9; ++k) if (!std::isfinite(t[k])) return -2;
        const double ax = t[0], ay = t[1], az = t[2];
        const double bx = t[3], by = t[4], bz = t[5];
        const double cx = t[6], cy = t[7], cz = t[8];
        const double minx = std::min({ax, bx, cx}), maxx = std::max({ax, bx, cx});
        const double miny = std::min({ay, by, cy}), maxy = std::max({ay, by, cy});
        if (maxx < .5 || maxy < .5 || minx >= size + .5 || miny >= size + .5)
            continue;
        const int x0 = int(std::clamp(std::ceil(minx - .5), 0., double(size)));
        const int y0 = int(std::clamp(std::ceil(miny - .5), 0., double(size)));
        const int x1 = int(std::clamp(std::floor(maxx - .5), -1., double(size - 1)));
        const int y1 = int(std::clamp(std::floor(maxy - .5), -1., double(size - 1)));
        if (x0 > x1 || y0 > y1) continue;
        const double denominator = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
        if (denominator == 0. || (denominator > 0. && !double_sided[face])) continue;
        const double sign = denominator > 0. ? 1. : -1.;
        const auto top_left = [sign](double qx, double qy, double rx, double ry) {
            const double dx = (rx - qx) * sign, dy = (ry - qy) * sign;
            return dy < 0. || (dy == 0. && dx > 0.);
        };
        const bool top0 = top_left(bx, by, cx, cy);
        const bool top1 = top_left(cx, cy, ax, ay);
        const bool top2 = top_left(ax, ay, bx, by);
        for (int y = y0; y <= y1; ++y) {
            const double py = y + .5;
            for (int x = x0; x <= x1; ++x) {
                const double px = x + .5;
                const double e0 = (by - cy) * (px - cx) + (cx - bx) * (py - cy);
                const double e1 = (cy - ay) * (px - ax) + (ax - cx) * (py - ay);
                const double e2 = (ay - by) * (px - bx) + (bx - ax) * (py - by);
                if (!(e0 * sign > 0. || (e0 == 0. && top0)) ||
                    !(e1 * sign > 0. || (e1 == 0. && top1)) ||
                    !(e2 * sign > 0. || (e2 == 0. && top2))) continue;
                const double w0 = e0 / denominator, w1 = e1 / denominator;
                const double w2 = 1. - w0 - w1;
                const double z = w0 * az + w1 * bz + w2 * cz;
                const int64_t pixel = int64_t(y) * size + x;
                if (z <= depths[pixel]) continue;
                ids[pixel] = face;
                depths[pixel] = z;
                barycentric[pixel * 3] = w0;
                barycentric[pixel * 3 + 1] = w1;
                barycentric[pixel * 3 + 2] = w2;
            }
        }
    }
    return 0;
}
