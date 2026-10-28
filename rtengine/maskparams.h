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
 * Data model of the "Masks" (localized adjustments) tool.
 *
 * This header deliberately depends only on the C++ standard library so that
 * the mask math (maskgeometry.h) can be compiled and unit tested without
 * GTK/Glib. All geometry is stored procedurally and in normalized
 * coordinates of the full, uncropped image as it is before the coarse
 * (90 degree / flip) and the geometric (rotation, distortion, perspective)
 * transforms, so that masks stay attached to the image content when those
 * change (see masktransform.h):
 *
 *   - positions are fractions in [0, 1] of the full image width / height
 *   - radii / brush sizes are fractions of the full image diagonal
 *
 * This keeps masks resolution independent: the same pp3 renders identically
 * on the downscaled preview, on a 1:1 detail crop, in batch export and on the
 * thumbnail.
 */

#include <functional>
#include <string>
#include <vector>

namespace rtengine
{

namespace procparams
{

/// Kind of mask primitive.
enum class MaskComponentType : int {
    BRUSH = 0,
    LINEAR_GRADIENT,
    RADIAL_GRADIENT,
    COLOR_RANGE,
    LUMINANCE_RANGE,
    AI_SUBJECT,
    AI_SKY,
    AI_BACKGROUND,
    AI_OBJECT,
    RECTANGLE,      ///< rectangular marquee (optionally rotated, rounded, feathered)
    POLYGON,        ///< lasso / polygonal lasso: closed polygon
    MAGIC_WAND,     ///< seed based color selection (contiguous flood fill or "similar colors")
    QUICK_SELECT    ///< brushed seeds grown over similar, connected pixels (geodesic region growing)
};

/// Last valid enumerator, for range checks when loading a pp3.
constexpr MaskComponentType MASK_COMPONENT_TYPE_LAST = MaskComponentType::QUICK_SELECT;

/// Boolean operation used to combine a component with the components before it.
enum class MaskBooleanOp : int {
    ADD = 0,      ///< union
    SUBTRACT,     ///< remove
    INTERSECT     ///< keep only the overlap
};

inline bool maskComponentIsAI(MaskComponentType t)
{
    return t == MaskComponentType::AI_SUBJECT || t == MaskComponentType::AI_SKY
           || t == MaskComponentType::AI_BACKGROUND || t == MaskComponentType::AI_OBJECT;
}

/**
 * One brush stroke. A stroke is a polyline painted with a round, feathered
 * brush. The coverage of a stroke is the union of the capsules around each
 * segment; "flow" is the opacity applied once per stroke.
 */
struct MaskBrushStroke {
    double size;                 ///< brush radius, fraction of the image diagonal (0..1]
    double feather;              ///< 0..1, fraction of the radius that fades out
    double flow;                 ///< 0..1 opacity of the stroke
    bool erase;                  ///< true: the stroke removes coverage instead of adding it
    std::vector<double> points;  ///< x0, y0, x1, y1, ... normalized to [0, 1]

    MaskBrushStroke();

    size_t pointCount() const
    {
        return points.size() / 2;
    }

    bool operator ==(const MaskBrushStroke& other) const;
    bool operator !=(const MaskBrushStroke& other) const;
};

/**
 * One mask primitive. Only the fields relevant to 'type' are used, the others
 * keep their defaults. Keeping a single struct (instead of a class hierarchy)
 * makes the pp3 (de)serialization, copying, comparison and the GUI's
 * ParamsEdited handling trivial.
 */
struct MaskComponent {
    MaskComponentType type;
    MaskBooleanOp op;
    bool enabled;
    bool invert;

    // BRUSH
    std::vector<MaskBrushStroke> strokes;

    // LINEAR_GRADIENT: fully selected at (x0, y0), fading to zero at (x1, y1)
    double x0;
    double y0;
    double x1;
    double y1;

    // RADIAL_GRADIENT
    double centerX;   ///< normalized
    double centerY;   ///< normalized
    double radiusX;   ///< fraction of the diagonal
    double radiusY;   ///< fraction of the diagonal
    double angle;     ///< degrees, counter clockwise
    double feather;   ///< 0..100, inner part of the ellipse that is fully selected = (100 - feather) %

    // COLOR_RANGE: reference color in CIE L*a*b* (L* 0..100, a*/b* about -128..128)
    double sampleL;
    double sampleA;
    double sampleB;
    double tolerance;   ///< 0..100, size of the color neighbourhood
    double smoothness;  ///< 0..100, softness of the falloff

    // LUMINANCE_RANGE: L* window
    double lumLow;          ///< 0..100
    double lumHigh;         ///< 0..100
    double lumFeatherLow;   ///< 0..50, L* units below lumLow
    double lumFeatherHigh;  ///< 0..50, L* units above lumHigh

    // AI_*: segmentation provider id ("" = default provider), point hint for AI_OBJECT
    std::string aiProvider;
    std::string aiPrompt;    ///< optional concept text for the provider (e.g. "person", "red car")
    double aiHintX;     ///< normalized, AI_OBJECT only
    double aiHintY;     ///< normalized, AI_OBJECT only
    double aiThreshold; ///< 0..100, confidence threshold
    bool aiRefineEdges; ///< refine the mask edges with a matting model (ViTMatte) in the backend

