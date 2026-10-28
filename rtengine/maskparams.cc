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

#define _USE_MATH_DEFINES   // M_PI on MinGW and MSVC
#include "maskparams.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace rtengine
{

namespace procparams
{

MaskBrushStroke::MaskBrushStroke() :
    size(0.03),
    feather(0.5),
    flow(1.0),
    erase(false),
    points()
{
}

bool MaskBrushStroke::operator ==(const MaskBrushStroke& other) const
{
    return size == other.size
           && feather == other.feather
           && flow == other.flow
           && erase == other.erase
           && points == other.points;
}

bool MaskBrushStroke::operator !=(const MaskBrushStroke& other) const
{
    return !(*this == other);
}

MaskComponent::MaskComponent() :
    MaskComponent(MaskComponentType::BRUSH)
{
}

MaskComponent::MaskComponent(MaskComponentType t) :
    type(t),
    op(MaskBooleanOp::ADD),
    enabled(true),
    invert(false),
    strokes(),
    x0(0.5),
    y0(0.25),
    x1(0.5),
    y1(0.75),
    centerX(0.5),
    centerY(0.5),
    radiusX(0.25),
    radiusY(0.18),
    angle(0.0),
    feather(50.0),
    sampleL(50.0),
    sampleA(0.0),
    sampleB(0.0),
    tolerance(50.0),
    smoothness(50.0),
    lumLow(50.0),
    lumHigh(100.0),
    lumFeatherLow(10.0),
    lumFeatherHigh(0.0),
    aiProvider(),
    aiPrompt(),
    aiHintX(0.5),
    aiHintY(0.5),
    aiThreshold(50.0),
    aiRefineEdges(false),
    cornerRadius(0.0),
    points(),
    evenOdd(false),
    seeds(),
    contiguous(true),
    edgeSensitivity(50.0)
{
    if (t == MaskComponentType::RECTANGLE) {
        x0 = 0.25;
        y0 = 0.25;
        x1 = 0.75;
        y1 = 0.75;
        feather = 0.0;
    } else if (t == MaskComponentType::POLYGON) {
        feather = 0.0;
    } else if (t == MaskComponentType::MAGIC_WAND) {
        tolerance = 30.0;
        smoothness = 30.0;
    } else if (t == MaskComponentType::QUICK_SELECT) {
        tolerance = 40.0;
        smoothness = 30.0;
    }
}

bool MaskComponent::operator ==(const MaskComponent& other) const
{
    return type == other.type
           && op == other.op
           && enabled == other.enabled
           && invert == other.invert
           && strokes == other.strokes
           && x0 == other.x0
           && y0 == other.y0
           && x1 == other.x1
           && y1 == other.y1
           && centerX == other.centerX
           && centerY == other.centerY
           && radiusX == other.radiusX
           && radiusY == other.radiusY
           && angle == other.angle
           && feather == other.feather
           && sampleL == other.sampleL
           && sampleA == other.sampleA
           && sampleB == other.sampleB
           && tolerance == other.tolerance
           && smoothness == other.smoothness
           && lumLow == other.lumLow
           && lumHigh == other.lumHigh
           && lumFeatherLow == other.lumFeatherLow
           && lumFeatherHigh == other.lumFeatherHigh
           && aiProvider == other.aiProvider
           && aiPrompt == other.aiPrompt
           && aiHintX == other.aiHintX
           && aiHintY == other.aiHintY
           && aiThreshold == other.aiThreshold
           && aiRefineEdges == other.aiRefineEdges
           && cornerRadius == other.cornerRadius
           && points == other.points
           && evenOdd == other.evenOdd
           && seeds == other.seeds
           && contiguous == other.contiguous
           && edgeSensitivity == other.edgeSensitivity;
}

bool MaskComponent::operator !=(const MaskComponent& other) const
{
    return !(*this == other);
}

MaskLayer::MaskLayer() :
    enabled(true),
    name(),
    invert(false),
    expand(0.0),
    smooth(0.0),
    feather(0.0),
    amount(100.0),
    components(),
    exposure(0.0),
    contrast(0.0),
    highlights(0.0),
    midtones(0.0),
    shadows(0.0),
    whites(0.0),
    blacks(0.0),
    temperature(0.0),
    tint(0.0),
    hue(0.0),
    saturation(0.0),
    clarity(0.0),
    lcurve{0.0},
    acurve{0.0},
    bcurve{0.0},
    cccurve{0.0},
    chcurve{0.0},
    lhcurve{0.0},
    hhcurve{0.0},
    lccurve{0.0},
    clcurve{0.0}
{
}

bool MaskLayer::hasCurves() const
{
    // the first value is the curve type, 0 = linear for diagonal and flat curves
    for (const std::vector<double>* c : {&lcurve, &acurve, &bcurve, &cccurve, &chcurve, &lhcurve, &hhcurve, &lccurve, &clcurve}) {
        if (!c->empty() && (*c)[0] != 0.0) {
            return true;
        }
    }

    return false;
}

bool MaskLayer::hasAdjustments() const
{
    return exposure != 0.0
           || contrast != 0.0
           || highlights != 0.0
           || midtones != 0.0
           || shadows != 0.0
           || whites != 0.0
           || blacks != 0.0
           || temperature != 0.0
           || tint != 0.0
           || hue != 0.0
           || saturation != 0.0
           || clarity != 0.0
           || hasCurves();
}

void MaskLayer::resetAdjustments()
{
    exposure = 0.0;
    contrast = 0.0;
    highlights = 0.0;
    midtones = 0.0;
    shadows = 0.0;
    whites = 0.0;
    blacks = 0.0;
    temperature = 0.0;
    tint = 0.0;
    hue = 0.0;
    saturation = 0.0;
    clarity = 0.0;

    for (std::vector<double>* c : {&lcurve, &acurve, &bcurve, &cccurve, &chcurve, &lhcurve, &hhcurve, &lccurve, &clcurve}) {
        c->assign(1, 0.0);
    }
}

bool MaskLayer::operator ==(const MaskLayer& other) const
{
    return enabled == other.enabled
           && name == other.name
           && invert == other.invert
           && expand == other.expand
           && smooth == other.smooth
           && feather == other.feather
           && amount == other.amount
           && components == other.components
           && exposure == other.exposure
           && contrast == other.contrast
           && highlights == other.highlights
           && midtones == other.midtones
           && shadows == other.shadows
           && whites == other.whites
           && blacks == other.blacks
           && temperature == other.temperature
           && tint == other.tint
           && hue == other.hue
           && saturation == other.saturation
           && clarity == other.clarity
           && lcurve == other.lcurve
           && acurve == other.acurve
           && bcurve == other.bcurve
           && cccurve == other.cccurve
           && chcurve == other.chcurve
           && lhcurve == other.lhcurve
           && hhcurve == other.hhcurve
           && lccurve == other.lccurve
           && clcurve == other.clcurve;
}

bool MaskLayer::operator !=(const MaskLayer& other) const
{
    return !(*this == other);
}

void remapMasksToCrop(std::vector<MaskLayer>& masks, double cropX, double cropY, double cropW, double cropH, double fullW, double fullH)
{
    if (cropW <= 0.0 || cropH <= 0.0 || fullW <= 0.0 || fullH <= 0.0) {
        return;
    }

    const double sizeFactor = std::sqrt(fullW * fullW + fullH * fullH) / std::sqrt(cropW * cropW + cropH * cropH);

    auto mapX = [&](double nx) {
        return (nx * fullW - cropX) / cropW;
    };
    auto mapY = [&](double ny) {
        return (ny * fullH - cropY) / cropH;
    };

    for (MaskLayer& m : masks) {
        for (MaskComponent& c : m.components) {
            c.x0 = mapX(c.x0);
            c.y0 = mapY(c.y0);
            c.x1 = mapX(c.x1);
            c.y1 = mapY(c.y1);
            c.centerX = mapX(c.centerX);
            c.centerY = mapY(c.centerY);
            c.radiusX *= sizeFactor;
            c.radiusY *= sizeFactor;
            c.aiHintX = mapX(c.aiHintX);
            c.aiHintY = mapY(c.aiHintY);

            for (size_t i = 0; i + 1 < c.points.size(); i += 2) {
                c.points[i] = mapX(c.points[i]);
                c.points[i + 1] = mapY(c.points[i + 1]);
            }

            for (size_t i = 0; i + 4 < c.seeds.size(); i += 5) {
                c.seeds[i] = mapX(c.seeds[i]);
                c.seeds[i + 1] = mapY(c.seeds[i + 1]);
            }

            for (MaskBrushStroke& s : c.strokes) {
                s.size *= sizeFactor;

                for (size_t i = 0; i + 1 < s.points.size(); i += 2) {
                    s.points[i] = mapX(s.points[i]);
                    s.points[i + 1] = mapY(s.points[i + 1]);
                }
            }
        }
    }
}

namespace
{

/// Position and pixel Jacobian (d dst / d src) of a map at one point.
struct LocalMap {
    double x, y;      ///< mapped point, normalized
    double j[2][2];   ///< Jacobian in pixels of the destination per pixel of the source
};

LocalMap localMap(const MaskPointMap& map, double nx, double ny, double srcW, double srcH, double dstW, double dstH)
{
    // central differences: the Jacobians of a map and of its inverse then
    // agree to second order, so that a round trip does not drift
    const double e = std::max(1e-3 * std::sqrt(srcW * srcW + srcH * srcH), 1e-6);
    const double ex = e / srcW;
    const double ey = e / srcH;
    std::vector<double> xy = {nx, ny, nx + ex, ny, nx - ex, ny, nx, ny + ey, nx, ny - ey};
    map(xy);
    LocalMap l;
    l.x = xy[0];
    l.y = xy[1];
    l.j[0][0] = (xy[2] - xy[4]) * dstW / (2.0 * e);
    l.j[1][0] = (xy[3] - xy[5]) * dstH / (2.0 * e);
    l.j[0][1] = (xy[6] - xy[8]) * dstW / (2.0 * e);
    l.j[1][1] = (xy[7] - xy[9]) * dstH / (2.0 * e);
    return l;
}

double localScale(const LocalMap& l)
{
    return std::sqrt(std::fabs(l.j[0][0] * l.j[1][1] - l.j[0][1] * l.j[1][0]));
}

void mapPoint(const MaskPointMap& map, double& x, double& y)
{
    std::vector<double> xy = {x, y};
    map(xy);
    x = xy[0];
    y = xy[1];
}

void mapPoints(const MaskPointMap& map, std::vector<double>& xy, size_t stride)
{
    if (stride == 2) {
        map(xy);
        return;
    }

    std::vector<double> tmp;
    tmp.reserve(xy.size() / stride * 2);

    for (size_t i = 0; i + stride - 1 < xy.size(); i += stride) {
        tmp.push_back(xy[i]);
        tmp.push_back(xy[i + 1]);
    }

    map(tmp);

    for (size_t i = 0, k = 0; i + stride - 1 < xy.size(); i += stride, k += 2) {
        xy[i] = tmp[k];
        xy[i + 1] = tmp[k + 1];
    }
}

/// Oriented box / ellipse: center (normalized), half axes (pixels) along
/// the direction 'angle' (degrees, axis = (cos, sin) in y-down pixels).
/// The ellipse x^T M^-1 x = 1, M = R diag(hx^2, hy^2) R^T, maps exactly to
/// M' = J M J^T under the local linear map J; its axes are the eigenvectors of
/// M'. This is exactly undone by the inverse map, also when J is not a
/// similarity (distortion, perspective).
void mapOriented(const MaskPointMap& map, double& cx, double& cy, double& hx, double& hy, double& angle, double srcW, double srcH, double dstW, double dstH)
{
    const LocalMap l = localMap(map, cx, cy, srcW, srcH, dstW, dstH);
    const double rad = angle * M_PI / 180.0;
    const double ux = std::cos(rad), uy = std::sin(rad);
    const double a2 = hx * hx, b2 = hy * hy;

    // M = R diag(a2, b2) R^T
    const double m00 = a2 * ux * ux + b2 * uy * uy;
    const double m01 = (a2 - b2) * ux * uy;
    const double m11 = a2 * uy * uy + b2 * ux * ux;

    // N = J M J^T
    const double (&j)[2][2] = l.j;
    const double t00 = j[0][0] * m00 + j[0][1] * m01;
    const double t01 = j[0][0] * m01 + j[0][1] * m11;
    const double t10 = j[1][0] * m00 + j[1][1] * m01;
    const double t11 = j[1][0] * m01 + j[1][1] * m11;
    const double n00 = t00 * j[0][0] + t01 * j[0][1];
    const double n01 = t00 * j[1][0] + t01 * j[1][1];
    const double n11 = t10 * j[1][0] + t11 * j[1][1];

    // the first axis follows the mapped first axis J u
    const double jux = j[0][0] * ux + j[0][1] * uy;
    const double juy = j[1][0] * ux + j[1][1] * uy;
    const double ref = std::atan2(juy, jux);

    const double mean = 0.5 * (n00 + n11);
    const double dev = std::sqrt(0.25 * (n00 - n11) * (n00 - n11) + n01 * n01);
    double theta = ref;
    // (nearly) a circle: the eigen-axes are ill-defined, keep the mapped
    // direction and measure the radii along it
    const double cr = std::cos(ref), sr = std::sin(ref);
    double l1 = n00 * cr * cr + 2.0 * n01 * cr * sr + n11 * sr * sr;
    double l2 = n00 * sr * sr - 2.0 * n01 * cr * sr + n11 * cr * cr;

    if (dev > 1e-3 * mean) {
        theta = 0.5 * std::atan2(2.0 * n01, n00 - n11);   // axis of the larger eigenvalue
        l1 = mean + dev;
        l2 = mean - dev;

        // pick the eigen-axis closest to J u (mod 180 degrees)
        double d = std::remainder(theta - ref, M_PI);

        if (std::fabs(d) > 0.25 * M_PI) {
            theta += M_PI / 2.0;
            std::swap(l1, l2);
            d = std::remainder(theta - ref, M_PI);
        }

        theta = ref + d;
    }

    cx = l.x;
    cy = l.y;
    hx = std::sqrt(std::max(l1, 0.0));
    hy = std::sqrt(std::max(l2, 0.0));
    angle = theta * 180.0 / M_PI;
}

}

void transformMaskComponent(MaskComponent& c, const MaskPointMap& map, double srcW, double srcH, double dstW, double dstH)
{
    const double srcDiag = std::sqrt(srcW * srcW + srcH * srcH);
    const double dstDiag = std::sqrt(dstW * dstW + dstH * dstH);

    switch (c.type) {
        case MaskComponentType::BRUSH:
        case MaskComponentType::QUICK_SELECT:
            for (MaskBrushStroke& s : c.strokes) {
                if (s.points.size() >= 2) {
                    const LocalMap l = localMap(map, s.points[0], s.points[1], srcW, srcH, dstW, dstH);
                    s.size *= localScale(l) * srcDiag / dstDiag;
                    mapPoints(map, s.points, 2);
                }
            }

            break;

        case MaskComponentType::LINEAR_GRADIENT: {
            std::vector<double> xy = {c.x0, c.y0, c.x1, c.y1};
            map(xy);
            c.x0 = xy[0];
            c.y0 = xy[1];
            c.x1 = xy[2];
            c.y1 = xy[3];
            break;
        }

        case MaskComponentType::RADIAL_GRADIENT: {
            double rx = c.radiusX * srcDiag;
            double ry = c.radiusY * srcDiag;
            mapOriented(map, c.centerX, c.centerY, rx, ry, c.angle, srcW, srcH, dstW, dstH);
            c.radiusX = rx / dstDiag;
            c.radiusY = ry / dstDiag;
            break;
        }

        case MaskComponentType::RECTANGLE: {
            double cx = 0.5 * (c.x0 + c.x1);
            double cy = 0.5 * (c.y0 + c.y1);
            double hx = 0.5 * std::fabs(c.x1 - c.x0) * srcW;
            double hy = 0.5 * std::fabs(c.y1 - c.y0) * srcH;
            mapOriented(map, cx, cy, hx, hy, c.angle, srcW, srcH, dstW, dstH);
            c.x0 = cx - hx / dstW;
            c.x1 = cx + hx / dstW;
            c.y0 = cy - hy / dstH;
            c.y1 = cy + hy / dstH;
            break;
        }

        case MaskComponentType::POLYGON:
            if (c.points.size() >= 2) {
                const LocalMap l = localMap(map, c.points[0], c.points[1], srcW, srcH, dstW, dstH);
                c.feather = std::min(c.feather * localScale(l) * srcDiag / dstDiag, 100.0);
                mapPoints(map, c.points, 2);
            }

            break;

        case MaskComponentType::MAGIC_WAND:
            mapPoints(map, c.seeds, 5);
            break;

        case MaskComponentType::AI_OBJECT:
            mapPoint(map, c.aiHintX, c.aiHintY);
            break;

        default:
            break;   // image based (ranges, AI subject / sky / background)
    }
}

void transformMasks(std::vector<MaskLayer>& masks, const MaskPointMap& map, double srcW, double srcH, double dstW, double dstH)
{
    for (MaskLayer& m : masks) {
        for (MaskComponent& c : m.components) {
            transformMaskComponent(c, map, srcW, srcH, dstW, dstH);
        }
    }
}

}

}
