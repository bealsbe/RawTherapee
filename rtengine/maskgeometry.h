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

/**
 * Mask evaluation math for the "Masks" tool.
 *
 * All functions rasterize one MaskComponent into a float array2D with values
 * in [0, 1] at the resolution of the processing buffer. They depend only on
 * array2D.h and maskparams.h so that they can be compiled and tested on their
 * own (see tests/maskgeometry_test.cc).
 *
 * Coordinate handling: a MaskFrame describes how the processing buffer maps
 * onto the whole image. Buffer pixel (x, y) is image pixel
 * (offsetX + x, offsetY + y) in a coordinate system where the whole image is
 * imageWidth x imageHeight pixels. All four pipelines (preview, detail crop,
 * batch, thumbnail) can be described this way, so the same component renders
 * identically everywhere.
 */

#include "array2D.h"
#include "maskparams.h"

namespace rtengine
{

struct MaskFrame {
    int imageWidth = 1;   ///< width of the whole image, in buffer pixels
    int imageHeight = 1;  ///< height of the whole image, in buffer pixels
    int offsetX = 0;      ///< image x of buffer pixel 0
    int offsetY = 0;      ///< image y of buffer pixel 0

    MaskFrame() = default;
    MaskFrame(int w, int h, int ox, int oy) : imageWidth(w), imageHeight(h), offsetX(ox), offsetY(oy) {}

    /// length of the image diagonal in buffer pixels
    double diagonal() const;

    /// normalized -> buffer
    double bufferX(double nx) const
    {
        return nx * imageWidth - offsetX;
    }
    double bufferY(double ny) const
    {
        return ny * imageHeight - offsetY;
    }

    /// buffer -> normalized
    double normX(double bx) const
    {
        return (bx + offsetX) / imageWidth;
    }
    double normY(double by) const
    {
        return (by + offsetY) / imageHeight;
    }
};

namespace maskgeom
{

/// Hermite smoothstep; a degenerate interval (e0 >= e1) behaves like a step at e0.
float smoothstep(float e0, float e1, float x);

/// 1 at (x0, y0), fading to 0 at (x1, y1), constant along the perpendicular direction.
void linearGradient(const procparams::MaskComponent& c, const MaskFrame& frame, array2D<float>& out, bool multiThread);

/// Rotated ellipse; 1 inside the (100 - feather) % core, fading to 0 at the boundary.
void radialGradient(const procparams::MaskComponent& c, const MaskFrame& frame, array2D<float>& out, bool multiThread);

/**
 * Rasterize all strokes of a brush component. Each stroke is rendered into a
 * coverage buffer limited to its bounding box (capsule union of its
 * segments), then accumulated with its flow (or erased).
 */
void brush(const procparams::MaskComponent& c, const MaskFrame& frame, array2D<float>& out, bool multiThread);

/**
 * Apply the strokes of a brush component onto an existing mask, in order:
 * paint strokes add coverage to what is already selected, erase strokes
 * remove it. This is how a brush edits a selection made by other tools
 * (AI, lasso, gradients...). With subtractMode the roles are swapped.
 */
void brushApply(const procparams::MaskComponent& c, const MaskFrame& frame, array2D<float>& mask, bool subtractMode, bool multiThread);

/// Rasterize a single stroke's coverage (0..1, before flow) into 'out', which must be W x H.
void strokeCoverage(const procparams::MaskBrushStroke& stroke, const MaskFrame& frame, array2D<float>& out, bool multiThread);

/// Inclusive pixel rectangle of a buffer; empty when x0 > x1 or y0 > y1.
struct MaskBox {
    int x0 = 0;
    int y0 = 0;
    int x1 = -1;
    int y1 = -1;