    // RECTANGLE: corners (x0, y0) / (x1, y1), rotation 'angle' about the center,
    // 'feather' (0..100, inner part of the smaller half-extent that fades out)
    double cornerRadius;   ///< 0..100, corner rounding in percent of the smaller half-extent

    // POLYGON: closed outline, x0, y0, x1, y1, ... normalized; 'feather' 0..100
    // (half width of the transition as a fraction of the diagonal, see maskgeometry.cc)
    std::vector<double> points;
    bool evenOdd;          ///< fill rule: true = even-odd, false = non-zero winding

    // MAGIC_WAND: seeds as (x, y, L*, a*, b*) quintuples, 'tolerance' / 'smoothness' as COLOR_RANGE
    std::vector<double> seeds;
    bool contiguous;       ///< true: flood fill from the seeds, false: all similar colors

    // QUICK_SELECT: the brushed seed paths are kept in 'strokes' (size = brush radius,
    // erase = the stroke marks background that is subtracted from the grown region),
    // 'tolerance' (how far the selection grows) and 'smoothness' (softness of its edge)
    double edgeSensitivity;   ///< 0..100, how strongly image edges stop the growth

    MaskComponent();
    explicit MaskComponent(MaskComponentType t);

    /// number of polygon vertices
    size_t pointCount() const
    {
        return points.size() / 2;
    }

    /// number of magic wand seeds
    size_t seedCount() const
    {
        return seeds.size() / 5;
    }

    bool operator ==(const MaskComponent& other) const;
    bool operator !=(const MaskComponent& other) const;
};

/**
 * A "mask" as the user sees it: an ordered list of components combined with
 * boolean operations, optional feathering / inversion, and the set of
 * adjustments that are applied where the mask is > 0.
 */
struct MaskLayer {
    bool enabled;
    std::string name;
    bool invert;
    double expand;    ///< -100..100, morphological contract (< 0) / expand (> 0) of the combined mask
    double smooth;    ///< 0..100, morphological smoothing (removes specks and holes, rounds corners)
    double feather;   ///< 0..100, extra blur applied to the combined mask
    double amount;    ///< 0..100, global opacity of the mask
    std::vector<MaskComponent> components;

    // adjustments
    double exposure;    ///< EV, -5..5
    double contrast;    ///< -100..100
    double highlights;  ///< -100..100
    double midtones;    ///< -100..100
    double shadows;     ///< -100..100
    double whites;      ///< -100..100
    double blacks;      ///< -100..100
    double temperature; ///< -100..100 (negative = cooler, positive = warmer)
    double tint;        ///< -100..100 (negative = green, positive = magenta)
    double hue;         ///< degrees, -180..180
    double saturation;  ///< -100..100
    double clarity;     ///< -100..100

    // L*a*b* curves, same encoding and meaning as those of the L*a*b*
    // Adjustments tool (LCurveParams); {0} = linear (DCT_Linear / FCT_Linear)
    std::vector<double> lcurve;    ///< L = f(L), diagonal
    std::vector<double> acurve;    ///< a = f(a), diagonal
    std::vector<double> bcurve;    ///< b = f(b), diagonal
    std::vector<double> cccurve;   ///< C = f(C), diagonal
    std::vector<double> chcurve;   ///< C = f(H), flat
    std::vector<double> lhcurve;   ///< L = f(H), flat
    std::vector<double> hhcurve;   ///< H = f(H), flat
    std::vector<double> lccurve;   ///< L = f(C), diagonal
    std::vector<double> clcurve;   ///< C = f(L), diagonal

    MaskLayer();

    /// true if any of the curves is not of the linear type
    bool hasCurves() const;
    /// true if any adjustment differs from its neutral value
    bool hasAdjustments() const;
    /// sets every adjustment to its neutral value (the mask itself is kept)
    void resetAdjustments();

    bool operator ==(const MaskLayer& other) const;
    bool operator !=(const MaskLayer& other) const;
};

/**
 * Re-expresses every normalized coordinate / size of the given masks relative
 * to a crop rectangle of the image. Used when the image is cropped before the
 * masks are evaluated (fast batch export pipeline).
 */
void remapMasksToCrop(std::vector<MaskLayer>& masks, double cropX, double cropY, double cropW, double cropH, double fullW, double fullH);

/// Maps normalized points of one frame to another, in place; 'xy' holds
/// interleaved x, y pairs.
using MaskPointMap = std::function<void(std::vector<double>& xy)>;

/**
 * Re-expresses the geometry of a component in another frame: positions go
 * through 'map'; sizes and angles (brush radius, ellipse / rectangle axes,
 * polygon feather) follow the local Jacobian of the map. srcW/srcH and
 * dstW/dstH are the pixel sizes of the two normalized frames (any common
 * scale).
 */
void transformMaskComponent(MaskComponent& c, const MaskPointMap& map, double srcW, double srcH, double dstW, double dstH);
void transformMasks(std::vector<MaskLayer>& masks, const MaskPointMap& map, double srcW, double srcH, double dstW, double dstH);

}

}
