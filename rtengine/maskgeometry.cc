/* -*- C++ -*-
 *
 *  This file is part of RawTherapee.
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  RawTherapee is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RawTherapee.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "maskgeometry.h"

#include <algorithm>
#include <array>
#include <functional>
#include <queue>
#define _USE_MATH_DEFINES   // M_PI / M_SQRT2 on MinGW and MSVC
#include <cmath>
#include <utility>
#include <vector>

namespace rtengine
{

namespace
{

constexpr float LAB_L_SCALE = 327.68f;   // internal L (0..32768) -> L* (0..100)
constexpr float LAB_AB_SCALE = 327.68f;  // internal a/b -> a*/b*

inline float clamp01(float v)
{
    return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}

struct Segment {
    double ax, ay, bx, by;   // end points, buffer coordinates
    double minX, maxX, minY, maxY;   // bounding box, radius included
};

/// squared distance from p to segment ab
inline double segmentDistanceSquared(double px, double py, const Segment& s)
{
    const double dx = s.bx - s.ax;
    const double dy = s.by - s.ay;
    const double len2 = dx * dx + dy * dy;
    double t = 0.0;

    if (len2 > 1e-12) {
        t = ((px - s.ax) * dx + (py - s.ay) * dy) / len2;
        t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    }

    const double cx = s.ax + t * dx - px;
    const double cy = s.ay + t * dy - py;
    return cx * cx + cy * cy;
}

}

double MaskFrame::diagonal() const
{
    return std::sqrt(double(imageWidth) * imageWidth + double(imageHeight) * imageHeight);
}

namespace maskgeom
{

float smoothstep(float e0, float e1, float x)
{
    if (e1 <= e0) {
        return x >= e0 ? 1.f : 0.f;
    }

    const float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.f - 2.f * t);
}

void linearGradient(const procparams::MaskComponent& c, const MaskFrame& frame, array2D<float>& out, bool multiThread)
{
    const int W = out.getWidth();
    const int H = out.getHeight();

    const double ax = frame.bufferX(c.x0);
    const double ay = frame.bufferY(c.y0);
    const double bx = frame.bufferX(c.x1);
    const double by = frame.bufferY(c.y1);
    const double dx = bx - ax;
    const double dy = by - ay;
    const double len2 = dx * dx + dy * dy;

    if (len2 < 1e-9) {
        // degenerate gradient: fully selected everywhere
        out.fill(1.f, multiThread);
        return;
    }

#ifdef _OPENMP
    #pragma omp parallel for if (multiThread)
#endif
    for (int y = 0; y < H; ++y) {
        const double py = y + 0.5;
        float* row = out[y];

        for (int x = 0; x < W; ++x) {
            const double px = x + 0.5;
            const double t = ((px - ax) * dx + (py - ay) * dy) / len2;
            row[x] = 1.f - smoothstep(0.f, 1.f, float(t));
        }
    }
}

void radialGradient(const procparams::MaskComponent& c, const MaskFrame& frame, array2D<float>& out, bool multiThread)
{
    const int W = out.getWidth();
    const int H = out.getHeight();

    const double diag = frame.diagonal();
    const double cx = frame.bufferX(c.centerX);
    const double cy = frame.bufferY(c.centerY);
    const double rx = std::max(c.radiusX * diag, 1e-3);
    const double ry = std::max(c.radiusY * diag, 1e-3);
    const double rad = c.angle * M_PI / 180.0;
    const double cosA = std::cos(rad);
    const double sinA = std::sin(rad);
    const float inner = 1.f - clamp01(float(c.feather / 100.0));

#ifdef _OPENMP
    #pragma omp parallel for if (multiThread)
#endif
    for (int y = 0; y < H; ++y) {
        const double dy = (y + 0.5) - cy;
        float* row = out[y];

        for (int x = 0; x < W; ++x) {
            const double dx = (x + 0.5) - cx;
            const double u = (cosA * dx + sinA * dy) / rx;
            const double v = (-sinA * dx + cosA * dy) / ry;
            const float rho = float(std::sqrt(u * u + v * v));
            row[x] = 1.f - smoothstep(inner, 1.f, rho);
        }
    }
}

namespace
{

/// Segments of a stroke starting at segment 'firstSegment' (a single point is one zero-length segment).
void strokeSegments(const procparams::MaskBrushStroke& stroke, const MaskFrame& frame, size_t firstSegment, double radius, std::vector<Segment>& segments, double& minX, double& maxX, double& minY, double& maxY)
{
    const size_t n = stroke.pointCount();
    segments.clear();
    minX = 1e30;
    maxX = -1e30;
    minY = 1e30;
    maxY = -1e30;

    if (n == 0) {
        return;
    }

    auto bufferPoint = [&](size_t i, double& x, double& y) {
        x = frame.bufferX(stroke.points[2 * i]);
        y = frame.bufferY(stroke.points[2 * i + 1]);
    };

    const size_t count = std::max<size_t>(n, 2) - 1;
    segments.reserve(count > firstSegment ? count - firstSegment : 0);

    for (size_t i = firstSegment; i < count; ++i) {
        Segment s;
        bufferPoint(i, s.ax, s.ay);

        if (n == 1) {
            s.bx = s.ax;
            s.by = s.ay;
        } else {
            bufferPoint(i + 1, s.bx, s.by);
        }

        s.minX = std::min(s.ax, s.bx) - radius;
        s.maxX = std::max(s.ax, s.bx) + radius;
        s.minY = std::min(s.ay, s.by) - radius;
        s.maxY = std::max(s.ay, s.by) + radius;
        minX = std::min(minX, s.minX);
        maxX = std::max(maxX, s.maxX);
        minY = std::min(minY, s.minY);
        maxY = std::max(maxY, s.maxY);
        segments.push_back(s);
    }
}

inline double strokeRadius(const procparams::MaskBrushStroke& stroke, const MaskFrame& frame)
{
    return std::max(stroke.size, 1e-5) * frame.diagonal();
}

}

bool MaskBox::empty() const
{
    return x0 > x1 || y0 > y1;
}

void MaskBox::unite(const MaskBox& o)
{
    if (o.empty()) {
        return;
    }

    if (empty()) {
        *this = o;
        return;
    }

    x0 = std::min(x0, o.x0);
    y0 = std::min(y0, o.y0);
    x1 = std::max(x1, o.x1);
    y1 = std::max(y1, o.y1);
}

MaskBox strokeBox(const procparams::MaskBrushStroke& stroke, const MaskFrame& frame, int W, int H, size_t firstSegment)
{
    MaskBox box;
    std::vector<Segment> segments;
    double minX, maxX, minY, maxY;
    strokeSegments(stroke, frame, firstSegment, strokeRadius(stroke, frame), segments, minX, maxX, minY, maxY);

    if (segments.empty() || W <= 0 || H <= 0) {
        return box;
    }

    box.y0 = std::max(int(std::floor(minY)), 0);
    box.y1 = std::min(int(std::ceil(maxY)), H - 1);
    box.x0 = std::max(int(std::floor(minX)), 0);
    box.x1 = std::min(int(std::ceil(maxX)), W - 1);

    if (box.empty()) {
        box = MaskBox();
    }

    return box;
}

