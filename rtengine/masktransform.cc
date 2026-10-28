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
#include "masktransform.h"

#include <algorithm>
#include <cmath>

#include "coord2d.h"
#include "improcfun.h"
#include "procparams.h"

namespace rtengine
{

using procparams::ProcParams;

MaskTransform::MaskTransform(const ProcParams& p, int w, int h) :
    params(new ProcParams()),
    outW(std::max(w, 1)),
    outH(std::max(h, 1)),
    srcW(outW),
    srcH(outH),
    rotate(((p.coarse.rotate % 360) + 360) % 360),
    hflip(p.coarse.hflip),
    vflip(p.coarse.vflip),
    fine(false),
    identity(true),
    ascale(1.0),
    centerScale(1.0),
    centerSrc{0.0, 0.0},
    centerJ{{1.0, 0.0}, {0.0, 1.0}}
{
    if (rotate == 90 || rotate == 270) {
        std::swap(srcW, srcH);
    }

    copyGeometry(p, *params);
    ipf.reset(new ImProcFunctions(params.get(), false));

    if (outW > 1 && outH > 1) {
        ascale = params->commonTrans.autofill && params->perspective.render ? ipf->getTransformAutoFill(outW, outH) : 1.0 / params->commonTrans.getScale();

        // transCoord returns the points unchanged when no geometric transform
        // is needed: probe the corners, edge middles and the center.
        std::vector<double> probe;

        for (double fy : {0.0, 0.5, 1.0}) {
            for (double fx : {0.0, 0.5, 1.0}) {
                probe.push_back(fx * (outW - 1));
                probe.push_back(fy * (outH - 1));
            }
        }

        std::vector<double> mapped = probe;
        fine = true;
        fineInverse(mapped);
        fine = false;

        for (size_t i = 0; i < probe.size(); ++i) {
            if (std::fabs(mapped[i] - probe[i]) > 1e-9) {
                fine = true;
                break;
            }
        }
    }

    identity = !fine && rotate == 0 && !hflip && !vflip;

    if (fine) {
        // magnification at the center = 1 / sqrt(det) of the inverse map
        const double cx = 0.5 * (outW - 1);
        const double cy = 0.5 * (outH - 1);
        std::vector<double> px = {cx, cy, cx + 1.0, cy, cx, cy + 1.0};
        fineInverse(px);
        centerSrc[0] = px[0];
        centerSrc[1] = px[1];
        centerJ[0][0] = px[2] - px[0];
        centerJ[0][1] = px[4] - px[0];
        centerJ[1][0] = px[3] - px[1];
        centerJ[1][1] = px[5] - px[1];
        const double det = centerJ[0][0] * centerJ[1][1] - centerJ[0][1] * centerJ[1][0];
        centerScale = std::fabs(det) > 1e-12 ? 1.0 / std::sqrt(std::fabs(det)) : 1.0;
    }
}

MaskTransform::~MaskTransform() = default;

bool MaskTransform::sameGeometry(const ProcParams& a, const ProcParams& b)
{
    return a.coarse == b.coarse
           && a.commonTrans == b.commonTrans
           && a.rotate == b.rotate
           && a.distortion == b.distortion
           && a.perspective == b.perspective
           && a.cacorrection == b.cacorrection;
}

void MaskTransform::copyGeometry(const ProcParams& from, ProcParams& to)
{
    to.coarse = from.coarse;
    to.commonTrans = from.commonTrans;
    to.rotate = from.rotate;
    to.distortion = from.distortion;
    to.perspective = from.perspective;
    to.cacorrection = from.cacorrection;   // takes part in the auto-fill scale
}

void MaskTransform::coarseForward(double& x, double& y) const
{
    // same order as the raw / image sources: rotate clockwise, then flip
    // (see RawImageSource::transformPosition)
    const double ox = x;
    const double oy = y;

    switch (rotate) {
        case 90:
            x = 1.0 - oy;
            y = ox;
            break;

        case 180:
            x = 1.0 - ox;
            y = 1.0 - oy;
            break;

        case 270:
            x = oy;
            y = 1.0 - ox;
            break;

        default:
            break;
    }

    if (hflip) {
        x = 1.0 - x;
    }

    if (vflip) {
        y = 1.0 - y;
    }
}

void MaskTransform::coarseInverse(double& x, double& y) const
{
    if (hflip) {
        x = 1.0 - x;
    }

    if (vflip) {
        y = 1.0 - y;
    }

    const double ox = x;
    const double oy = y;

    switch (rotate) {
        case 90:
            x = oy;
            y = 1.0 - ox;
            break;

        case 180:
            x = 1.0 - ox;
            y = 1.0 - oy;
            break;

        case 270:
            x = 1.0 - oy;
            y = ox;
            break;

        default:
            break;
    }
}

void MaskTransform::fineInverse(std::vector<double>& px) const
{
    if (!fine || px.empty()) {
        return;
    }

    std::vector<Coord2D> src;
    src.reserve(px.size() / 2);

    for (size_t i = 0; i + 1 < px.size(); i += 2) {
        src.emplace_back(px[i], px[i + 1]);
    }

    std::vector<Coord2D> red, green, blue;
    ipf->transCoord(outW, outH, src, red, green, blue, ascale, nullptr, false);

    for (size_t i = 0, k = 0; i + 1 < px.size(); i += 2, ++k) {
        px[i] = green[k].x;
        px[i + 1] = green[k].y;
    }
}

void MaskTransform::fineForward(std::vector<double>& px) const
{
    if (!fine || px.empty()) {
        return;
    }

    // Newton iterations on the inverse map T (output -> source): find d with
    // T(d) = p. The start point comes from the affine approximation of T at
    // the image center, which is exact for a pure rotation / scale.
    const size_t n = px.size() / 2;
    const std::vector<double> target(px.begin(), px.begin() + 2 * n);
    constexpr double h = 0.5;

    const double cx = 0.5 * (outW - 1);
    const double cy = 0.5 * (outH - 1);
    double a = centerJ[0][0], b = centerJ[0][1];
    double cc = centerJ[1][0], d = centerJ[1][1];
    double det = a * d - b * cc;

    std::vector<double> cur(2 * n);

    for (size_t i = 0; i < n; ++i) {
        const double rx = target[2 * i] - centerSrc[0];
        const double ry = target[2 * i + 1] - centerSrc[1];

        if (std::fabs(det) > 1e-12) {
            cur[2 * i] = cx + (d * rx - b * ry) / det;
            cur[2 * i + 1] = cy + (-cc * rx + a * ry) / det;
        } else {
            cur[2 * i] = target[2 * i];
            cur[2 * i + 1] = target[2 * i + 1];
        }
    }

    std::vector<size_t> active(n);

    for (size_t i = 0; i < n; ++i) {
        active[i] = i;
    }

    std::vector<double> probe;

    for (int iter = 0; iter < 12 && !active.empty(); ++iter) {
        probe.clear();

        for (size_t i : active) {
            const double x = cur[2 * i], y = cur[2 * i + 1];
            probe.insert(probe.end(), {x, y, x + h, y, x, y + h});
        }

        fineInverse(probe);
        std::vector<size_t> next;

        for (size_t k = 0; k < active.size(); ++k) {
            const size_t i = active[k];
            const double* t = &probe[6 * k];
            const double rx = target[2 * i] - t[0];
            const double ry = target[2 * i + 1] - t[1];

            if (rx * rx + ry * ry < 1e-8) {
                continue;
            }

            a = (t[2] - t[0]) / h;
            b = (t[4] - t[0]) / h;
            cc = (t[3] - t[1]) / h;
            d = (t[5] - t[1]) / h;
            det = a * d - b * cc;

            if (std::fabs(det) < 1e-12) {
                continue;
            }

            cur[2 * i] += (d * rx - b * ry) / det;
            cur[2 * i + 1] += (-cc * rx + a * ry) / det;
            next.push_back(i);
        }

        active.swap(next);
    }

    std::copy(cur.begin(), cur.end(), px.begin());
}

void MaskTransform::toOutput(std::vector<double>& xy) const
{
    if (identity) {
        return;
    }

    for (size_t i = 0; i + 1 < xy.size(); i += 2) {
        coarseForward(xy[i], xy[i + 1]);
        xy[i] *= outW;
        xy[i + 1] *= outH;
    }

    fineForward(xy);

    for (size_t i = 0; i + 1 < xy.size(); i += 2) {
        xy[i] /= outW;
        xy[i + 1] /= outH;
    }
}

void MaskTransform::toSource(std::vector<double>& xy) const
{
    if (identity) {
        return;
    }

    for (size_t i = 0; i + 1 < xy.size(); i += 2) {
        xy[i] *= outW;
        xy[i + 1] *= outH;
    }

    fineInverse(xy);

    for (size_t i = 0; i + 1 < xy.size(); i += 2) {
        xy[i] /= outW;
        xy[i + 1] /= outH;
        coarseInverse(xy[i], xy[i + 1]);
    }
}

void MaskTransform::toOutput(double& x, double& y) const
{
    std::vector<double> xy = {x, y};
    toOutput(xy);
    x = xy[0];
    y = xy[1];
}

void MaskTransform::toSource(double& x, double& y) const
{
    std::vector<double> xy = {x, y};
    toSource(xy);
    x = xy[0];
    y = xy[1];
}

void MaskTransform::masksToOutput(std::vector<procparams::MaskLayer>& masks) const
{
    if (!identity) {
        procparams::transformMasks(masks, [this](std::vector<double>& xy) { toOutput(xy); }, srcW, srcH, outW, outH);
    }
}

void MaskTransform::componentToOutput(procparams::MaskComponent& c) const
{
    if (!identity) {
        procparams::transformMaskComponent(c, [this](std::vector<double>& xy) { toOutput(xy); }, srcW, srcH, outW, outH);
    }
}

void MaskTransform::componentToSource(procparams::MaskComponent& c) const
{
    if (!identity) {
        procparams::transformMaskComponent(c, [this](std::vector<double>& xy) { toSource(xy); }, outW, outH, srcW, srcH);
    }
}

}
