/* -*- C++ -*-
 *
 *  This file is part of RawTherapee.
 *
 *  Standalone test for the Masks tool math (maskgeometry.cc, maskparams.cc,
 *  aisegmentation.cc). It has no GTK/Glib dependency and is not part of the
 *  regular build. Compile and run from the repository root with:
 *
 *    g++ -std=c++17 -fopenmp -O2 \
 *        rtengine/tests/maskgeometry_test.cc rtengine/maskgeometry.cc \
 *        rtengine/maskparams.cc rtengine/aisegmentation.cc -o /tmp/maskgeometry_test \
 *    && /tmp/maskgeometry_test
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

#include "../aisegmentation.h"
#include "../maskgeometry.h"
#include "../maskparams.h"

using namespace rtengine;
using namespace rtengine::procparams;

namespace
{

int failures = 0;

void check(bool cond, const char* what)
{
    if (!cond) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

bool near(float a, float b, float eps = 1e-3f)
{
    return std::fabs(a - b) <= eps;
}

void testSmoothstep()
{
    check(near(maskgeom::smoothstep(0.f, 1.f, -1.f), 0.f), "smoothstep below");
    check(near(maskgeom::smoothstep(0.f, 1.f, 0.5f), 0.5f), "smoothstep middle");
    check(near(maskgeom::smoothstep(0.f, 1.f, 2.f), 1.f), "smoothstep above");
    check(near(maskgeom::smoothstep(3.f, 3.f, 2.9f), 0.f), "degenerate step below");
    check(near(maskgeom::smoothstep(3.f, 3.f, 3.f), 1.f), "degenerate step at");
}

void testLinearGradient()
{
    MaskComponent c(MaskComponentType::LINEAR_GRADIENT);
    c.x0 = 0.2;
    c.y0 = 0.5;
    c.x1 = 0.8;
    c.y1 = 0.5;

    const int W = 100, H = 50;
    MaskFrame frame(W, H, 0, 0);
    array2D<float> m(W, H);
    maskgeom::linearGradient(c, frame, m, false);

    check(near(m[25][10], 1.f), "linear: fully selected before start");
    check(near(m[25][90], 0.f), "linear: zero after end");
    check(near(m[25][50], 0.5f, 0.03f), "linear: half way is 0.5");
    check(near(m[5][50], m[45][50]), "linear: constant perpendicular to the axis");
    check(m[25][30] > m[25][40] && m[25][40] > m[25][60], "linear: monotonic");
}

void testRadialGradient()
{
    MaskComponent c(MaskComponentType::RADIAL_GRADIENT);
    c.centerX = 0.5;
    c.centerY = 0.5;
    c.radiusX = 0.2;
    c.radiusY = 0.1;
    c.angle = 0.0;
    c.feather = 50.0;

    const int W = 200, H = 100;
    MaskFrame frame(W, H, 0, 0);
    array2D<float> m(W, H);
    maskgeom::radialGradient(c, frame, m, false);

    const double diag = frame.diagonal();
    const int rx = int(0.2 * diag);
    const int ry = int(0.1 * diag);

    check(near(m[50][100], 1.f), "radial: center is 1");
    check(near(m[50][std::min(100 + rx + 2, W - 1)], 0.f), "radial: outside along x is 0");
    check(near(m[std::min(50 + ry + 2, H - 1)][100], 0.f), "radial: outside along y is 0");
    check(near(m[50][100 + rx / 4], 1.f), "radial: inside the core is 1");
    check(m[50][100 + rx / 2] > m[50][100 + (3 * rx) / 4], "radial: falloff is monotonic");

    // 90 degree rotation swaps the axes
    c.angle = 90.0;
    maskgeom::radialGradient(c, frame, m, false);
    check(near(m[50][100 + ry / 4], 1.f) && near(m[50][std::min(100 + rx, W - 1)], 0.f), "radial: rotation swaps axes");
}

void testBrush()
{
    const int W = 200, H = 100;
    MaskFrame frame(W, H, 0, 0);
    const double diag = frame.diagonal();

    MaskComponent c(MaskComponentType::BRUSH);
    MaskBrushStroke s;
    s.size = 0.05;      // radius = 0.05 * diag
    s.feather = 0.5;
    s.flow = 1.0;
    s.points = {0.25, 0.5};   // single dab
    c.strokes.push_back(s);

    array2D<float> m(W, H);
    maskgeom::brush(c, frame, m, false);

    const int cx = int(0.25 * W), cy = int(0.5 * H);
    const int r = int(0.05 * diag);
    check(near(m[cy][cx], 1.f), "brush: center of a dab is 1");
    check(near(m[cy][cx + r / 4], 1.f), "brush: hard core is 1");
    check(m[cy][cx + (3 * r) / 4] > 0.f && m[cy][cx + (3 * r) / 4] < 1.f, "brush: feather zone is partial");
    check(near(m[cy][cx + r + 2], 0.f), "brush: outside the radius is 0");
    check(near(m[cy][W - 5], 0.f), "brush: far away is 0");

    // A line stroke covers the capsule between its points
    MaskComponent line(MaskComponentType::BRUSH);
    MaskBrushStroke ls = s;
    ls.points = {0.2, 0.5, 0.6, 0.5};
    line.strokes.push_back(ls);
    maskgeom::brush(line, frame, m, false);
    check(near(m[cy][int(0.4 * W)], 1.f), "brush: middle of a segment is covered");
    check(near(m[cy][int(0.2 * W) - r - 2], 0.f), "brush: before the segment start is 0");

    // Flow accumulates: two 50% strokes give 75%
    MaskComponent flow(MaskComponentType::BRUSH);
    MaskBrushStroke fs = s;
    fs.flow = 0.5;
    flow.strokes.push_back(fs);
    flow.strokes.push_back(fs);
    maskgeom::brush(flow, frame, m, false);
    check(near(m[cy][cx], 0.75f), "brush: flow accumulates");

    // Erase removes coverage
    MaskComponent erase(MaskComponentType::BRUSH);
    erase.strokes.push_back(s);
    MaskBrushStroke es = s;
    es.erase = true;
    erase.strokes.push_back(es);
    maskgeom::brush(erase, frame, m, false);
    check(near(m[cy][cx], 0.f), "brush: erase removes coverage");

    // Brush on top of an existing selection: paint adds, erase removes
    array2D<float> base(W, H);
    base.fill(1.f);   // everything selected (e.g. by an AI component)
    MaskComponent edit(MaskComponentType::BRUSH);
    edit.strokes.push_back(es);   // the erase dab at (0.25, 0.5)
    maskgeom::brushApply(edit, frame, base, false, false);
    check(near(base[cy][cx], 0.f), "brushApply: erase removes from a prior selection");
    check(near(base[cy][W - 5], 1.f), "brushApply: untouched area keeps the prior selection");
    MaskComponent sub(MaskComponentType::BRUSH);
    sub.strokes.push_back(s);     // a paint dab, applied in subtract mode
    maskgeom::brushApply(sub, frame, base, true, false);
    check(near(base[cy][W - 5], 1.f), "brushApply: subtract mode leaves untouched area alone");
    base.fill(1.f);
    maskgeom::brushApply(sub, frame, base, true, false);
    check(near(base[cy][cx], 0.f), "brushApply: subtract mode makes paint strokes erase");

    // Empty component -> empty mask
    MaskComponent empty(MaskComponentType::BRUSH);
    maskgeom::brush(empty, frame, m, false);
    check(maskgeom::isEmpty(m), "brush: no strokes gives an empty mask");
}

void testFrameConsistency()
{
    // The same component must evaluate identically on the full preview and
    // on a detail crop covering a sub-rectangle of it.
    MaskComponent c(MaskComponentType::RADIAL_GRADIENT);
    c.centerX = 0.4;
    c.centerY = 0.6;
    c.radiusX = 0.15;
    c.radiusY = 0.15;
    c.feather = 80.0;

    const int W = 160, H = 120;
    MaskFrame full(W, H, 0, 0);
    array2D<float> fm(W, H);
    maskgeom::radialGradient(c, full, fm, false);

    const int ox = 30, oy = 40, cw = 70, ch = 50;
    MaskFrame crop(W, H, ox, oy);
    array2D<float> cm(cw, ch);
    maskgeom::radialGradient(c, crop, cm, false);

    bool same = true;

    for (int y = 0; y < ch && same; ++y) {
        for (int x = 0; x < cw; ++x) {
            if (!near(cm[y][x], fm[y + oy][x + ox], 1e-5f)) {
                same = false;
                break;
            }
        }
    }

    check(same, "frame: detail crop matches the full preview");

    MaskComponent b(MaskComponentType::BRUSH);
    MaskBrushStroke s;
    s.size = 0.08;
    s.points = {0.3, 0.7, 0.6, 0.4};
    b.strokes.push_back(s);
    maskgeom::brush(b, full, fm, false);
    maskgeom::brush(b, crop, cm, false);
    same = true;

    for (int y = 0; y < ch && same; ++y) {
        for (int x = 0; x < cw; ++x) {
            if (!near(cm[y][x], fm[y + oy][x + ox], 1e-5f)) {
                same = false;
                break;
            }
        }
    }

    check(same, "frame: brush on a detail crop matches the full preview");
}

void testRanges()
{
    MaskComponent lum(MaskComponentType::LUMINANCE_RANGE);
    lum.lumLow = 40;
    lum.lumHigh = 70;
    lum.lumFeatherLow = 10;
    lum.lumFeatherHigh = 10;
    check(near(maskgeom::luminanceRangeWeight(lum, 20.f), 0.f), "lum: below window");
    check(near(maskgeom::luminanceRangeWeight(lum, 55.f), 1.f), "lum: inside window");
    check(near(maskgeom::luminanceRangeWeight(lum, 90.f), 0.f), "lum: above window");
    check(near(maskgeom::luminanceRangeWeight(lum, 35.f), 0.5f), "lum: middle of the low feather");
    check(near(maskgeom::luminanceRangeWeight(lum, 75.f), 0.5f), "lum: middle of the high feather");

    MaskComponent col(MaskComponentType::COLOR_RANGE);
    col.sampleL = 50;
    col.sampleA = 40;
    col.sampleB = 30;
    col.tolerance = 50;
    col.smoothness = 50;
    check(near(maskgeom::colorRangeWeight(col, 50.f, 40.f, 30.f), 1.f), "color: exact sample is 1");
    check(near(maskgeom::colorRangeWeight(col, 50.f, -40.f, -30.f), 0.f), "color: opposite color is 0");
    check(maskgeom::colorRangeWeight(col, 60.f, 40.f, 30.f) > 0.9f, "color: lightness is weighted down");

    // The plane based versions must agree with the scalar ones.
    const int W = 4, H = 2;
    array2D<float> L(W, H), a(W, H), b(W, H), out(W, H);

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            L[y][x] = (10.f + 20.f * x) * 327.68f;
            a[y][x] = 40.f * 327.68f;
            b[y][x] = 30.f * 327.68f;
        }
    }

    maskgeom::luminanceRange(lum, L, W, H, out, false);
    check(near(out[0][2], maskgeom::luminanceRangeWeight(lum, 50.f)), "lum: plane version matches scalar");
    maskgeom::colorRange(col, L, a, b, W, H, out, false);
    check(near(out[1][2], maskgeom::colorRangeWeight(col, 50.f, 40.f, 30.f)), "color: plane version matches scalar");
}

void testCombine()
{
    check(near(maskgeom::combineValue(MaskBooleanOp::ADD, 0.5f, 0.5f), 0.75f), "combine: add is a union");
    check(near(maskgeom::combineValue(MaskBooleanOp::ADD, 0.f, 1.f), 1.f), "combine: add with 1");
    check(near(maskgeom::combineValue(MaskBooleanOp::SUBTRACT, 1.f, 0.25f), 0.75f), "combine: subtract");
    check(near(maskgeom::combineValue(MaskBooleanOp::INTERSECT, 0.5f, 0.5f), 0.25f), "combine: intersect");

    const int W = 3, H = 1;
    array2D<float> d(W, H), s(W, H);
    d[0][0] = 1.f;
    d[0][1] = 0.5f;
    d[0][2] = 0.f;
    s[0][0] = 0.5f;
    s[0][1] = 0.5f;
    s[0][2] = 0.5f;
    maskgeom::combine(MaskBooleanOp::INTERSECT, d, s, false);
    check(near(d[0][0], 0.5f) && near(d[0][1], 0.25f) && near(d[0][2], 0.f), "combine: array version");
    maskgeom::invert(d, false);
    check(near(d[0][2], 1.f), "invert");
    maskgeom::scale(d, 0.5f, false);
    check(near(d[0][2], 0.5f), "scale");
}

void testDummyProvider()
{
    const int W = 64, H = 48;
    array2D<float> L(W, H), a(W, H), b(W, H), out(W, H);

    // Upper half: bright blue "sky", lower half: dark warm "ground".
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const bool sky = y < H / 2;
            L[y][x] = (sky ? 75.f : 30.f) * 327.68f;
            a[y][x] = (sky ? -5.f : 15.f) * 327.68f;
            b[y][x] = (sky ? -30.f : 25.f) * 327.68f;
        }
    }

    auto& reg = AISegmentationRegistry::getInstance();
    check(reg.getProvider("") == nullptr, "registry: no default provider before anything is registered");
    check(reg.listProviders().empty() && reg.listProviderInfos().empty(), "registry: empty lists before anything is registered");

    // The heuristic provider is hidden: usable by id, never listed, never the default.
    reg.registerProvider(std::unique_ptr<AISegmentationProvider>(new DummySegmentationProvider()));
    check(reg.getProvider("") == nullptr, "registry: a hidden provider never becomes the default");
    check(reg.getProvider(DummySegmentationProvider::ID) != nullptr, "registry: hidden provider is reachable by id");
    check(reg.listProviders().empty() && reg.listProviderInfos().empty(), "registry: hidden provider is not listed");

    // An unavailable provider is listed with its reason but is never chosen as default.
    class Unavailable final : public AISegmentationProvider
    {
    public:
        std::string id() const override { return "unavailable"; }
        std::string displayName() const override { return "Unavailable model"; }
        bool supports(MaskComponentType) const override { return true; }
        bool isAvailable() const override { return false; }
        std::string availabilityReason() const override { return "gated weights"; }
        bool segment(const SegmentationRequest&, array2D<float>&) override { return false; }
    };
    reg.registerProvider(std::unique_ptr<AISegmentationProvider>(new Unavailable()));
    check(reg.getProvider("") == nullptr, "registry: an unavailable provider never becomes the default");
    check(reg.getProvider("unavailable") != nullptr && !reg.getProvider("unavailable")->isAvailable(), "registry: unavailable provider is reachable by id");
    check(reg.listProviders().empty(), "registry: unavailable provider is not selectable");
    check(reg.listProviderInfos().size() == 1 && !reg.listProviderInfos()[0].available && reg.listProviderInfos()[0].reason == "gated weights", "registry: unavailable provider is listed with its reason");
    reg.setDefaultProvider("unavailable");
    check(reg.getProvider("") == nullptr, "registry: setDefaultProvider ignores unavailable providers");

    // An available provider becomes the default, and unknown ids fall back to it.
    class Available final : public AISegmentationProvider
    {
    public:
        std::string id() const override { return "available"; }
        std::string displayName() const override { return "Available model"; }
        bool supports(MaskComponentType) const override { return true; }
        bool segment(const SegmentationRequest&, array2D<float>& out) override { out.fill(1.f, false); return true; }
    };
    reg.registerProvider(std::unique_ptr<AISegmentationProvider>(new Available()));
    check(reg.getProvider("") != nullptr && reg.getProvider("")->id() == "available", "registry: first available provider is the default");
    check(reg.getProvider("does-not-exist") == reg.getProvider(""), "registry: unknown id falls back to the default");
    check(reg.listProviders().size() == 1 && reg.listProviders()[0].first == "available", "registry: only usable providers are selectable");
    check(reg.listProviderInfos().size() == 2, "registry: all non-hidden providers are listed");
    reg.setDefaultProvider(DummySegmentationProvider::ID);
    check(reg.getDefaultProviderId() == "available", "registry: setDefaultProvider ignores hidden providers");

    SegmentationRequest req;
    req.L = L;
    req.a = a;
    req.b = b;
    req.width = W;
    req.height = H;
    req.frame = MaskFrame(W, H, 0, 0);
    req.multiThread = false;

    req.target = MaskComponentType::AI_SKY;
    check(reg.getProvider(DummySegmentationProvider::ID)->segment(req, out), "dummy: sky succeeds");
    check(out[H / 4][W / 2] > 0.8f, "dummy: sky is detected in the upper half");
    check(out[(3 * H) / 4][W / 2] < 0.1f, "dummy: ground is not sky");

    req.target = MaskComponentType::AI_SUBJECT;
    check(reg.getProvider(DummySegmentationProvider::ID)->segment(req, out), "dummy: subject succeeds");
    check(out[(3 * H) / 4 - 4][W / 2] > 0.5f, "dummy: subject prefers the center");

    req.target = MaskComponentType::AI_OBJECT;
    req.hintX = 0.5;
    req.hintY = 0.8;
    check(reg.getProvider(DummySegmentationProvider::ID)->segment(req, out), "dummy: object succeeds");
    check(out[int(0.8 * H)][W / 2] > 0.95f, "dummy: object is 1 at the hint");
    check(out[H / 8][W / 2] < 0.2f, "dummy: object does not leak into a different color far away");

    bool inRange = true;

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (out[y][x] < 0.f || out[y][x] > 1.f) {
                inRange = false;
            }
        }
    }

    check(inRange, "dummy: output is within [0, 1]");
}

bool cropMatchesFull(const array2D<float>& full, const array2D<float>& crop, int ox, int oy, float eps = 1e-5f, int margin = 0)
{
    for (int y = margin; y < crop.getHeight() - margin; ++y) {
        for (int x = margin; x < crop.getWidth() - margin; ++x) {
            if (!near(crop[y][x], full[y + oy][x + ox], eps)) {
                std::printf("  mismatch at crop (%d, %d): %f vs %f\n", x, y, crop[y][x], full[y + oy][x + ox]);
                return false;
            }
        }
    }

    return true;
}

void testRectangle()
{
    const int W = 200, H = 100;
    MaskFrame frame(W, H, 0, 0);
    array2D<float> m(W, H);

    MaskComponent c(MaskComponentType::RECTANGLE);
    c.x0 = 0.25;
    c.y0 = 0.3;
    c.x1 = 0.75;
    c.y1 = 0.7;
    c.feather = 0.0;
    c.cornerRadius = 0.0;
    maskgeom::rectangle(c, frame, m, false);

    check(near(m[50][100], 1.f), "rect: center is 1");
    check(near(m[50][55], 1.f), "rect: just inside the left edge is 1");
    check(near(m[50][45], 0.f), "rect: outside the left edge is 0");
    check(near(m[25][100], 0.f), "rect: above the top edge is 0");
    check(near(m[32][52], 1.f), "rect: a sharp corner is filled");
    check(near(m[50][149], m[50][50]) && m[50][50] > 0.f && m[50][50] < 1.f, "rect: the edge pixel is anti-aliased symmetrically");

    // Swapped corners describe the same rectangle.
    MaskComponent swapped = c;
    std::swap(swapped.x0, swapped.x1);
    std::swap(swapped.y0, swapped.y1);
    array2D<float> m2(W, H);
    maskgeom::rectangle(swapped, frame, m2, false);
    check(cropMatchesFull(m, m2, 0, 0), "rect: corner order does not matter");

    // Rounded corners cut the corner pixels only.
    c.cornerRadius = 100.0;
    maskgeom::rectangle(c, frame, m, false);
    check(near(m[32][52], 0.f), "rect: rounded corner is cut");
    check(near(m[50][52], 1.f), "rect: edge middle stays with rounded corners");
    check(near(m[50][100], 1.f), "rect: center stays with rounded corners");

    // Feather: fades inwards, the center of a 100 % feather is still 1-ish, the edge is 0.
    c.cornerRadius = 0.0;
    c.feather = 100.0;
    maskgeom::rectangle(c, frame, m, false);
    check(m[50][100] > 0.95f, "rect: feather keeps the center");
    check(m[50][60] > 0.f && m[50][60] < 1.f, "rect: feather zone is partial");
    check(m[50][60] > m[50][52], "rect: feather fades towards the edge");
    check(near(m[50][49], 0.f), "rect: feather is inside only");

    // Rotation by 90 degrees swaps the extents.
    c.feather = 0.0;
    c.angle = 90.0;
    maskgeom::rectangle(c, frame, m, false);
    check(near(m[50][100], 1.f), "rect: rotated center is 1");
    check(near(m[50][70], 0.f), "rect: rotation swaps the extents (x)");
    check(near(m[5][100], 1.f), "rect: rotation swaps the extents (y)");

    // Detail crop consistency.
    c.angle = 20.0;
    c.feather = 40.0;
    c.cornerRadius = 50.0;
    array2D<float> full(W, H);
    maskgeom::rectangle(c, frame, full, false);
    MaskFrame cropFrame(W, H, 37, 21);
    array2D<float> crop(80, 50);
    maskgeom::rectangle(c, cropFrame, crop, false);
    check(cropMatchesFull(full, crop, 37, 21), "rect: detail crop matches the full frame");
}

void testPolygon()
{
    const int W = 200, H = 100;
    MaskFrame frame(W, H, 0, 0);
    array2D<float> m(W, H);

    // A diamond.
    MaskComponent c(MaskComponentType::POLYGON);
    c.points = {0.5, 0.1, 0.8, 0.5, 0.5, 0.9, 0.2, 0.5};
    c.feather = 0.0;
    maskgeom::polygon(c, frame, m, false);

    check(near(m[50][100], 1.f), "polygon: center is 1");
    check(near(m[50][45], 1.f), "polygon: inside near the left vertex is 1");
    check(near(m[50][35], 0.f), "polygon: outside the left vertex is 0");
    check(near(m[15][60], 0.f), "polygon: outside a slanted edge is 0");
    check(near(m[5][100], 0.f), "polygon: above the top vertex is 0");
    check(near(m[85][100], 1.f), "polygon: just inside the bottom vertex is 1");

    // Anti-aliasing: the slanted edge has partial pixels.
    bool partial = false;

    for (int x = 40; x < 100; ++x) {
        if (m[30][x] > 0.05f && m[30][x] < 0.95f) {
            partial = true;
        }
    }

    check(partial, "polygon: slanted edges are anti-aliased");

    // Too few points: empty.
    MaskComponent two(MaskComponentType::POLYGON);
    two.points = {0.1, 0.1, 0.9, 0.9};
    maskgeom::polygon(two, frame, m, false);
    check(maskgeom::isEmpty(m), "polygon: fewer than 3 points gives an empty mask");

    // Self intersecting bow tie: both lobes are filled with either fill rule.
    MaskComponent bow(MaskComponentType::POLYGON);
    bow.points = {0.1, 0.1, 0.9, 0.9, 0.9, 0.1, 0.1, 0.9};
    maskgeom::polygon(bow, frame, m, false);
    check(near(m[50][40], 1.f) && near(m[50][160], 1.f), "polygon: bow tie lobes are filled (non-zero)");
    check(near(m[10][100], 0.f) && near(m[90][100], 0.f), "polygon: bow tie gaps are empty");
    bow.evenOdd = true;
    maskgeom::polygon(bow, frame, m, false);
    check(near(m[50][40], 1.f) && near(m[50][160], 1.f), "polygon: bow tie lobes are filled (even-odd)");

    // Star pentagram: the center is filled with non-zero winding, empty with even-odd.
    MaskComponent star(MaskComponentType::POLYGON);

    for (int i = 0; i < 5; ++i) {
        const double t = -M_PI / 2 + i * 4.0 * M_PI / 5.0;
        star.points.push_back(0.5 + 0.2 * std::cos(t));
        star.points.push_back(0.5 + 0.4 * std::sin(t));
    }

    maskgeom::polygon(star, frame, m, false);
    check(near(m[50][100], 1.f), "polygon: non-zero fills the pentagram center");
    star.evenOdd = true;
    maskgeom::polygon(star, frame, m, false);
    check(near(m[50][100], 0.f), "polygon: even-odd leaves the pentagram center empty");

    // Feather is symmetric around the outline.
    MaskComponent rect(MaskComponentType::POLYGON);
    rect.points = {0.25, 0.2, 0.75, 0.2, 0.75, 0.8, 0.25, 0.8};
    rect.feather = 50.0;
    maskgeom::polygon(rect, frame, m, false);
    const double r = maskgeom::polygonFeatherPixels(rect, frame);
    check(r > 2.0, "polygon: feather radius is a few pixels");
    check(near(m[50][100], 1.f), "polygon: feathered center is 1");
    check(near(m[50][50], 0.5f, 0.15f), "polygon: the outline is at 0.5");
    check(m[50][50 - int(r / 2)] > 0.05f && m[50][50 - int(r / 2)] < 0.5f, "polygon: feather extends outside");
    check(m[50][50 + int(r / 2)] > 0.5f && m[50][50 + int(r / 2)] < 0.95f, "polygon: feather extends inside");
    check(near(m[50][50 - int(r) - 2], 0.f), "polygon: beyond the feather is 0");
    check(near(m[50][50 + int(r) + 2], 1.f), "polygon: inside beyond the feather is 1");

    // Detail crop consistency, with and without feather, including a crop whose
    // border cuts through the feather zone.
    for (double f : {0.0, 60.0}) {
        rect.feather = f;
        array2D<float> full(W, H);
        maskgeom::polygon(rect, frame, full, false);
        MaskFrame cropFrame(W, H, 40, 15);
        array2D<float> crop(70, 60);
        maskgeom::polygon(rect, cropFrame, crop, false);
        check(cropMatchesFull(full, crop, 40, 15, 1e-4f), f > 0 ? "polygon: feathered detail crop matches the full frame" : "polygon: detail crop matches the full frame");
    }

    // A polygon that extends beyond the image edge: the image border is not an outline.
    MaskComponent big(MaskComponentType::POLYGON);
    big.points = {-0.2, -0.2, 0.6, -0.2, 0.6, 1.2, -0.2, 1.2};
    big.feather = 60.0;
    array2D<float> full(W, H);
    maskgeom::polygon(big, frame, full, false);
    check(near(full[0][0], 1.f), "polygon: image border inside a big polygon is 1");
    MaskFrame cropFrame(W, H, 0, 0);
    array2D<float> crop(60, 40);
    maskgeom::polygon(big, cropFrame, crop, false);
    check(cropMatchesFull(full, crop, 0, 0, 1e-4f), "polygon: crop at the image corner matches");
}

void fillPlanes(array2D<float>& L, array2D<float>& a, array2D<float>& b, int W, int H)
{
    // Left half: red-ish, right half: blue-ish, a green island in the left half,
    // and a thin red bridge connecting a second red patch on the right side.
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            bool red = x < W / 2;

            if (x >= W / 4 - 5 && x <= W / 4 + 5 && y >= H / 2 - 5 && y <= H / 2 + 5) {
                red = false;   // island (treated as "green" below)
            }

            const bool island = !red && x < W / 2;
            const bool bridge = y == H / 4 && x >= W / 2 && x < 3 * W / 4;
            const bool patch = x >= 3 * W / 4 && x < 3 * W / 4 + 10 && y >= H / 4 - 5 && y <= H / 4 + 5;
            const bool patch2 = x >= 3 * W / 4 && x < 3 * W / 4 + 10 && y >= 3 * H / 4 - 5 && y <= 3 * H / 4 + 5;   // not connected

            if (red || bridge || patch || patch2) {
                L[y][x] = 50.f * 327.68f;
                a[y][x] = 60.f * 327.68f;
                b[y][x] = 40.f * 327.68f;
            } else if (island) {
                L[y][x] = 50.f * 327.68f;
                a[y][x] = -60.f * 327.68f;
                b[y][x] = 40.f * 327.68f;
            } else {
                L[y][x] = 50.f * 327.68f;
                a[y][x] = 0.f;
                b[y][x] = -60.f * 327.68f;
            }
        }
    }
}

void testMagicWand()
{
    const int W = 160, H = 80;
    array2D<float> L(W, H), a(W, H), b(W, H), out(W, H);
    fillPlanes(L, a, b, W, H);
    MaskFrame frame(W, H, 0, 0);

    MaskComponent c(MaskComponentType::MAGIC_WAND);
    c.tolerance = 20.0;
    c.smoothness = 0.0;
    c.seeds = {0.1, 0.8, 50.0, 60.0, 40.0};   // in the red half
    c.contiguous = true;

    check(near(maskgeom::wandWeight(c, 50.f, 60.f, 40.f), 1.f), "wand: seed color weighs 1");
    check(near(maskgeom::wandWeight(c, 50.f, 0.f, -60.f), 0.f), "wand: a different color weighs 0");

    maskgeom::magicWand(c, frame, L, a, b, W, H, out, false);
    check(near(out[70][16], 1.f), "wand: the seed is selected");
    check(near(out[10][10], 1.f), "wand: the connected region is selected");
    check(near(out[H / 2][W / 4], 0.f), "wand: the island of another color is not selected");
    check(near(out[40][120], 0.f), "wand: the other half is not selected");
    check(near(out[H / 4][3 * W / 4 + 5], 1.f), "wand: a patch connected by a 1 px bridge is selected");
    check(near(out[3 * H / 4][3 * W / 4 + 5], 0.f), "wand: a disconnected patch of the same color is not selected");

    c.contiguous = false;
    maskgeom::magicWand(c, frame, L, a, b, W, H, out, false);
    check(near(out[3 * H / 4][3 * W / 4 + 5], 1.f), "wand: 'similar' selects disconnected patches too");
    check(near(out[40][120], 0.f), "wand: 'similar' still rejects other colors");

    // Several seeds are a union.
    c.contiguous = true;
    c.seeds.insert(c.seeds.end(), {0.8, 0.5, 50.0, 0.0, -60.0});
    maskgeom::magicWand(c, frame, L, a, b, W, H, out, false);
    check(near(out[40][120], 1.f) && near(out[10][10], 1.f), "wand: several seeds select the union");
    check(near(out[H / 2][W / 4], 0.f), "wand: the island is still excluded with two seeds");

    // No seeds: empty.
    MaskComponent none(MaskComponentType::MAGIC_WAND);
    maskgeom::magicWand(none, frame, L, a, b, W, H, out, false);
    check(maskgeom::isEmpty(out), "wand: no seeds gives an empty mask");

    // Detail crop: a crop that contains no seed but part of the connected red
    // region, plus the disconnected patch. With the full frame as reference the
    // crop must match the full evaluation exactly.
    c.seeds = {0.1, 0.8, 50.0, 60.0, 40.0};
    array2D<float> full(W, H);
    maskgeom::magicWand(c, frame, L, a, b, W, H, full, false);

    const int ox = 100, oy = 10, cw = 50, ch = 60;
    array2D<float> cL(cw, ch), ca(cw, ch), cb(cw, ch), crop(cw, ch);

    for (int y = 0; y < ch; ++y) {
        for (int x = 0; x < cw; ++x) {
            cL[y][x] = L[y + oy][x + ox];
            ca[y][x] = a[y + oy][x + ox];
            cb[y][x] = b[y + oy][x + ox];
        }
    }

    MaskFrame cropFrame(W, H, ox, oy);
    maskgeom::LabPlanes ref;
    ref.L = L;
    ref.a = a;
    ref.b = b;
    ref.W = W;
    ref.H = H;
    ref.frame = frame;
    maskgeom::magicWand(c, cropFrame, cL, ca, cb, cw, ch, crop, false, &ref);
    check(cropMatchesFull(full, crop, ox, oy), "wand: detail crop with a reference matches the full frame");
    check(near(crop[H / 4 - oy][3 * W / 4 + 5 - ox], 1.f), "wand: crop keeps the bridged patch");
    check(near(crop[3 * H / 4 - oy][3 * W / 4 + 5 - ox], 0.f), "wand: crop drops the disconnected patch");

    // Without a reference the crop can only guess: it seeds from its border.
    maskgeom::magicWand(c, cropFrame, cL, ca, cb, cw, ch, crop, false, nullptr);
    check(near(crop[H / 4 - oy][3 * W / 4 + 5 - ox], 1.f), "wand: crop without reference keeps the bridged patch");

    // A reference at a lower resolution still provides the connectivity.
    const int rw = W / 4, rh = H / 4;
    array2D<float> rL(rw, rh), ra(rw, rh), rb(rw, rh);

    for (int y = 0; y < rh; ++y) {
        for (int x = 0; x < rw; ++x) {
            rL[y][x] = L[y * 4][x * 4];
            ra[y][x] = a[y * 4][x * 4];
            rb[y][x] = b[y * 4][x * 4];
        }
    }

    ref.L = rL;
    ref.a = ra;
    ref.b = rb;
    ref.W = rw;
    ref.H = rh;
    ref.frame = MaskFrame(rw, rh, 0, 0);
    maskgeom::magicWand(c, cropFrame, cL, ca, cb, cw, ch, crop, false, &ref);
    check(near(crop[H / 4 - oy][3 * W / 4 + 5 - ox], 1.f), "wand: low-res reference keeps the bridged patch");
    check(near(crop[3 * H / 4 - oy][3 * W / 4 + 5 - ox], 0.f), "wand: low-res reference drops the disconnected patch");
}

// Procedural test image for the quick selection, sampled at any resolution.
void qsImage(double nx, double ny, double aspect, float& l, float& a, float& b)
{
    auto inDisc = [&](double cx, double cy, double r) {
        const double dx = (nx - cx) * aspect;
        const double dy = ny - cy;
        return dx * dx + dy * dy <= r * r;
    };

    if (inDisc(0.3, 0.5, 0.25) || inDisc(0.9, 0.15, 0.08)) {   // red disc + disconnected red disc
        l = 50.f; a = 60.f; b = 40.f;
    } else if (nx >= 0.6 && nx <= 0.75 && ny >= 0.3 && ny <= 0.7) {   // green block
        l = 60.f; a = -50.f; b = 30.f;
    } else {   // gray-blue background
        l = 50.f; a = 0.f; b = -30.f;
    }
}

void qsFill(array2D<float>& L, array2D<float>& a, array2D<float>& b, const MaskFrame& f, int W, int H)
{
    const double aspect = double(f.imageWidth) / f.imageHeight;

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float l, aa, bb;
            qsImage(f.normX(x + 0.5), f.normY(y + 0.5), aspect, l, aa, bb);
            L[y][x] = l * 327.68f;
            a[y][x] = aa * 327.68f;
            b[y][x] = bb * 327.68f;
        }
    }
}

MaskBrushStroke qsStroke(std::vector<double> pts, double size, bool erase)
{
    MaskBrushStroke s;
    s.size = size;
    s.erase = erase;
    s.points = std::move(pts);
    return s;
}

/// value of the mask at a normalized position
float qsAt(const array2D<float>& m, const MaskFrame& f, double nx, double ny)
{
    const int x = int(std::floor(f.bufferX(nx)));
    const int y = int(std::floor(f.bufferY(ny)));
    return m[y][x];
}

void testQuickSelect()
{
    const int W = 640, H = 320;
    MaskFrame frame(W, H, 0, 0);
    array2D<float> L(W, H), a(W, H), b(W, H), out(W, H);
    qsFill(L, a, b, frame, W, H);

    MaskComponent c(MaskComponentType::QUICK_SELECT);
    check(c.tolerance == 40.0 && c.edgeSensitivity == 50.0, "qs: defaults");
    c.strokes.push_back(qsStroke({0.28, 0.5, 0.32, 0.52}, 0.01, false));

    maskgeom::quickSelect(c, frame, L, a, b, W, H, out, true);
    check(near(qsAt(out, frame, 0.3, 0.5), 1.f), "qs: the stroke is selected");
    check(near(qsAt(out, frame, 0.3, 0.3), 1.f) && near(qsAt(out, frame, 0.22, 0.6), 1.f), "qs: grows over the whole disc");
    check(near(qsAt(out, frame, 0.5, 0.1), 0.f), "qs: stops at the edge of the disc");
    check(near(qsAt(out, frame, 0.67, 0.5), 0.f), "qs: the green block is not selected");
    check(near(qsAt(out, frame, 0.9, 0.15), 0.f), "qs: a disconnected patch of the same color is not selected");

    // Edges follow the image at buffer resolution (joint bilateral upsampling).
    {
        int bad = 0;

        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const bool red = a[y][x] > 50.f * 327.68f && x < W / 2;
                if (std::fabs(out[y][x] - (red ? 1.f : 0.f)) > 0.25f) {
                    ++bad;
                }
            }
        }

        if (bad >= W * H / 2000) std::printf("  bad edge pixels %d\n", bad);
        check(bad < W * H / 2000, "qs: edge pixels follow the image");
    }

    // No strokes: empty.
    MaskComponent none(MaskComponentType::QUICK_SELECT);
    maskgeom::quickSelect(none, frame, L, a, b, W, H, out, false);
    check(maskgeom::isEmpty(out), "qs: no strokes gives an empty mask");

    // A stroke in the flat background selects the connected background, not the objects.
    MaskComponent bgc(MaskComponentType::QUICK_SELECT);
    bgc.strokes.push_back(qsStroke({0.5, 0.9}, 0.01, false));
    maskgeom::quickSelect(bgc, frame, L, a, b, W, H, out, true);
    check(near(qsAt(out, frame, 0.98, 0.98), 1.f) && near(qsAt(out, frame, 0.02, 0.02), 1.f), "qs: flat background fills");
    check(near(qsAt(out, frame, 0.3, 0.5), 0.f) && near(qsAt(out, frame, 0.67, 0.5), 0.f), "qs: background excludes the objects");

    // Erase strokes: background seeds. The region they claim is reported in
    // eraseOut and taken out of the grown region (the cells geodesically
    // closer to the erase seeds go to them).
    MaskComponent er(MaskComponentType::QUICK_SELECT);
    er.strokes.push_back(qsStroke({0.36, 0.5}, 0.01, false));
    er.strokes.push_back(qsStroke({0.5, 0.1, 0.5, 0.9}, 0.01, true));
    array2D<float> erased(W, H);
    maskgeom::quickSelect(er, frame, L, a, b, W, H, out, true, nullptr, &erased);
    check(near(qsAt(out, frame, 0.3, 0.5), 1.f) && near(qsAt(out, frame, 0.5, 0.5), 0.f), "qs: erase in the background keeps the disc");
    check(near(qsAt(erased, frame, 0.05, 0.95), 1.f) && near(qsAt(erased, frame, 0.3, 0.5), 0.f), "qs: erase region output covers the background");
    er.strokes.push_back(qsStroke({0.22, 0.5}, 0.01, true));
    maskgeom::quickSelect(er, frame, L, a, b, W, H, out, true, nullptr, &erased);
    check(near(qsAt(out, frame, 0.36, 0.5), 1.f), "qs: erase inside the selection keeps the add side");
    check(near(qsAt(out, frame, 0.22, 0.5), 0.f) && near(qsAt(out, frame, 0.2, 0.5), 0.f), "qs: erase inside the selection removes its side");

    // Stroke order: an add stroke over an earlier erase footprint wins there.
    MaskComponent order(MaskComponentType::QUICK_SELECT);
    order.strokes.push_back(qsStroke({0.3, 0.5}, 0.02, true));
    order.strokes.push_back(qsStroke({0.3, 0.5}, 0.005, false));
    maskgeom::quickSelect(order, frame, L, a, b, W, H, out, false);
    check(near(qsAt(out, frame, 0.3, 0.5), 1.f), "qs: later add stroke wins over an earlier erase");

    // Another resolution of the same image gives the same mask (away from edges: identical).
    {
        const int W2 = 320, H2 = 160;
        MaskFrame f2(W2, H2, 0, 0);
        array2D<float> L2(W2, H2), a2(W2, H2), b2(W2, H2), o2(W2, H2);
        qsFill(L2, a2, b2, f2, W2, H2);
        maskgeom::quickSelect(c, frame, L, a, b, W, H, out, false);
        maskgeom::quickSelect(c, f2, L2, a2, b2, W2, H2, o2, false);
        double sum = 0.0;

        for (int y = 0; y < H2; ++y) {
            for (int x = 0; x < W2; ++x) {
                sum += std::fabs(o2[y][x] - 0.25f * (out[2 * y][2 * x] + out[2 * y][2 * x + 1] + out[2 * y + 1][2 * x] + out[2 * y + 1][2 * x + 1]));
            }
        }

        check(sum / (W2 * H2) < 0.01, "qs: half resolution matches the full resolution");
    }

    // Detail crop: with the full frame as reference the crop matches exactly,
    // also when it contains no seed.
    maskgeom::quickSelect(c, frame, L, a, b, W, H, out, false);

    for (const auto& r : std::vector<std::array<int, 4>> {{{100, 60, 200, 150}}, {{0, 0, 120, 90}}, {{330, 200, 300, 120}}}) {
        const int ox = r[0], oy = r[1], cw = r[2], ch = r[3];
        array2D<float> cL(cw, ch), ca(cw, ch), cb(cw, ch), crop(cw, ch);

        for (int y = 0; y < ch; ++y) {
            for (int x = 0; x < cw; ++x) {
                cL[y][x] = L[y + oy][x + ox];
                ca[y][x] = a[y + oy][x + ox];
                cb[y][x] = b[y + oy][x + ox];
            }
        }

        MaskFrame cropFrame(W, H, ox, oy);
        maskgeom::LabPlanes ref;
        ref.L = L;
        ref.a = a;
        ref.b = b;
        ref.W = W;
        ref.H = H;
        ref.frame = frame;
        maskgeom::quickSelect(c, cropFrame, cL, ca, cb, cw, ch, crop, true, &ref);
        check(cropMatchesFull(out, crop, ox, oy, 1e-5f), "qs: detail crop with a full-frame reference matches the full frame");
    }

    // 1:1 detail crop of a 2x image with the half-size preview as reference
    // (what dcrop.cc does): matches the 2x full frame up to edge pixels.
    {
        const int W2 = 2 * W, H2 = 2 * H;
        MaskFrame f2(W2, H2, 0, 0);
        array2D<float> L2(W2, H2), a2(W2, H2), b2(W2, H2), full2(W2, H2);
        qsFill(L2, a2, b2, f2, W2, H2);
        maskgeom::quickSelect(c, f2, L2, a2, b2, W2, H2, full2, true);
        const int ox = 300, oy = 150, cw = 400, ch = 300;
        array2D<float> cL(cw, ch), ca(cw, ch), cb(cw, ch), crop(cw, ch);
        MaskFrame cropFrame(W2, H2, ox, oy);
        qsFill(cL, ca, cb, cropFrame, cw, ch);
        maskgeom::LabPlanes ref;
        ref.L = L;
        ref.a = a;
        ref.b = b;
        ref.W = W;
        ref.H = H;
        ref.frame = frame;
        maskgeom::quickSelect(c, cropFrame, cL, ca, cb, cw, ch, crop, true, &ref);
        int diff = 0;

        for (int y = 0; y < ch; ++y) {
            for (int x = 0; x < cw; ++x) {
                if (std::fabs(crop[y][x] - full2[y + oy][x + ox]) > 0.02f) {
                    ++diff;
                }
            }
        }

        if (diff > cw * ch / 1000) std::printf("  crop diff %d\n", diff);
        check(diff <= cw * ch / 1000, "qs: 1:1 crop with a preview reference matches the 2x full frame");
        check(near(crop[0][0], full2[oy][ox]) && crop[150][0] > 0.99f, "qs: 1:1 crop selects the disc part it shows");
    }
}

void testMorphology()
{
    const int W = 120, H = 80;
    array2D<float> m(W, H);

    // Dilation of a single pixel gives a disc-like blob of the radius.
    m.fill(0.f, false);
    m[40][60] = 1.f;
    maskgeom::morphology(m, 10, true, false);
    check(near(m[40][60], 1.f), "morph: dilation keeps the seed");
    check(near(m[40][69], 1.f) && near(m[31][60], 1.f), "morph: dilation reaches the radius along the axes");
    check(near(m[40][72], 0.f) && near(m[28][60], 0.f), "morph: dilation stops after the radius");
    check(near(m[47][67], 1.f), "morph: dilation reaches along the diagonal");
    check(near(m[50][70], 0.f), "morph: dilation is not a square");

    // Erosion of a square shrinks it by the radius; the image border is not eroded.
    m.fill(0.f, false);

    for (int y = 0; y < 60; ++y) {
        for (int x = 0; x < 60; ++x) {
            m[y][x] = 1.f;
        }
    }

    maskgeom::morphology(m, 10, false, false);
    check(near(m[0][0], 1.f), "morph: erosion does not eat the image border");
    check(near(m[30][48], 1.f), "morph: erosion keeps the interior");
    check(near(m[30][52], 0.f), "morph: erosion removes the band near the edge");
    check(near(m[30][30], 1.f), "morph: erosion keeps the center");

    // Soft values are preserved by a dilation (grayscale morphology).
    m.fill(0.f, false);
    m[40][60] = 0.4f;
    maskgeom::morphology(m, 3, true, false);
    check(near(m[40][62], 0.4f), "morph: grayscale dilation propagates the value");

    // Smoothing removes a speck and fills a hole, keeps a large square.
    m.fill(0.f, false);

    for (int y = 20; y < 60; ++y) {
        for (int x = 20; x < 100; ++x) {
            m[y][x] = 1.f;
        }
    }

    m[40][60] = 0.f;       // hole
    m[70][110] = 1.f;      // speck
    maskgeom::smoothMask(m, 3, false);
    check(near(m[40][60], 1.f), "smooth: a hole is filled");
    check(near(m[70][110], 0.f), "smooth: a speck is removed");
    check(near(m[40][30], 1.f) && near(m[10][30], 0.f), "smooth: the large shape stays");
    check(m[20][20] < 1.f, "smooth: convex corners are rounded");

    // Detail crop consistency away from the crop border (the refinement
    // looks radius pixels around each pixel).
    MaskComponent c(MaskComponentType::POLYGON);
    c.points = {0.2, 0.2, 0.7, 0.3, 0.6, 0.8, 0.3, 0.7};
    MaskFrame frame(W, H, 0, 0);
    array2D<float> full(W, H);
    maskgeom::polygon(c, frame, full, false);
    const int r = maskgeom::expandRadiusPixels(100.0, frame);
    check(r >= 1, "morph: expand radius is at least a pixel on a small frame");
    maskgeom::morphology(full, r, true, false);
    const int ox = 20, oy = 10, cw = 70, ch = 50;
    MaskFrame cropFrame(W, H, ox, oy);
    array2D<float> crop(cw, ch);
    maskgeom::polygon(c, cropFrame, crop, false);
    maskgeom::morphology(crop, r, true, false);
    check(cropMatchesFull(full, crop, ox, oy, 1e-5f, r + 1), "morph: detail crop matches the full frame inside the margin");
    check(maskgeom::expandRadiusPixels(50.0, MaskFrame(4000, 3000, 0, 0)) == 25, "morph: expand radius scales with the frame diagonal");
    check(maskgeom::smoothRadiusPixels(100.0, MaskFrame(4000, 3000, 0, 0)) == 25, "morph: smooth radius scales with the frame diagonal");
}

/// Reference: max / min over the window of half width k along (dx, dy), truncated at the border.
void referencePass(array2D<float>& m, int dx, int dy, int k, bool useMax)
{
    const int W = m.getWidth();
    const int H = m.getHeight();
    const array2D<float> in(m);

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float v = in[y][x];

            for (int t = -k; t <= k; ++t) {
                const int xx = x + t * dx;
                const int yy = y + t * dy;

                if (xx >= 0 && yy >= 0 && xx < W && yy < H) {
                    v = useMax ? std::max(v, in[yy][xx]) : std::min(v, in[yy][xx]);
                }
            }

            m[y][x] = v;
        }
    }
}

void testMorphologyExact()
{
    // morphology() == the square + diamond decomposition with exact truncated
    // windows, for the direct (small radius) and the van Herk (large radius) passes.
    unsigned seed = 12345;
    auto rnd = [&seed]() {
        seed = seed * 1103515245u + 12345u;
        return float((seed >> 8) & 0xffff) / 65535.f;
    };
    int bad = 0;

    for (const int W : {1, 7, 66, 131}) {
        for (const int H : {1, 3, 50}) {
            for (const int r : {1, 2, 5, 9, 31, 40}) {
                for (const bool dilate : {false, true}) {
                    array2D<float> m(W, H), ref(W, H);

                    for (int y = 0; y < H; ++y) {
                        for (int x = 0; x < W; ++x) {
                            const float v = rnd();
                            m[y][x] = ref[y][x] = v < 0.7f ? 0.f : v;
                        }
                    }

                    maskgeom::morphology(m, r, dilate, false);
                    const int a = std::max(int(std::lround(r * (M_SQRT2 - 1.0))), 1);
                    const int bHalf = int(std::lround(r * (2.0 - M_SQRT2) * 0.5));
                    referencePass(ref, 1, 0, a, dilate);
                    referencePass(ref, 0, 1, a, dilate);

                    if (bHalf > 0) {
                        referencePass(ref, 1, 1, bHalf, dilate);
                        referencePass(ref, 1, -1, bHalf, dilate);
                    }

                    for (int y = 0; y < H; ++y) {
                        for (int x = 0; x < W; ++x) {
                            if (m[y][x] != ref[y][x]) {
                                ++bad;
                                y = H;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    check(bad == 0, "morph: matches the exact truncated-window decomposition (small and large radii)");
}

void testIncrementalStroke()
{
    // A growing stroke rasterized segment by segment equals the whole stroke
    // (bit-identical), and painting it onto a base matches brushApply.
    const int W = 160, H = 120;
    const MaskFrame frame(W, H, 0, 0);
    MaskBrushStroke s;
    s.size = 0.04;
    s.feather = 0.6;
    s.flow = 0.7;
    s.points = {0.1, 0.2, 0.2, 0.25, 0.35, 0.5, 0.5, 0.45, 0.62, 0.7, 0.8, 0.75};

    array2D<float> full(W, H), inc(W, H);
    full.fill(0.f, false);
    inc.fill(0.f, false);
    maskgeom::strokeCoverage(s, frame, full, false);

    MaskBrushStroke grow = s;
    grow.points.resize(4);   // 2 points
    maskgeom::MaskBox box = maskgeom::strokeCoverageSegments(grow, frame, inc, 0, false);

    for (size_t n = 3; n <= s.pointCount(); ++n) {
        const size_t before = grow.pointCount();
        grow.points.assign(s.points.begin(), s.points.begin() + 2 * n);
        box.unite(maskgeom::strokeCoverageSegments(grow, frame, inc, before - 1, false));
    }

    bool same = true;

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            same = same && full[y][x] == inc[y][x];
        }
    }

    check(same, "brush: incremental stroke rasterization is bit-identical");
    const maskgeom::MaskBox all = maskgeom::strokeBox(s, frame, W, H);
    check(box.x0 == all.x0 && box.y0 == all.y0 && box.x1 == all.x1 && box.y1 == all.y1, "brush: incremental boxes cover the stroke box");

    array2D<float> base(W, H), viaApply(W, H), viaBrush(W, H);

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            base[y][x] = viaBrush[y][x] = float((x * 7 + y * 3) % 10) / 10.f;
        }
    }

    viaApply = base;
    maskgeom::applyStrokeCoverage(base, full, all, float(s.flow), false, viaApply, false);
    MaskComponent c(MaskComponentType::BRUSH);
    c.strokes = {s};
    maskgeom::brushApply(c, frame, viaBrush, false, false);
    same = true;

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            same = same && viaApply[y][x] == viaBrush[y][x];
        }
    }

    check(same, "brush: applyStrokeCoverage matches brushApply");
}

void testRemap()
{
    MaskLayer layer;
    MaskComponent poly(MaskComponentType::POLYGON);
    poly.points = {0.5, 0.5, 0.75, 0.5};
    MaskComponent wand(MaskComponentType::MAGIC_WAND);
    wand.seeds = {0.5, 0.5, 10.0, 20.0, 30.0};
    layer.components = {poly, wand};
    std::vector<MaskLayer> masks = {layer};
    remapMasksToCrop(masks, 100, 100, 200, 200, 400, 400);
    check(near(float(masks[0].components[0].points[0]), 0.5f) && near(float(masks[0].components[0].points[2]), 1.0f), "remap: polygon points follow the crop");
    check(near(float(masks[0].components[1].seeds[0]), 0.5f) && near(float(masks[0].components[1].seeds[2]), 10.f), "remap: wand seeds follow the crop, colors are kept");
}

void testParamsEquality()
{
    MaskLayer l1, l2;
    check(l1 == l2, "params: default layers are equal");
    check(!l1.hasAdjustments(), "params: default layer has no adjustments");
    l2.exposure = 0.5;
    check(l1 != l2 && l2.hasAdjustments(), "params: adjustment makes layers differ");
    MaskComponent c1(MaskComponentType::BRUSH), c2(MaskComponentType::BRUSH);
    check(c1 == c2, "params: default components are equal");
    c2.strokes.emplace_back();
    check(c1 != c2, "params: strokes are compared");
    MaskComponent p1(MaskComponentType::POLYGON), p2(MaskComponentType::POLYGON);
    p2.points = {0.1, 0.1};
    check(p1 != p2, "params: polygon points are compared");
    p2.points.clear();
    p2.evenOdd = true;
    check(p1 != p2, "params: fill rule is compared");
    MaskComponent w1(MaskComponentType::MAGIC_WAND), w2(MaskComponentType::MAGIC_WAND);
    w2.contiguous = false;
    check(w1 != w2, "params: contiguous flag is compared");

    MaskComponent r1, r2;
    r2.aiRefineEdges = true;
    check(!r1.aiRefineEdges && r1 != r2, "params: AI edge refinement flag defaults to off and is compared");
    MaskLayer l3;
    l3.expand = 10.0;
    check(l1 != l3, "params: expand is compared");
    l3.expand = 0.0;
    l3.smooth = 10.0;
    check(l1 != l3, "params: smooth is compared");
}

}

int main()
{
    testSmoothstep();
    testLinearGradient();
    testRadialGradient();
    testBrush();
    testFrameConsistency();
    testRanges();
    testCombine();
    testRectangle();
    testPolygon();
    testMagicWand();
    testQuickSelect();
    testMorphology();
    testMorphologyExact();
    testIncrementalStroke();
    testRemap();
    testDummyProvider();
    testParamsEquality();

    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }

    std::printf("All mask geometry checks passed\n");
    return EXIT_SUCCESS;
}