MaskBox strokeCoverageSegments(const procparams::MaskBrushStroke& stroke, const MaskFrame& frame, array2D<float>& out, size_t firstSegment, bool multiThread)
{
    const int W = out.getWidth();
    const int H = out.getHeight();

    if (stroke.pointCount() == 0 || W <= 0 || H <= 0) {
        return MaskBox();
    }

    const double radius = strokeRadius(stroke, frame);
    const double hard = radius * (1.0 - clamp01(float(stroke.feather)));
    const double soft = std::max(radius - hard, 1e-6);

    std::vector<Segment> segments;
    double minX, maxX, minY, maxY;
    strokeSegments(stroke, frame, firstSegment, radius, segments, minX, maxX, minY, maxY);

    if (segments.empty()) {
        return MaskBox();
    }

    // Bounding box of the segments, clipped to the buffer.
    const int y0 = std::max(int(std::floor(minY)), 0);
    const int y1 = std::min(int(std::ceil(maxY)), H - 1);
    const int bx0 = std::max(int(std::floor(minX)), 0);
    const int bx1 = std::min(int(std::ceil(maxX)), W - 1);

    if (y0 > y1 || bx0 > bx1) {
        return MaskBox();   // stroke entirely outside of this buffer
    }

    const double radius2 = radius * radius;

#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic, 16) if (multiThread)
#endif
    for (int y = y0; y <= y1; ++y) {
        const double py = y + 0.5;
        float* row = out[y];

        for (const Segment& s : segments) {
            if (py < s.minY || py > s.maxY) {
                continue;
            }

            const int sx0 = std::max(int(std::floor(s.minX)), bx0);
            const int sx1 = std::min(int(std::ceil(s.maxX)), bx1);

            for (int x = sx0; x <= sx1; ++x) {
                const double d2 = segmentDistanceSquared(x + 0.5, py, s);

                if (d2 >= radius2) {
                    continue;
                }

                float cov;
                const double d = std::sqrt(d2);

                if (d <= hard) {
                    cov = 1.f;
                } else {
                    cov = 1.f - smoothstep(0.f, 1.f, float((d - hard) / soft));
                }

                if (cov > row[x]) {
                    row[x] = cov;
                }
            }
        }
    }

    MaskBox box;
    box.x0 = bx0;
    box.y0 = y0;
    box.x1 = bx1;
    box.y1 = y1;
    return box;
}

void strokeCoverage(const procparams::MaskBrushStroke& stroke, const MaskFrame& frame, array2D<float>& out, bool multiThread)
{
    strokeCoverageSegments(stroke, frame, out, 0, multiThread);
}

void applyStrokeCoverage(const array2D<float>& base, const array2D<float>& cov, const MaskBox& box, float flow, bool erase, array2D<float>& out, bool multiThread)
{
    // Clip to the smallest buffer: a box may come from a cache entry made for
    // a buffer of another size (detail window resized).
    const int W = std::min(std::min(base.getWidth(), cov.getWidth()), out.getWidth());
    const int H = std::min(std::min(base.getHeight(), cov.getHeight()), out.getHeight());
    const int x0 = std::max(box.x0, 0);
    const int x1 = std::min(box.x1, W - 1);
    const int y0 = std::max(box.y0, 0);
    const int y1 = std::min(box.y1, H - 1);

    if (box.empty() || x0 > x1 || y0 > y1) {
        return;
    }

#ifdef _OPENMP
    #pragma omp parallel for if (multiThread)
#endif
    for (int y = y0; y <= y1; ++y) {
        const float* b = base[y];
        const float* s = cov[y];
        float* m = out[y];

        for (int x = x0; x <= x1; ++x) {
            const float v = flow * s[x];
            float r = b[x];

            if (v > 0.f) {
                if (erase) {
                    r *= (1.f - v);
                } else {
                    r += v * (1.f - r);
                }
            }

            m[x] = r;
        }
    }
}

void clearBox(array2D<float>& m, const MaskBox& box, bool multiThread)
{
    const int x0 = std::max(box.x0, 0);
    const int x1 = std::min(box.x1, m.getWidth() - 1);
    const int y0 = std::max(box.y0, 0);
    const int y1 = std::min(box.y1, m.getHeight() - 1);

    if (box.empty() || x0 > x1 || y0 > y1) {
        return;
    }

#ifdef _OPENMP
    #pragma omp parallel for if (multiThread)
#endif
    for (int y = y0; y <= y1; ++y) {
        std::fill(m[y] + x0, m[y] + x1 + 1, 0.f);
    }
}

void brush(const procparams::MaskComponent& c, const MaskFrame& frame, array2D<float>& out, bool multiThread)
{
    out.fill(0.f, multiThread);
    brushApply(c, frame, out, false, multiThread);
}

void brushApply(const procparams::MaskComponent& c, const MaskFrame& frame, array2D<float>& out, bool subtractMode, bool multiThread)
{
    const int W = out.getWidth();
    const int H = out.getHeight();

    if (c.strokes.empty()) {
        return;
    }

    // Each stroke only touches its bounding box: clear, rasterize and
    // accumulate there instead of over the whole buffer.
    array2D<float> cov(W, H);

    for (const auto& stroke : c.strokes) {
        if (stroke.pointCount() == 0) {
            continue;
        }

        const float flow = clamp01(float(stroke.flow));

        if (flow <= 0.f) {
            continue;
        }

        const MaskBox box = strokeBox(stroke, frame, W, H);

        if (box.empty()) {
            continue;
        }

        clearBox(cov, box, multiThread);
        strokeCoverageSegments(stroke, frame, cov, 0, multiThread);
        applyStrokeCoverage(out, cov, box, flow, stroke.erase != subtractMode, out, multiThread);
    }
}

float luminanceRangeWeight(const procparams::MaskComponent& c, float Lstar)
{
    const float low = float(c.lumLow);
    const float high = float(c.lumHigh);
    const float fl = std::max(float(c.lumFeatherLow), 0.f);
    const float fh = std::max(float(c.lumFeatherHigh), 0.f);

    const float rise = smoothstep(low - fl, low, Lstar);
    const float fall = 1.f - smoothstep(high, high + fh, Lstar);
    return rise * fall;
}

void luminanceRange(const procparams::MaskComponent& c, const float* const* L, int W, int H, array2D<float>& out, bool multiThread)
{
#ifdef _OPENMP
    #pragma omp parallel for if (multiThread)
#endif
    for (int y = 0; y < H; ++y) {
        float* row = out[y];
        const float* l = L[y];

        for (int x = 0; x < W; ++x) {
            row[x] = luminanceRangeWeight(c, l[x] / LAB_L_SCALE);
        }
    }
}

float colorRangeWeight(const procparams::MaskComponent& c, float Lstar, float astar, float bstar)
{
    return colorWeight(float(c.sampleL), float(c.sampleA), float(c.sampleB), c.tolerance, c.smoothness, Lstar, astar, bstar);
}

void colorRange(const procparams::MaskComponent& c, const float* const* L, const float* const* a, const float* const* b, int W, int H, array2D<float>& out, bool multiThread)
{
#ifdef _OPENMP
    #pragma omp parallel for if (multiThread)
#endif
    for (int y = 0; y < H; ++y) {
        float* row = out[y];
        const float* l = L[y];
        const float* aa = a[y];
        const float* bb = b[y];

        for (int x = 0; x < W; ++x) {
            row[x] = colorRangeWeight(c, l[x] / LAB_L_SCALE, aa[x] / LAB_AB_SCALE, bb[x] / LAB_AB_SCALE);
        }
    }
}

// ---------------------------------------------------------------------------
// Rectangle marquee

void rectangle(const procparams::MaskComponent& c, const MaskFrame& frame, array2D<float>& out, bool multiThread)
{
    const int W = out.getWidth();
    const int H = out.getHeight();

    const double ax = frame.bufferX(c.x0);
    const double ay = frame.bufferY(c.y0);
    const double bx = frame.bufferX(c.x1);
    const double by = frame.bufferY(c.y1);
    const double cx = 0.5 * (ax + bx);
    const double cy = 0.5 * (ay + by);
    const double hw = std::max(0.5 * std::fabs(bx - ax), 0.5);
    const double hh = std::max(0.5 * std::fabs(by - ay), 0.5);
    const double minHalf = std::min(hw, hh);
    const double corner = clamp01(float(c.cornerRadius / 100.0)) * minHalf;
    const double featherPx = std::max(clamp01(float(c.feather / 100.0)) * minHalf, 1.0);   // at least a 1 px anti-aliasing ramp
    const double rad = c.angle * M_PI / 180.0;
    const double cosA = std::cos(rad);
    const double sinA = std::sin(rad);
    const double ex = hw - corner;
    const double ey = hh - corner;

#ifdef _OPENMP
    #pragma omp parallel for if (multiThread)
#endif
    for (int y = 0; y < H; ++y) {
        const double dy = (y + 0.5) - cy;
        float* row = out[y];

        for (int x = 0; x < W; ++x) {
            const double dx = (x + 0.5) - cx;
            const double u = cosA * dx + sinA * dy;
            const double v = -sinA * dx + cosA * dy;
            // signed distance to a rounded box (negative inside)
            const double qx = std::fabs(u) - ex;
            const double qy = std::fabs(v) - ey;
            const double ox = std::max(qx, 0.0);
            const double oy = std::max(qy, 0.0);
            const double d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0) - corner;
            row[x] = 1.f - smoothstep(float(-featherPx), 0.f, float(d));
        }
    }
}

