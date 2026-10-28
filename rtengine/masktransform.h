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
#pragma once

#include <memory>
#include <vector>

#include "maskparams.h"

namespace rtengine
{

class ImProcFunctions;

namespace procparams
{
class ProcParams;
}

/**
 * Maps mask coordinates between the frame they are stored in (the image
 * before the coarse rotation / flips and the geometric transform: rotation,
 * distortion, perspective, scale) and the output frame in which the masks are
 * rendered and edited (after them). This keeps masks attached to the image
 * content when the image is rotated, flipped or corrected.
 *
 * Profiled lens distortion is not modelled: the GUI and the engine use the
 * same map, so masks still render where they are drawn, they only don't
 * follow a later change of that correction exactly.
 */
class MaskTransform
{
public:
    /// outW / outH: size of the full output frame (any scale)
    MaskTransform(const procparams::ProcParams& params, int outW, int outH);
    ~MaskTransform();

    MaskTransform(const MaskTransform&) = delete;
    MaskTransform& operator=(const MaskTransform&) = delete;

    bool isIdentity() const
    {
        return identity;
    }

    /// true if a and b give the same map
    static bool sameGeometry(const procparams::ProcParams& a, const procparams::ProcParams& b);
    /// copies the parameters the map depends on
    static void copyGeometry(const procparams::ProcParams& from, procparams::ProcParams& to);

    /// normalized interleaved (x, y) pairs, in place
    void toOutput(std::vector<double>& xy) const;
    void toSource(std::vector<double>& xy) const;
    void toOutput(double& x, double& y) const;
    void toSource(double& x, double& y) const;

    void masksToOutput(std::vector<procparams::MaskLayer>& masks) const;
    void componentToOutput(procparams::MaskComponent& c) const;
    void componentToSource(procparams::MaskComponent& c) const;

    /// magnification from the stored frame to the output frame at the image center
    double scale() const
    {
        return centerScale;
    }

private:
    void coarseForward(double& x, double& y) const;
    void coarseInverse(double& x, double& y) const;
    /// output pixels -> pixels before the geometric transform, in place
    void fineInverse(std::vector<double>& px) const;
    /// pixels before the geometric transform -> output pixels, in place
    void fineForward(std::vector<double>& px) const;

    std::unique_ptr<procparams::ProcParams> params;
    std::unique_ptr<ImProcFunctions> ipf;
    int outW;
    int outH;
    int srcW;   ///< size of the stored frame (before the coarse rotation)
    int srcH;
    int rotate;
    bool hflip;
    bool vflip;
    bool fine;  ///< a geometric transform is present
    bool identity;
    double ascale;
    double centerScale;
    /// affine approximation of the inverse geometric map at the image center:
    /// source = c + J (output - center)
    double centerSrc[2];
    double centerJ[2][2];
};

}