    bool empty() const;
    void unite(const MaskBox& other);
};

/// Pixels a stroke (from segment 'firstSegment' on) can touch in a W x H buffer.
MaskBox strokeBox(const procparams::MaskBrushStroke& stroke, const MaskFrame& frame, int W, int H, size_t firstSegment = 0);

/**
 * Like strokeCoverage(), restricted to the segments from 'firstSegment' on
 * (segment i joins points i and i + 1). Coverage is a per pixel maximum over
 * the segments, so a growing stroke can be rasterized incrementally with
 * bit-identical results. Returns the rectangle that may have been touched.
 */
MaskBox strokeCoverageSegments(const procparams::MaskBrushStroke& stroke, const MaskFrame& frame, array2D<float>& out, size_t firstSegment, bool multiThread);

/// out = base painted (or erased) with flow * cov inside 'box' (out may be base). Same arithmetic as brushApply().
void applyStrokeCoverage(const array2D<float>& base, const array2D<float>& cov, const MaskBox& box, float flow, bool erase, array2D<float>& out, bool multiThread);

/// Sets the pixels of 'box' to 0.
void clearBox(array2D<float>& m, const MaskBox& box, bool multiThread);

/**
 * Luminance window. L is the L* channel in RawTherapee's internal scale
 * (0..32768 == 0..100 L*).
 */
void luminanceRange(const procparams::MaskComponent& c, const float* const* L, int W, int H, array2D<float>& out, bool multiThread);

/**
 * Color similarity to the sampled L*a*b* color. L, a, b are RawTherapee's
 * internal Lab planes (L 0..32768, a/b about +-42000).
 */
void colorRange(const procparams::MaskComponent& c, const float* const* L, const float* const* a, const float* const* b, int W, int H, array2D<float>& out, bool multiThread);

/// Per-pixel weight of a color-range component for one L*a*b* triplet (standard units).
float colorRangeWeight(const procparams::MaskComponent& c, float Lstar, float astar, float bstar);

/// Per-pixel weight of a luminance-range component for one L* value (0..100).
float luminanceRangeWeight(const procparams::MaskComponent& c, float Lstar);

/**
 * Rectangular marquee: 1 inside the (optionally rotated and rounded)
 * rectangle spanned by (x0, y0) / (x1, y1), with an anti-aliased edge. The
 * feather fades the outer (feather %) part of the smaller half-extent, like
 * the radial gradient does.
 */
void rectangle(const procparams::MaskComponent& c, const MaskFrame& frame, array2D<float>& out, bool multiThread);

/**
 * Lasso / polygon: anti-aliased scanline fill of the closed polygon c.points
 * (non-zero winding or even-odd). The feather is a symmetric transition of
 * half width polygonFeatherPixels() across the outline, computed from a
 * signed distance transform on a padded window so that a detail crop renders
 * exactly like the full frame.
 */
void polygon(const procparams::MaskComponent& c, const MaskFrame& frame, array2D<float>& out, bool multiThread);

/// Half width (buffer pixels) of a polygon's feather transition.
double polygonFeatherPixels(const procparams::MaskComponent& c, const MaskFrame& frame);

/// Lab planes of a (full frame) image, used by the magic wand for connectivity on detail crops.
struct LabPlanes {
    const float* const* L = nullptr;
    const float* const* a = nullptr;
    const float* const* b = nullptr;
    int W = 0;
    int H = 0;
    MaskFrame frame;
};

/**
 * Magic wand / quick selection. Every seed (x, y, L*, a*, b*) selects the
 * colors within 'tolerance' of its color (see colorRangeWeight). With
 * c.contiguous the selection is restricted to the 4-connected region(s)
 * around the seeds. On a buffer that does not cover the whole image the
 * connectivity through the invisible part is taken from 'reference' (a
 * lower resolution full frame, e.g. the preview) when given; without it the
 * fill is seeded from the buffer border wherever a seed lies outside.
 */
void magicWand(const procparams::MaskComponent& c, const MaskFrame& frame, const float* const* L, const float* const* a, const float* const* b, int W, int H, array2D<float>& out, bool multiThread, const LabPlanes* reference = nullptr);

/// Per-pixel weight of a magic wand component (maximum over its seeds) for one L*a*b* triplet.
float wandWeight(const procparams::MaskComponent& c, float Lstar, float astar, float bstar);

/// Weight of a color against a reference color with the color-range falloff parameters.
float colorWeight(float refL, float refA, float refB, double tolerance, double smoothness, float Lstar, float astar, float bstar);

/**
 * Quick selection (QUICK_SELECT). The cells of a fixed working grid laid over
 * the whole (normalized) image are filled with box averaged Lab values; the
 * brush strokes of c.strokes are the seeds (erase strokes: background seeds).
 * The selection is the soft-thresholded geodesic distance from the add seeds
 * (step cost: image gradient * edge sensitivity + distance to the seed colors),
 * restricted to the cells geodesically closer to the add seeds than to the
 * erase seeds, then brought to the buffer with a joint bilateral upsampling
 * guided by the buffer's colors. A buffer that does not cover the whole image
 * takes its grid from 'reference' (full frame) when given, so preview, detail
 * crop, export and thumbnail agree. When eraseOut is given it receives the
 * region claimed by the erase strokes (to remove it from a mask beneath).
 */
void quickSelect(const procparams::MaskComponent& c, const MaskFrame& frame, const float* const* L, const float* const* a, const float* const* b, int W, int H, array2D<float>& out, bool multiThread, const LabPlanes* reference = nullptr, array2D<float>* eraseOut = nullptr);

/// Size of the quick selection working grid for an image of the frame's aspect ratio.
void quickSelectGridSize(const MaskFrame& frame, int& gw, int& gh);

/**
 * Grayscale dilation (dilate = true) / erosion of the mask with an octagonal
 * structuring element of the given radius in buffer pixels (exact for the
 * decomposition square + diamond, O(N) per pass). Values outside of the
 * buffer are ignored (the border is neither eroded nor dilated).
 */
void morphology(array2D<float>& m, int radiusPx, bool dilate, bool multiThread);

/// Morphological smoothing (opening followed by closing): removes specks and holes smaller than the radius and rounds corners.
void smoothMask(array2D<float>& m, int radiusPx, bool multiThread);

/// Radius in buffer pixels for a layer's expand (> 0) / contract (< 0) amount (-100..100).
int expandRadiusPixels(double expand, const MaskFrame& frame);

/// Radius in buffer pixels for a layer's smooth amount (0..100).
int smoothRadiusPixels(double smooth, const MaskFrame& frame);

/// m = 1 - m
void invert(array2D<float>& m, bool multiThread);

/// m *= factor, clamped to [0, 1]
void scale(array2D<float>& m, float factor, bool multiThread);

/// dst = dst (op) src, per pixel. ADD = union, SUBTRACT = dst * (1 - src), INTERSECT = dst * src.
void combine(procparams::MaskBooleanOp op, array2D<float>& dst, const array2D<float>& src, bool multiThread);

/// Scalar version of combine().
inline float combineValue(procparams::MaskBooleanOp op, float dst, float src)
{
    switch (op) {
        case procparams::MaskBooleanOp::SUBTRACT:
            return dst * (1.f - src);

        case procparams::MaskBooleanOp::INTERSECT:
            return dst * src;

        case procparams::MaskBooleanOp::ADD:
        default:
            return dst + src - dst * src;
    }
}

/// true if any value in m is > 0 (cheap early-out for empty masks)
bool isEmpty(const array2D<float>& m);

}

}