// ---------------------------------------------------------------------------
// Polygon (lasso)

namespace
{

constexpr float EDT_INF = 1e20f;

/// 1D squared euclidean distance transform (Felzenszwalb & Huttenlocher).
void edt1d(const float* f, float* d, int n, std::vector<int>& v, std::vector<double>& z)
{
    v.resize(n);
    z.resize(n + 1);
    int k = 0;
    v[0] = 0;
    z[0] = -1e30;
    z[1] = 1e30;

    for (int q = 1; q < n; ++q) {
        double s = ((double(f[q]) + double(q) * q) - (double(f[v[k]]) + double(v[k]) * v[k])) / (2.0 * q - 2.0 * v[k]);

        while (s <= z[k]) {
            --k;
            s = ((double(f[q]) + double(q) * q) - (double(f[v[k]]) + double(v[k]) * v[k])) / (2.0 * q - 2.0 * v[k]);
        }

        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = 1e30;
    }

    k = 0;

    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < q) {
            ++k;
        }

        const double dq = double(q) - v[k];
        d[q] = float(dq * dq + f[v[k]]);
    }
}

/**
 * Squared distance of every pixel to the nearest pixel where
 * (mask[y][x] != 0) == feature. 'dist' must be W x H.
 */
void edt2d(const std::vector<unsigned char>& mask, int W, int H, bool feature, array2D<float>& dist, bool multiThread)
{
#ifdef _OPENMP
    #pragma omp parallel if (multiThread)
#endif
    {
        std::vector<int> v;
        std::vector<double> z;
        std::vector<float> f(std::max(W, H));
        std::vector<float> d(std::max(W, H));

#ifdef _OPENMP
        #pragma omp for
#endif
        for (int y = 0; y < H; ++y) {
            const unsigned char* mrow = &mask[size_t(y) * W];

            for (int x = 0; x < W; ++x) {
                f[x] = ((mrow[x] != 0) == feature) ? 0.f : EDT_INF;
            }

            edt1d(f.data(), dist[y], W, v, z);
        }

#ifdef _OPENMP
        #pragma omp for
#endif
        for (int x = 0; x < W; ++x) {
            for (int y = 0; y < H; ++y) {
                f[y] = dist[y][x];
            }

            edt1d(f.data(), d.data(), H, v, z);

            for (int y = 0; y < H; ++y) {
                dist[y][x] = d[y];
            }
        }
    }
}

struct Crossing {
    double x;
    int dir;
};

/// Anti-aliased scanline fill of a closed polygon (vertices in buffer coordinates) into 'cov' (W x H, zero initialised).
void fillPolygon(const std::vector<double>& vx, const std::vector<double>& vy, bool evenOdd, array2D<float>& cov, bool multiThread)
{
    const int W = cov.getWidth();
    const int H = cov.getHeight();
    const size_t n = vx.size();
    constexpr int SUB = 4;
    constexpr float SUB_WEIGHT = 1.f / SUB;

    double minY = 1e30, maxY = -1e30;

    for (size_t i = 0; i < n; ++i) {
        minY = std::min(minY, vy[i]);
        maxY = std::max(maxY, vy[i]);
    }

    const int y0 = std::max(int(std::floor(minY)), 0);
    const int y1 = std::min(int(std::ceil(maxY)), H - 1);

    if (y0 > y1) {
        return;
    }

#ifdef _OPENMP
    #pragma omp parallel if (multiThread)
#endif
    {
        std::vector<Crossing> crossings;

#ifdef _OPENMP
        #pragma omp for schedule(dynamic, 16)
#endif
        for (int y = y0; y <= y1; ++y) {
            float* row = cov[y];

            for (int s = 0; s < SUB; ++s) {
                const double sy = y + (s + 0.5) / SUB;
                crossings.clear();

                for (size_t i = 0; i < n; ++i) {
                    const size_t j = (i + 1) % n;
                    const double ya = vy[i], yb = vy[j];

                    if ((ya <= sy && sy < yb) || (yb <= sy && sy < ya)) {
                        const double t = (sy - ya) / (yb - ya);
                        crossings.push_back({vx[i] + t * (vx[j] - vx[i]), yb > ya ? 1 : -1});
                    }
                }

                if (crossings.size() < 2) {
                    continue;
                }

                std::sort(crossings.begin(), crossings.end(), [](const Crossing& l, const Crossing& r) {
                    return l.x < r.x;
                });

                int winding = 0;

                for (size_t k = 0; k + 1 < crossings.size(); ++k) {
                    winding += evenOdd ? 1 : crossings[k].dir;
                    const bool inside = evenOdd ? (winding & 1) : (winding != 0);

                    if (!inside) {
                        continue;
                    }

                    const double xa = std::max(crossings[k].x, 0.0);
                    const double xb = std::min(crossings[k + 1].x, double(W));

                    if (xb <= xa) {
                        continue;
                    }

                    const int ia = int(std::floor(xa));
                    const int ib = int(std::floor(xb));

                    if (ia == ib) {
                        row[ia] += SUB_WEIGHT * float(xb - xa);
                    } else {
                        row[ia] += SUB_WEIGHT * float(ia + 1 - xa);

                        for (int x = ia + 1; x < ib; ++x) {
                            row[x] += SUB_WEIGHT;
                        }

                        if (ib < W) {
                            row[ib] += SUB_WEIGHT * float(xb - ib);
                        }
                    }
                }
            }

            for (int x = 0; x < W; ++x) {
                row[x] = clamp01(row[x]);
            }
        }
    }
}

}

double polygonFeatherPixels(const procparams::MaskComponent& c, const MaskFrame& frame)
{
    return clamp01(float(c.feather / 100.0)) * 0.03 * frame.diagonal();
}

void polygon(const procparams::MaskComponent& c, const MaskFrame& frame, array2D<float>& out, bool multiThread)
{
    const int W = out.getWidth();
    const int H = out.getHeight();
    const size_t n = c.pointCount();

    out.fill(0.f, multiThread);

    if (n < 3 || W <= 0 || H <= 0) {
        return;
    }

    const double featherPx = polygonFeatherPixels(c, frame);

    // Padded window (clipped to the image) so that the distance transform
    // sees the same outline on a detail crop as on the full frame.
    const int pad = featherPx > 0.0 ? int(std::ceil(featherPx)) + 1 : 0;
    const int padL = std::min(pad, std::max(frame.offsetX, 0));
    const int padT = std::min(pad, std::max(frame.offsetY, 0));
    const int padR = std::min(pad, std::max(frame.imageWidth - (frame.offsetX + W), 0));
    const int padB = std::min(pad, std::max(frame.imageHeight - (frame.offsetY + H), 0));
    const int PW = W + padL + padR;
    const int PH = H + padT + padB;

    std::vector<double> vx(n), vy(n);

    for (size_t i = 0; i < n; ++i) {
        vx[i] = frame.bufferX(c.points[2 * i]) + padL;
        vy[i] = frame.bufferY(c.points[2 * i + 1]) + padT;
    }

    array2D<float> cov(PW, PH);
    cov.fill(0.f, multiThread);
    fillPolygon(vx, vy, c.evenOdd, cov, multiThread);

    if (featherPx <= 0.0) {
#ifdef _OPENMP
        #pragma omp parallel for if (multiThread)
#endif
        for (int y = 0; y < H; ++y) {
            const float* src = cov[y + padT] + padL;
            float* dst = out[y];

            for (int x = 0; x < W; ++x) {
                dst[x] = src[x];
            }
        }

        return;
    }

    // The feather only depends on the distances up to sqrt(4 r^2 + 4) (see
    // the clamping below): work on the polygon's bounding box grown by that
    // margin instead of the whole buffer. Outside of it, every pixel is
    // farther than the margin from the polygon, so its value is exactly 0.
    const float r = float(featherPx);
    const float maxDist2 = 4.f * r * r + 4.f;
    const int margin = int(std::ceil(std::sqrt(double(maxDist2)))) + 2;
    double minX = 1e30, maxX = -1e30, minY = 1e30, maxY = -1e30;

    for (size_t i = 0; i < n; ++i) {
        minX = std::min(minX, vx[i]);
        maxX = std::max(maxX, vx[i]);
        minY = std::min(minY, vy[i]);
        maxY = std::max(maxY, vy[i]);
    }

    const int wx0 = int(std::max(std::floor(minX) - margin, 0.0));
    const int wy0 = int(std::max(std::floor(minY) - margin, 0.0));
    const int wx1 = int(std::min(std::ceil(maxX) + margin, double(PW - 1)));
    const int wy1 = int(std::min(std::ceil(maxY) + margin, double(PH - 1)));

    if (wx0 > wx1 || wy0 > wy1) {
        return;   // no pixel inside
    }

    const int WW = wx1 - wx0 + 1;
    const int WH = wy1 - wy0 + 1;
    std::vector<unsigned char> inside(size_t(WW) * WH);
    bool anyIn = false;
    bool anyOut = WW < PW || WH < PH;   // pixels outside of the window are outside

    for (int y = 0; y < WH; ++y) {
        for (int x = 0; x < WW; ++x) {
            const bool in = cov[y + wy0][x + wx0] >= 0.5f;
            inside[size_t(y) * WW + x] = in ? 1 : 0;
            anyIn |= in;
            anyOut |= !in;
        }
    }

    if (!anyIn) {
        return;
    }

    if (!anyOut) {
        out.fill(1.f, multiThread);
        return;
    }

    array2D<float> distToOut(WW, WH), distToIn(WW, WH);
    edt2d(inside, WW, WH, false, distToOut, multiThread);
    edt2d(inside, WW, WH, true, distToIn, multiThread);

    const int y0 = std::max(wy0 - padT, 0);
    const int y1 = std::min(wy1 - padT, H - 1);
    const int x0 = std::max(wx0 - padL, 0);
    const int x1 = std::min(wx1 - padL, W - 1);

#ifdef _OPENMP
    #pragma omp parallel for if (multiThread)
#endif
    for (int y = y0; y <= y1; ++y) {
        const int py = y + padT;
        const int wy = py - wy0;
        float* dst = out[y];

        for (int x = x0; x <= x1; ++x) {
            const int px = x + padL;
            const int wx = px - wx0;
            const float cv = cov[py][px];
            float d;   // signed distance to the outline, positive inside

            if (cv > 0.f && cv < 1.f) {
                d = cv - 0.5f;
            } else if (inside[size_t(wy) * WW + wx]) {
                d = std::sqrt(std::min(distToOut[wy][wx], maxDist2)) - 0.5f;
            } else {
                d = 0.5f - std::sqrt(std::min(distToIn[wy][wx], maxDist2));
            }

            dst[x] = smoothstep(-r, r, d);
        }
    }
}

// ---------------------------------------------------------------------------
// Magic wand

float colorWeight(float refL, float refA, float refB, double tolerance, double smoothness, float Lstar, float astar, float bstar)
{
    // Weighted Lab distance: lightness counts less than chroma/hue so that
    // shading of the same colored surface stays inside the mask.
    constexpr float wL = 0.5f;
    const float dL = (Lstar - refL) * wL;
    const float da = astar - refA;
    const float db = bstar - refB;
    const float dE = std::sqrt(dL * dL + da * da + db * db);

    // tolerance 0..100 -> accepted radius 2..62 dE
    const float radius = 2.f + clamp01(float(tolerance / 100.0)) * 60.f;
    const float softness = clamp01(float(smoothness / 100.0));
    const float innerRadius = radius * (1.f - softness);
    return 1.f - smoothstep(innerRadius, radius, dE);
}

float wandWeight(const procparams::MaskComponent& c, float Lstar, float astar, float bstar)
{
    float w = 0.f;

    for (size_t i = 0; i + 4 < c.seeds.size(); i += 5) {
        w = std::max(w, colorWeight(float(c.seeds[i + 2]), float(c.seeds[i + 3]), float(c.seeds[i + 4]), c.tolerance, c.smoothness, Lstar, astar, bstar));

        if (w >= 1.f) {
            break;
        }
    }

    return w;
}

namespace
{

/// 4-connected scanline flood fill over (sim > 0) from the queued seeds; marks 'region'.
void floodFill(const array2D<float>& sim, std::vector<unsigned char>& region, std::vector<std::pair<int, int>>& stack)
{
    const int W = sim.getWidth();
    const int H = sim.getHeight();

    auto open = [&](int x, int y) {
        return sim[y][x] > 0.f && !region[size_t(y) * W + x];
    };

    while (!stack.empty()) {
        const auto p = stack.back();
        stack.pop_back();
        int x = p.first;
        const int y = p.second;

        if (!open(x, y)) {
            continue;
        }

        int x1 = x, x2 = x;

        while (x1 > 0 && open(x1 - 1, y)) {
            --x1;
        }

        while (x2 < W - 1 && open(x2 + 1, y)) {
            ++x2;
        }

        unsigned char* rrow = &region[size_t(y) * W];

        for (int i = x1; i <= x2; ++i) {
            rrow[i] = 1;
        }

        for (const int ny : {y - 1, y + 1}) {
            if (ny < 0 || ny >= H) {
                continue;
            }

            bool inRun = false;

            for (int i = x1; i <= x2; ++i) {
                const bool o = open(i, ny);

                if (o && !inRun) {
                    stack.emplace_back(i, ny);
                }

                inRun = o;
            }
        }
    }
}

}

void magicWand(const procparams::MaskComponent& c, const MaskFrame& frame, const float* const* L, const float* const* a, const float* const* b, int W, int H, array2D<float>& out, bool multiThread, const LabPlanes* reference)
{
    out.fill(0.f, multiThread);

    if (c.seedCount() == 0 || W <= 0 || H <= 0) {
        return;
    }

#ifdef _OPENMP
    #pragma omp parallel for if (multiThread)
#endif
    for (int y = 0; y < H; ++y) {
        float* row = out[y];
        const float* l = L[y];
        const float* aa = a[y];
        const float* bb = b[y];

        for (int x = 0; x < W; ++x) {
            row[x] = wandWeight(c, l[x] / LAB_L_SCALE, aa[x] / LAB_AB_SCALE, bb[x] / LAB_AB_SCALE);
        }
    }

    if (!c.contiguous) {
        return;
    }

    std::vector<unsigned char> region(size_t(W) * H, 0);
    std::vector<std::pair<int, int>> stack;
    bool seedOutside = false;

    for (size_t i = 0; i + 4 < c.seeds.size(); i += 5) {
        const int sx = int(std::floor(frame.bufferX(c.seeds[i])));
        const int sy = int(std::floor(frame.bufferY(c.seeds[i + 1])));

        if (sx >= 0 && sy >= 0 && sx < W && sy < H) {
            stack.emplace_back(sx, sy);
        } else {
            seedOutside = true;
        }
    }

    const bool fullFrame = frame.offsetX == 0 && frame.offsetY == 0 && W == frame.imageWidth && H == frame.imageHeight;

    if (!fullFrame) {
        // Connectivity through the part of the image outside of this buffer.
        array2D<float> refRegion;
        bool haveRef = false;

        if (reference && reference->L && reference->W > 0 && reference->H > 0) {
            refRegion(reference->W, reference->H);
            magicWand(c, reference->frame, reference->L, reference->a, reference->b, reference->W, reference->H, refRegion, multiThread, nullptr);
            haveRef = true;
        }

        auto borderSeed = [&](int x, int y) {
            if (out[y][x] <= 0.f) {
                return;
            }

            if (haveRef) {
                const int rx = int(std::floor(reference->frame.bufferX(frame.normX(x + 0.5))));
                const int ry = int(std::floor(reference->frame.bufferY(frame.normY(y + 0.5))));

                if (rx < 0 || ry < 0 || rx >= reference->W || ry >= reference->H || refRegion[ry][rx] <= 0.f) {
                    return;
                }
            } else if (!seedOutside) {
                return;
            }

            stack.emplace_back(x, y);
        };

        for (int x = 0; x < W; ++x) {
            borderSeed(x, 0);
            borderSeed(x, H - 1);
        }

        for (int y = 1; y < H - 1; ++y) {
            borderSeed(0, y);
            borderSeed(W - 1, y);
        }
    }

    floodFill(out, region, stack);

#ifdef _OPENMP
    #pragma omp parallel for if (multiThread)
#endif
    for (int y = 0; y < H; ++y) {
        float* row = out[y];
        const unsigned char* rrow = &region[size_t(y) * W];

        for (int x = 0; x < W; ++x) {
            if (!rrow[x]) {
                row[x] = 0.f;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Morphology

namespace
{

/// Running max (or min) over a window of half width k, truncated at the ends (van Herk / Gil-Werman).
void lineFilter(const float* in, float* out, int n, int k, bool useMax, std::vector<float>& g, std::vector<float>& h)
{
    if (k <= 0 || n <= 0) {
        std::copy(in, in + n, out);
        return;
    }

    const int p = 2 * k + 1;
    g.resize(n);
    h.resize(n);

    auto better = [useMax](float u, float v) {
        return useMax ? std::max(u, v) : std::min(u, v);
    };

    for (int start = 0; start < n; start += p) {
        const int end = std::min(start + p, n);
        g[start] = in[start];

        for (int i = start + 1; i < end; ++i) {
            g[i] = better(g[i - 1], in[i]);
        }

        h[end - 1] = in[end - 1];

        for (int i = end - 2; i >= start; --i) {
            h[i] = better(h[i + 1], in[i]);
        }
    }

    for (int i = 0; i < n; ++i) {
        const int j = std::min(i + k, n - 1);

        if (i < k) {
            out[i] = g[j];   // [0, j] lies in the first block
        } else if ((i - k) / p == j / p) {
            // window truncated at the end of the line, inside the last block:
            // g[j] would also cover the block's values before i - k
            out[i] = h[i - k];
        } else {
            out[i] = better(h[i - k], g[j]);
        }
    }
}

enum class LineDir { HORIZONTAL, VERTICAL, DIAG_DOWN, DIAG_UP };

/// Applies the 1D filter along every line of the given direction.
void directionalPass(array2D<float>& m, LineDir dir, int k, bool useMax, bool multiThread)
{
    const int W = m.getWidth();
    const int H = m.getHeight();

    if (k <= 0) {
        return;
    }

    int lines;

    switch (dir) {
        case LineDir::HORIZONTAL:
            lines = H;
            break;

        case LineDir::VERTICAL:
            lines = W;
            break;

        default:
            lines = W + H - 1;
            break;
    }

#ifdef _OPENMP
    #pragma omp parallel if (multiThread)
#endif
    {
        std::vector<float> in, out, g, h;

#ifdef _OPENMP
        #pragma omp for schedule(dynamic, 8)
#endif
        for (int l = 0; l < lines; ++l) {
            // gather
            int x0, y0, dx, dy, n;

            switch (dir) {
                case LineDir::HORIZONTAL:
                    x0 = 0;
                    y0 = l;
                    dx = 1;
                    dy = 0;
                    n = W;
                    break;

                case LineDir::VERTICAL:
                    x0 = l;
                    y0 = 0;
                    dx = 0;
                    dy = 1;
                    n = H;
                    break;

                case LineDir::DIAG_DOWN:   // direction (1, 1), lines indexed by (x - y)
                    x0 = std::max(l - (H - 1), 0);
                    y0 = std::max((H - 1) - l, 0);
                    dx = 1;
                    dy = 1;
                    n = std::min(W - x0, H - y0);
                    break;

                default:   // DIAG_UP: direction (1, -1), lines indexed by (x + y)
                    x0 = std::max(l - (H - 1), 0);
                    y0 = std::min(l, H - 1);
                    dx = 1;
                    dy = -1;
                    n = std::min(W - x0, y0 + 1);
                    break;
            }

            if (n <= 1) {
                continue;
            }

            in.resize(n);
            out.resize(n);

            for (int i = 0; i < n; ++i) {
                in[i] = m[y0 + i * dy][x0 + i * dx];
            }

            lineFilter(in.data(), out.data(), n, k, useMax, g, h);

            for (int i = 0; i < n; ++i) {
                m[y0 + i * dy][x0 + i * dx] = out[i];
            }
        }
    }
}

}

namespace
{

/// Largest half width handled by directPass (beyond, the O(1) van Herk passes win).
constexpr int DIRECT_PASS_MAX = 12;

/**
 * Same result as directionalPass (max / min over the window of half width k
 * along direction (dx, dy), truncated at the buffer border), computed
 * directly from 'in' into 'out' row by row: cache friendly and vectorizable,
 * faster than the gather / scatter of directionalPass for small k.
 */
void directPass(const array2D<float>& in, array2D<float>& out, int dx, int dy, int k, bool useMax, bool multiThread)
{
    const int W = in.getWidth();
    const int H = in.getHeight();

#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic, 16) if (multiThread)
#endif
    for (int y = 0; y < H; ++y) {
        float* o = out[y];
        std::copy(in[y], in[y] + W, o);

        for (int t = -k; t <= k; ++t) {
            if (t == 0) {
                continue;
            }

            const int yy = y + t * dy;

            if (yy < 0 || yy >= H) {
                continue;
            }

            const int sx = t * dx;
            const int x0 = std::max(0, -sx);
            const int x1 = std::min(W, W - sx);
            const float* r = in[yy] + sx;

            if (useMax) {
                for (int x = x0; x < x1; ++x) {
                    o[x] = std::max(o[x], r[x]);
                }
            } else {
                for (int x = x0; x < x1; ++x) {
                    o[x] = std::min(o[x], r[x]);
                }
            }
        }
    }
}

}

void morphology(array2D<float>& m, int radiusPx, bool dilate, bool multiThread)
{
    if (radiusPx <= 0) {
        return;
    }

    // Regular octagon of inradius r = Minkowski sum of a square of half
    // width a and a diamond of half width b (a + b = r, 2a + b = r * sqrt 2);
    // the diamond itself is the sum of two diagonal segments of b / 2 steps.
    const int a = std::max(int(std::lround(radiusPx * (M_SQRT2 - 1.0))), 1);
    const int bHalf = int(std::lround(radiusPx * (2.0 - M_SQRT2) * 0.5));

    array2D<float> tmp;
    array2D<float>* src = &m;
    array2D<float>* dst = &tmp;

    auto pass = [&](LineDir dir, int dx, int dy, int k) {
        if (k <= 0) {
            return;
        }

        if (k <= DIRECT_PASS_MAX) {
            if (!tmp.getWidth()) {
                tmp(m.getWidth(), m.getHeight());
            }

            directPass(*src, *dst, dx, dy, k, dilate, multiThread);
            std::swap(src, dst);
        } else {
            directionalPass(*src, dir, k, dilate, multiThread);
        }
    };

    pass(LineDir::HORIZONTAL, 1, 0, a);
    pass(LineDir::VERTICAL, 0, 1, a);

    if (bHalf > 0) {
        pass(LineDir::DIAG_DOWN, 1, 1, bHalf);
        pass(LineDir::DIAG_UP, 1, -1, bHalf);
    }

    if (src != &m) {
        const int W = m.getWidth();
        const int H = m.getHeight();

#ifdef _OPENMP
        #pragma omp parallel for if (multiThread)
#endif
        for (int y = 0; y < H; ++y) {
            std::copy((*src)[y], (*src)[y] + W, m[y]);
        }
    }
}

void smoothMask(array2D<float>& m, int radiusPx, bool multiThread)
{
    if (radiusPx <= 0) {
        return;
    }

    // opening (removes specks, rounds convex corners) then closing (fills holes, rounds concave corners)
    morphology(m, radiusPx, false, multiThread);
    morphology(m, radiusPx, true, multiThread);
    morphology(m, radiusPx, true, multiThread);
    morphology(m, radiusPx, false, multiThread);
}

int expandRadiusPixels(double expand, const MaskFrame& frame)
{
    return int(std::lround(std::min(std::fabs(expand), 100.0) / 100.0 * 0.01 * frame.diagonal()));
}

int smoothRadiusPixels(double smooth, const MaskFrame& frame)
{
    return int(std::lround(clamp01(float(smooth / 100.0)) * 0.005 * frame.diagonal()));
}

void invert(array2D<float>& m, bool multiThread)
{
    const int W = m.getWidth();
    const int H = m.getHeight();

#ifdef _OPENMP
    #pragma omp parallel for if (multiThread)
#endif
    for (int y = 0; y < H; ++y) {
        float* row = m[y];

        for (int x = 0; x < W; ++x) {
            row[x] = 1.f - clamp01(row[x]);
        }
    }
}

void scale(array2D<float>& m, float factor, bool multiThread)
{
    const int W = m.getWidth();
    const int H = m.getHeight();

#ifdef _OPENMP
    #pragma omp parallel for if (multiThread)
#endif
    for (int y = 0; y < H; ++y) {
        float* row = m[y];

        for (int x = 0; x < W; ++x) {
            row[x] = clamp01(row[x] * factor);
        }
    }
}

void combine(procparams::MaskBooleanOp op, array2D<float>& dst, const array2D<float>& src, bool multiThread)
{
    const int W = std::min(dst.getWidth(), src.getWidth());
    const int H = std::min(dst.getHeight(), src.getHeight());

#ifdef _OPENMP
    #pragma omp parallel for if (multiThread)
#endif
    for (int y = 0; y < H; ++y) {
        float* d = dst[y];
        const float* s = src[y];

        for (int x = 0; x < W; ++x) {
            d[x] = clamp01(combineValue(op, d[x], s[x]));
        }
    }
}

bool isEmpty(const array2D<float>& m)
{
    const int W = m.getWidth();
    const int H = m.getHeight();

    for (int y = 0; y < H; ++y) {
        const float* row = m[y];

        for (int x = 0; x < W; ++x) {
            if (row[x] > 0.f) {
                return false;
            }
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Quick selection
//
// Modelled on the Quick Selection brush of photocraft / Photoshop (a stroke is
// one set of seeds, the selection grows from the brushed footprint to image
// edges), adapted to procedural masks: instead of one min cut on the pixels of
// the document, a geodesic distance is computed on a fixed working grid laid
// over the normalized image (so every pipeline sees the same grid), with a
// step cost made of image gradients and color distance to the seeds, and
// thresholded softly. The grid result is brought to the buffer with a joint
// bilateral upsampling guided by the buffer's own colors so that the edge
// follows the image at any resolution.

namespace
{

constexpr int QS_GRID = 640;          // cells along the long side of the image
constexpr float QS_SPATIAL = 0.015f;  // cost of one cell step in a flat area
constexpr float QS_NOISE = 0.5f;      // minimum dE per cell ignored by the edge term (noise floor)
constexpr float QS_COLOR_W = 0.04f;   // cost per cell per dE away from the seed colors
constexpr float QS_COLOR_DZ = 4.f;    // dE from the nearest seed color that costs nothing
constexpr float QS_INF = 1e30f;

struct QSGrid {
    int W = 0;
    int H = 0;
    std::vector<float> L, a, b;      // cell averages, L*a*b* standard units
    std::vector<unsigned char> valid;
};

void qsGridSize(const MaskFrame& f, int& gw, int& gh)
{
    const double iw = std::max(f.imageWidth, 1);
    const double ih = std::max(f.imageHeight, 1);

    if (iw >= ih) {
        gw = QS_GRID;
        gh = std::max(1, int(std::lround(QS_GRID * ih / iw)));
    } else {
        gh = QS_GRID;
        gw = std::max(1, int(std::lround(QS_GRID * iw / ih)));
    }
}

/// Fill the working grid (gw x gh cells over the whole image) from Lab planes mapped by 'f'.
void qsFillGrid(QSGrid& g, const MaskFrame& f, const float* const* L, const float* const* A, const float* const* B, int W, int H, bool multiThread)
{
    const int gw = g.W;
    const int gh = g.H;
    const size_t n = size_t(gw) * gh;
    g.L.assign(n, 0.f);
    g.a.assign(n, 0.f);
    g.b.assign(n, 0.f);
    g.valid.assign(n, 0);

    // Pixel ranges (pixel centers inside the cell) per grid column / row.
    std::vector<int> cx0(gw), cx1(gw), cy0(gh), cy1(gh);
    std::vector<double> ccx(gw), ccy(gh);

    for (int i = 0; i < gw; ++i) {
        const double b0 = f.bufferX(double(i) / gw);
        const double b1 = f.bufferX(double(i + 1) / gw);
        cx0[i] = std::max(int(std::ceil(b0 - 0.5)), 0);
        cx1[i] = std::min(int(std::ceil(b1 - 0.5)) - 1, W - 1);
        ccx[i] = f.bufferX((i + 0.5) / gw);
    }

    for (int j = 0; j < gh; ++j) {
        const double b0 = f.bufferY(double(j) / gh);
        const double b1 = f.bufferY(double(j + 1) / gh);
        cy0[j] = std::max(int(std::ceil(b0 - 0.5)), 0);
        cy1[j] = std::min(int(std::ceil(b1 - 0.5)) - 1, H - 1);
        ccy[j] = f.bufferY((j + 0.5) / gh);
    }

    auto bilinear = [&](const float* const* P, double x, double y) {
        x = std::min(std::max(x - 0.5, 0.0), double(W - 1));
        y = std::min(std::max(y - 0.5, 0.0), double(H - 1));
        const int x0 = int(x);
        const int y0 = int(y);
        const int x1 = std::min(x0 + 1, W - 1);
        const int y1 = std::min(y0 + 1, H - 1);
        const float fx = float(x - x0);
        const float fy = float(y - y0);
        return (P[y0][x0] * (1.f - fx) + P[y0][x1] * fx) * (1.f - fy) + (P[y1][x0] * (1.f - fx) + P[y1][x1] * fx) * fy;
    };

#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic, 8) if (multiThread)
#endif
    for (int j = 0; j < gh; ++j) {
        for (int i = 0; i < gw; ++i) {
            const size_t k = size_t(j) * gw + i;
            float sl = 0.f, sa = 0.f, sb = 0.f;
            int cnt = 0;

            if (cx0[i] <= cx1[i] && cy0[j] <= cy1[j]) {
                for (int y = cy0[j]; y <= cy1[j]; ++y) {
                    for (int x = cx0[i]; x <= cx1[i]; ++x) {
                        sl += L[y][x];
                        sa += A[y][x];
                        sb += B[y][x];
                    }
                }

                cnt = (cy1[j] - cy0[j] + 1) * (cx1[i] - cx0[i] + 1);
            } else if (ccx[i] >= 0.0 && ccy[j] >= 0.0 && ccx[i] < W && ccy[j] < H) {
                // Buffer coarser than the grid: interpolate at the cell center.
                sl = bilinear(L, ccx[i], ccy[j]);
                sa = bilinear(A, ccx[i], ccy[j]);
                sb = bilinear(B, ccx[i], ccy[j]);
                cnt = 1;
            }

            if (cnt > 0) {
                g.L[k] = sl / (cnt * LAB_L_SCALE);
                g.a[k] = sa / (cnt * LAB_AB_SCALE);
                g.b[k] = sb / (cnt * LAB_AB_SCALE);
                g.valid[k] = 1;
            }
        }
    }
}

/// Label the stroke footprints on the grid: 1 = add seed, 2 = erase seed (later strokes win).
void qsSeeds(const procparams::MaskComponent& c, int gw, int gh, std::vector<unsigned char>& label)
{
    label.assign(size_t(gw) * gh, 0);
    const double gdiag = std::sqrt(double(gw) * gw + double(gh) * gh);

    for (const procparams::MaskBrushStroke& s : c.strokes) {
        const size_t n = s.pointCount();

        if (n == 0) {
            continue;
        }

        const unsigned char v = s.erase ? 2 : 1;
        const double r = std::max(s.size * gdiag, 0.5);
        const double r2 = r * r;

        for (size_t p = 0; p < std::max<size_t>(n - 1, 1); ++p) {
            Segment seg;
            seg.ax = s.points[2 * p] * gw;
            seg.ay = s.points[2 * p + 1] * gh;
            seg.bx = n == 1 ? seg.ax : s.points[2 * p + 2] * gw;
            seg.by = n == 1 ? seg.ay : s.points[2 * p + 3] * gh;
            const int x0 = std::max(int(std::floor(std::min(seg.ax, seg.bx) - r)), 0);
            const int x1 = std::min(int(std::ceil(std::max(seg.ax, seg.bx) + r)), gw - 1);
            const int y0 = std::max(int(std::floor(std::min(seg.ay, seg.by) - r)), 0);
            const int y1 = std::min(int(std::ceil(std::max(seg.ay, seg.by) + r)), gh - 1);

            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    if (segmentDistanceSquared(x + 0.5, y + 0.5, seg) <= r2) {
                        label[size_t(y) * gw + x] = v;
                    }
                }
            }
        }

        // The cells under the points themselves, even for a brush smaller than a cell.
        for (size_t p = 0; p < n; ++p) {
            const int x = int(std::floor(s.points[2 * p] * gw));
            const int y = int(std::floor(s.points[2 * p + 1] * gh));

            if (x >= 0 && y >= 0 && x < gw && y < gh) {
                label[size_t(y) * gw + x] = v;
            }
        }
    }
}

inline float qsColorDist(float l1, float a1, float b1, float l2, float a2, float b2)
{
    const float dl = (l1 - l2) * 0.5f;
    const float da = a1 - a2;
    const float db = b1 - b2;
    return std::sqrt(dl * dl + da * da + db * db);
}

/**
 * Noise floor of the edge term: a multiple of the median color difference
 * between horizontally adjacent cells (dominated by the noise of flat areas),
 * so that noisy and clean renderings of the same image grow alike.
 */
float qsNoiseFloor(const QSGrid& g)
{
    std::vector<float> d;
    d.reserve(size_t(g.W) * g.H / 2 + 1);

    for (int j = 0; j < g.H; j += 2) {
        for (int i = 0; i + 1 < g.W; ++i) {
            const size_t k = size_t(j) * g.W + i;

            if (g.valid[k] && g.valid[k + 1]) {
                const float dl = g.L[k + 1] - g.L[k];
                const float da = g.a[k + 1] - g.a[k];
                const float db = g.b[k + 1] - g.b[k];
                d.push_back(std::sqrt(dl * dl + da * da + db * db));
            }
        }
    }

    if (d.empty()) {
        return QS_NOISE;
    }

    std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
    return std::max(QS_NOISE, std::min(1.5f * d[d.size() / 2], 6.f));
}

/**
 * Geodesic distance on the grid from the cells labelled 'which', up to 'cap'
 * (Dijkstra with a binary heap, early exit). Cells beyond stay QS_INF.
 */
void qsGeodesic(const QSGrid& g, const std::vector<unsigned char>& label, unsigned char which, float cap, float edgeW, float noise, std::vector<float>& D)
{
    const int gw = g.W;
    const int gh = g.H;
    const size_t n = size_t(gw) * gh;
    D.assign(n, QS_INF);

    // Representative seed colors (greedy, at most 12), and per cell distance to them.
    std::vector<std::array<float, 3>> reps;
    size_t seedCount = 0;

    for (size_t k = 0; k < n; ++k) {
        if (label[k] == which && g.valid[k]) {
            ++seedCount;
        }
    }

    if (seedCount == 0) {
        return;
    }

    const size_t stride = std::max<size_t>(seedCount / 4096, 1);
    size_t seen = 0;

    for (size_t k = 0; k < n && reps.size() < 12; ++k) {
        if (label[k] != which || !g.valid[k] || (seen++ % stride)) {
            continue;
        }

        float best = QS_INF;

        for (const auto& r : reps) {
            best = std::min(best, qsColorDist(r[0], r[1], r[2], g.L[k], g.a[k], g.b[k]));
        }

        if (best > 6.f) {
            reps.push_back({g.L[k], g.a[k], g.b[k]});
        }
    }

    using Item = std::pair<float, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> heap;

    for (size_t k = 0; k < n; ++k) {
        if (label[k] == which && g.valid[k]) {
            D[k] = 0.f;
            heap.emplace(0.f, int(k));
        }
    }

    // Per cell color term, computed lazily (most cells are never reached).
    std::vector<float> colorCost(n, -1.f);
    auto cellColorCost = [&](size_t k) {
        float& cc = colorCost[k];

        if (cc < 0.f) {
            float best = QS_INF;

            for (const auto& r : reps) {
                best = std::min(best, qsColorDist(r[0], r[1], r[2], g.L[k], g.a[k], g.b[k]));
            }

            cc = QS_SPATIAL + QS_COLOR_W * std::max(best - QS_COLOR_DZ, 0.f);
        }

        return cc;
    };

    static const int ndx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static const int ndy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    static const float nlen[8] = {1.f, 1.f, 1.f, 1.f, 1.41421356f, 1.41421356f, 1.41421356f, 1.41421356f};

    while (!heap.empty()) {
        const Item it = heap.top();
        heap.pop();
        const int k = it.second;

        if (it.first > D[k]) {
            continue;   // stale entry
        }

        const int x = k % gw;
        const int y = k / gw;

        for (int d = 0; d < 8; ++d) {
            const int nx = x + ndx[d];
            const int ny = y + ndy[d];

            if (nx < 0 || ny < 0 || nx >= gw || ny >= gh) {
                continue;
            }

            const int q = ny * gw + nx;

            if (!g.valid[q]) {
                continue;
            }

            const float l = nlen[d];
            const float dl = g.L[q] - g.L[k];
            const float da = g.a[q] - g.a[k];
            const float db = g.b[q] - g.b[k];
            const float grad = std::sqrt(dl * dl + da * da + db * db);
            const float step = l * cellColorCost(q) + edgeW * std::max(grad - noise * l, 0.f);
            const float nd = it.first + step;

            if (nd < D[q] && nd <= cap) {
                D[q] = nd;
                heap.emplace(nd, q);
            }
        }
    }
}

/// Joint bilateral upsampling of grid values to the buffer, guided by the buffer's colors.
void qsUpsample(const QSGrid& g, const std::vector<float>& m, const MaskFrame& frame, const float* const* L, const float* const* A, const float* const* B, int W, int H, array2D<float>& out, bool multiThread)
{
    const int gw = g.W;
    const int gh = g.H;

    // Cells whose 3x3 neighbourhood is uniform need no filtering (1), nearly
    // uniform ones (soft interior of the falloff) are interpolated bilinearly (2).
    std::vector<signed char> uniform(size_t(gw) * gh, 0);

#ifdef _OPENMP
    #pragma omp parallel for if (multiThread)
#endif
    for (int j = 0; j < gh; ++j) {
        for (int i = 0; i < gw; ++i) {
            const float v = m[size_t(j) * gw + i];
            float mn = v, mx = v;

            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int x = i + dx;
                    const int y = j + dy;

                    if (x >= 0 && y >= 0 && x < gw && y < gh && g.valid[size_t(y) * gw + x]) {
                        mn = std::min(mn, m[size_t(y) * gw + x]);
                        mx = std::max(mx, m[size_t(y) * gw + x]);
                    }
                }
            }

            uniform[size_t(j) * gw + i] = mx == mn ? 1 : (mx - mn < 0.05f ? 2 : 0);
        }
    }

    constexpr float invTwoSigmaS2 = 1.f / (2.f * 0.6f * 0.6f);
    constexpr float invTwoSigmaR2 = 1.f / (2.f * 7.f * 7.f);

#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic, 16) if (multiThread)
#endif
    for (int y = 0; y < H; ++y) {
        float* row = out[y];
        const float gy = float(frame.normY(y + 0.5) * gh - 0.5);
        const int cj = std::min(std::max(int(std::lround(gy)), 0), gh - 1);

        for (int x = 0; x < W; ++x) {
            const float gx = float(frame.normX(x + 0.5) * gw - 0.5);
            const int ci = std::min(std::max(int(std::lround(gx)), 0), gw - 1);
            const size_t ck = size_t(cj) * gw + ci;

            if (uniform[ck] == 1) {
                row[x] = m[ck];
                continue;
            }

            if (uniform[ck] == 2) {
                const int i0 = std::min(std::max(int(std::floor(gx)), 0), gw - 1);
                const int j0 = std::min(std::max(int(std::floor(gy)), 0), gh - 1);
                const int i1 = std::min(i0 + 1, gw - 1);
                const int j1 = std::min(j0 + 1, gh - 1);
                const float fx = std::min(std::max(gx - i0, 0.f), 1.f);
                const float fy = std::min(std::max(gy - j0, 0.f), 1.f);
                const float* m0 = &m[size_t(j0) * gw];
                const float* m1 = &m[size_t(j1) * gw];
                row[x] = (m0[i0] * (1.f - fx) + m0[i1] * fx) * (1.f - fy) + (m1[i0] * (1.f - fx) + m1[i1] * fx) * fy;
                continue;
            }

            const float pl = L[y][x] / LAB_L_SCALE;
            const float pa = A[y][x] / LAB_AB_SCALE;
            const float pb = B[y][x] / LAB_AB_SCALE;
            float sw = 0.f, sv = 0.f, bw = 0.f, bv = 0.f;

            for (int dy = -1; dy <= 1; ++dy) {
                const int j = cj + dy;

                if (j < 0 || j >= gh) {
                    continue;
                }

                for (int dx = -1; dx <= 1; ++dx) {
                    const int i = ci + dx;

                    if (i < 0 || i >= gw) {
                        continue;
                    }

                    const size_t k = size_t(j) * gw + i;

                    if (!g.valid[k]) {
                        continue;
                    }

                    const float sx = i - gx;
                    const float sy = j - gy;
                    const float ws = std::exp(-(sx * sx + sy * sy) * invTwoSigmaS2);
                    const float dl = pl - g.L[k];
                    const float da = pa - g.a[k];
                    const float db = pb - g.b[k];
                    const float wr = std::exp(-(dl * dl + da * da + db * db) * invTwoSigmaR2);
                    sw += ws * wr;
                    sv += ws * wr * m[k];
                    bw += ws;
                    bv += ws * m[k];
                }
            }

            row[x] = sw > 1e-4f ? clamp01(sv / sw) : (bw > 0.f ? clamp01(bv / bw) : m[ck]);
        }
    }
}

}

void quickSelectGridSize(const MaskFrame& frame, int& gw, int& gh)
{
    qsGridSize(frame, gw, gh);
}

void quickSelect(const procparams::MaskComponent& c, const MaskFrame& frame, const float* const* L, const float* const* a, const float* const* b, int W, int H, array2D<float>& out, bool multiThread, const LabPlanes* reference, array2D<float>* eraseOut)
{
    out.fill(0.f, multiThread);

    if (eraseOut) {
        eraseOut->fill(0.f, multiThread);
    }

    if (c.strokes.empty() || W <= 0 || H <= 0) {
        return;
    }

    const bool fullFrame = frame.offsetX == 0 && frame.offsetY == 0 && W == frame.imageWidth && H == frame.imageHeight;
    const bool useRef = !fullFrame && reference && reference->L && reference->W > 0 && reference->H > 0;
    const MaskFrame& srcFrame = useRef ? reference->frame : frame;

    QSGrid g;
    qsGridSize(srcFrame, g.W, g.H);

    if (useRef) {
        qsFillGrid(g, reference->frame, reference->L, reference->a, reference->b, reference->W, reference->H, multiThread);
    } else {
        qsFillGrid(g, frame, L, a, b, W, H, multiThread);
    }

    std::vector<unsigned char> label;
    qsSeeds(c, g.W, g.H, label);

    bool haveFg = false;
    bool haveBg = false;

    for (size_t k = 0; k < label.size(); ++k) {
        if (g.valid[k]) {
            haveFg = haveFg || label[k] == 1;
            haveBg = haveBg || label[k] == 2;
        }
    }

    if (!haveFg && !haveBg) {
        return;
    }

    // Parameters: tolerance -> geodesic threshold (dE-like units), smoothness -> soft band,
    // edge sensitivity -> weight of the image gradient in the step cost.
    const float T = 2.f + 0.5f * float(std::min(std::max(c.tolerance, 0.0), 100.0));
    const float soft = 0.05f + 0.9f * clamp01(float(c.smoothness / 100.0));
    const float edgeW = 0.3f + 1.7f * clamp01(float(c.edgeSensitivity / 100.0));
    const float lo = T * (1.f - soft);
    const float k = std::max(1.f, 0.5f * T * soft);   // extra reach of the erase seeds beyond the threshold

    const float noise = qsNoiseFloor(g);
    std::vector<float> Df, Db;

#ifdef _OPENMP
    #pragma omp parallel sections if (multiThread && haveFg && haveBg)
#endif
    {
#ifdef _OPENMP
        #pragma omp section
#endif
        {
            if (haveFg) {
                qsGeodesic(g, label, 1, T + k, edgeW, noise, Df);
            }
        }
#ifdef _OPENMP
        #pragma omp section
#endif
        {
            if (haveBg) {
                qsGeodesic(g, label, 2, T + k, edgeW, noise, Db);
            }
        }
    }

    const size_t n = label.size();
    std::vector<float> fg(n, 0.f);
    std::vector<float> bg;

    if (eraseOut && haveBg) {
        bg.assign(n, 0.f);
    }

    for (size_t i = 0; i < n; ++i) {
        if (!g.valid[i]) {
            continue;
        }

        const float df = haveFg ? Df[i] : QS_INF;
        const float db = haveBg ? Db[i] : QS_INF;
        const float sf = df >= QS_INF ? 0.f : 1.f - smoothstep(lo, T, df);
        const float sb = db >= QS_INF ? 0.f : 1.f - smoothstep(lo, T, db);
        // Competition: a cell belongs to the seeds it is geodesically closer to.
        float toFg = 1.f;

        if (db < QS_INF && df < QS_INF) {
            // relative difference: crisp in flat areas (small distances) and across edges alike
            toFg = smoothstep(-0.1f, 0.1f, (db - df) / (db + df + 1e-3f));
        } else if (db < QS_INF) {
            toFg = 0.f;
        }

        fg[i] = sf * toFg;

        if (!bg.empty()) {
            bg[i] = sb * (1.f - toFg);
        }

        if (label[i] == 1) {
            fg[i] = 1.f;

            if (!bg.empty()) {
                bg[i] = 0.f;
            }
        } else if (label[i] == 2) {
            fg[i] = 0.f;

            if (!bg.empty()) {
                bg[i] = 1.f;
            }
        }
    }

    if (haveFg) {
        qsUpsample(g, fg, frame, L, a, b, W, H, out, multiThread);
    }

    if (!bg.empty()) {
        qsUpsample(g, bg, frame, L, a, b, W, H, *eraseOut, multiThread);
    }
}

}

}
