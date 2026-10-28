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

/**
 * Masks tool: builds the per-pixel alpha of every mask layer from its
 * components (brush, gradients, ranges, AI segmentation) and applies the
 * layer's adjustments where alpha > 0.
 *
 * Runs in the Lab stage of the pipeline, right after the L*a*b* color
 * correction regions, in all four processing paths (preview, detail crop,
 * batch export and thumbnail). See maskgeometry.h for the coordinate model.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "aisegmentation.h"
#include "array2D.h"
#include "color.h"
#include "curves.h"
#include "gauss.h"
#include "iccstore.h"
#include "improcfun.h"
#include "labimage.h"
#include "maskcache.h"
#include "maskgeometry.h"
#include "masktransform.h"
#include "procparams.h"
#include "settings.h"
#include "rt_math.h"
#include "sleef.h"

//#define BENCHMARK
#include "StopWatch.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>

// Diagnostics (environment variables, read once):
//  RT_MASK_TIMING=1   prints the time of every stage of the Masks tool to stderr
//  RT_MASK_NOCACHE=1  disables the MaskingCache of the interactive pipelines (for A/B checks: the
//                     cached results must be bit-identical)
namespace {
inline bool maskProf() { static const bool on = std::getenv("RT_MASK_TIMING") != nullptr; return on; }
inline bool maskCacheDisabled() { static const bool off = std::getenv("RT_MASK_NOCACHE") != nullptr; return off; }
struct MProf {
    const char* what; int idx; std::chrono::steady_clock::time_point t0;
    MProf(const char* w, int i = -1) : what(w), idx(i), t0(std::chrono::steady_clock::now()) {}
    ~MProf() { if (maskProf()) { const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); std::fprintf(stderr, "[mask] %-22s %3d %8.2f ms\n", what, idx, ms); } }
};
}

namespace rtengine
{

using procparams::MaskComponent;
using procparams::MaskComponentType;
using procparams::MaskBooleanOp;
using procparams::MaskLayer;

namespace
{

constexpr float MAX_AB = 42000.f;

/// Rasterize a single component into 'out' (W x H, values 0..1). Returns false if nothing could be produced.
bool evaluateComponent(const MaskComponent& c, const MaskFrame& frame, const LabImage* lab, SegmentationPolicy policy, const maskgeom::LabPlanes* reference, array2D<float>& out, bool multiThread)
{
    switch (c.type) {
        case MaskComponentType::RECTANGLE:
            maskgeom::rectangle(c, frame, out, multiThread);
            return true;

        case MaskComponentType::POLYGON:
            maskgeom::polygon(c, frame, out, multiThread);
            return true;

        case MaskComponentType::MAGIC_WAND:
            maskgeom::magicWand(c, frame, lab->L, lab->a, lab->b, lab->W, lab->H, out, multiThread, reference);
            return true;

        case MaskComponentType::QUICK_SELECT:
            maskgeom::quickSelect(c, frame, lab->L, lab->a, lab->b, lab->W, lab->H, out, multiThread, reference);
            return true;

        case MaskComponentType::BRUSH:
            maskgeom::brush(c, frame, out, multiThread);
            return true;

        case MaskComponentType::LINEAR_GRADIENT:
            maskgeom::linearGradient(c, frame, out, multiThread);
            return true;

        case MaskComponentType::RADIAL_GRADIENT:
            maskgeom::radialGradient(c, frame, out, multiThread);
            return true;

        case MaskComponentType::COLOR_RANGE:
            maskgeom::colorRange(c, lab->L, lab->a, lab->b, lab->W, lab->H, out, multiThread);
            return true;

        case MaskComponentType::LUMINANCE_RANGE:
            maskgeom::luminanceRange(c, lab->L, lab->W, lab->H, out, multiThread);
            return true;

        case MaskComponentType::AI_SUBJECT:
        case MaskComponentType::AI_SKY:
        case MaskComponentType::AI_BACKGROUND:
        case MaskComponentType::AI_OBJECT: {
            auto& registry = AISegmentationRegistry::getInstance();
            AISegmentationProvider* provider = registry.getProvider(c.aiProvider);

            if (!provider) {
                // No usable model (backend disabled, Python/torch missing, ...): empty mask, reason for the GUI.
                if (registry.getLastError().empty()) {
                    registry.setLastError("no AI segmentation model is available; check the Python backend (see rtdata/segmentation/README.md)");
                }

                out.fill(0.f, multiThread);
                return false;
            }

            if (!provider->isAvailable()) {
                registry.setLastError(provider->displayName() + ": " + provider->availabilityReason());
                out.fill(0.f, multiThread);
                return false;
            }

            if (!provider->supports(c.type)) {
                out.fill(0.f, multiThread);
                return false;
            }

            SegmentationRequest req;
            req.L = lab->L;
            req.a = lab->a;
            req.b = lab->b;
            req.width = lab->W;
            req.height = lab->H;
            req.frame = frame;
            req.target = c.type;
            req.hintX = c.aiHintX;
            req.hintY = c.aiHintY;
            req.prompt = c.aiPrompt;
            req.threshold = 0.5;
            req.refineEdges = c.aiRefineEdges;
            req.policy = policy;
            req.multiThread = multiThread;

            if (!provider->segment(req, out)) {
                out.fill(0.f, multiThread);
                return false;
            }

            // Soft confidence threshold.
            const float t = LIM01(float(c.aiThreshold / 100.0));
            const float lo = std::max(t - 0.2f, 0.f);
            const float hi = std::min(t + 0.2f, 1.f);

#ifdef _OPENMP
            #pragma omp parallel for if (multiThread)
#endif
            for (int y = 0; y < lab->H; ++y) {
                float* row = out[y];

                for (int x = 0; x < lab->W; ++x) {
                    row[x] = maskgeom::smoothstep(lo, hi, row[x]);
                }
            }

            return true;
        }
    }

    out.fill(0.f, multiThread);
    return false;
}

inline bool isPaintBrush(const MaskComponent& c)
{
    return c.type == MaskComponentType::BRUSH && c.op != MaskBooleanOp::INTERSECT;
}

/// Quick selection in Add mode with erase strokes: edits the selection built so far (see applyComponent).
inline bool isQuickSelectEditing(const MaskComponent& c)
{
    return c.type == MaskComponentType::QUICK_SELECT && c.op == MaskBooleanOp::ADD && !c.invert
           && std::any_of(c.strokes.begin(), c.strokes.end(), [](const procparams::MaskBrushStroke& s) { return s.erase; });
}

/**
 * Applies one enabled component of a layer onto 'mask', the selection built
 * by the components before it ('first': no enabled component applied yet).
 * 'comp' is a W x H scratch buffer; when 'evaluated' is true it already holds
 * the rasterized component.
 */
void applyComponent(const MaskComponent& c, const MaskFrame& frame, const LabImage* lab, SegmentationPolicy policy, const maskgeom::LabPlanes* reference, array2D<float>& mask, array2D<float>& comp, bool evaluated, bool& first, bool multiThread)
{
    const int W = lab->W;
    const int H = lab->H;

    if (isPaintBrush(c)) {
        // The brush edits the selection built so far: paint adds, erase removes
        // (reversed in Subtract mode). On an empty selection this is plain painting.
        MProf mp_("brushApply", int(c.strokes.size()));
        maskgeom::brushApply(c, frame, mask, c.op == MaskBooleanOp::SUBTRACT, multiThread);
        first = false;
        return;
    }

    if (isQuickSelectEditing(c)) {
        // Quick selection in Add mode: the grown region is added, and the region
        // claimed by its erase strokes is removed from the selection built so far
        // (like the brush's eraser, it edits the whole mask).
        MProf mp_("quickSelect", int(c.strokes.size()));
        array2D<float> erase(W, H);
        maskgeom::quickSelect(c, frame, lab->L, lab->a, lab->b, W, H, comp, multiThread, reference, &erase);
        maskgeom::combine(MaskBooleanOp::ADD, mask, comp, multiThread);
        maskgeom::combine(MaskBooleanOp::SUBTRACT, mask, erase, multiThread);
        first = false;
        return;
    }

    if (!evaluated) {
        MProf mp_("comp", int(c.type));
        evaluateComponent(c, frame, lab, policy, reference, comp, multiThread);
    }

    if (c.invert) {
        maskgeom::invert(comp, multiThread);
    }

    if (first) {
        // The first component always defines the initial selection.
        maskgeom::combine(MaskBooleanOp::ADD, mask, comp, multiThread);
        first = false;
    } else {
        maskgeom::combine(c.op, mask, comp, multiThread);
    }
}

/**
 * Layer refinements, in a fixed order: expand / contract, smooth, feather,
 * then invert and amount. 'first' means that no component was enabled.
 * 'scratch' (optional) avoids an allocation for the feather.
 */
void refineLayerMask(const MaskLayer& layer, const MaskFrame& frame, array2D<float>& mask, bool first, bool multiThread, array2D<float>* scratch = nullptr)
{
    const int W = mask.getWidth();
    const int H = mask.getHeight();

    if (first) {
        // No enabled component: empty selection, or everything when inverted.
        if (layer.invert) {
            mask.fill(float(LIM01(layer.amount / 100.0)), multiThread);
        }

        return;
    }

    MProf mpr_("refinements");

    if (layer.expand != 0.0) {
        maskgeom::morphology(mask, maskgeom::expandRadiusPixels(layer.expand, frame), layer.expand > 0.0, multiThread);
    }

    if (layer.smooth > 0.0) {
        maskgeom::smoothMask(mask, maskgeom::smoothRadiusPixels(layer.smooth, frame), multiThread);
    }

    // Layer feather: gaussian blur, radius relative to the image diagonal.
    if (layer.feather > 0.0) {
        const double sigma = std::max(layer.feather / 100.0 * 0.03 * frame.diagonal(), 0.5);
        array2D<float> local;

        if (!scratch || scratch->getWidth() != W || scratch->getHeight() != H) {
            local(W, H);
            scratch = &local;
        }

        array2D<float>& blurred = *scratch;
#ifdef _OPENMP
        #pragma omp parallel if (multiThread)
#endif
        gaussianBlur(mask, blurred, W, H, sigma);

#ifdef _OPENMP
        #pragma omp parallel for if (multiThread)
#endif
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                mask[y][x] = LIM01(blurred[y][x]);
            }
        }
    }

    if (layer.invert) {
        maskgeom::invert(mask, multiThread);
    }

    if (layer.amount != 100.0) {
        maskgeom::scale(mask, float(layer.amount / 100.0), multiThread);
    }
}

/**
 * Combine all (enabled) components of a layer into 'mask'. When
 * onlyComponent >= 0, just that component is rendered (used by the overlay).
 */
void buildLayerMask(const MaskLayer& layer, int onlyComponent, const MaskFrame& frame, const LabImage* lab, SegmentationPolicy policy, const maskgeom::LabPlanes* reference, array2D<float>& mask, bool multiThread)
{
    const int W = lab->W;
    const int H = lab->H;

    mask.fill(0.f, multiThread);

    if (onlyComponent >= 0 && onlyComponent < int(layer.components.size())) {
        const MaskComponent& c = layer.components[onlyComponent];
        evaluateComponent(c, frame, lab, policy, reference, mask, multiThread);

        if (c.invert) {
            maskgeom::invert(mask, multiThread);
        }

        return;
    }

    MProf mpl_("buildLayerMask");
    array2D<float> comp(W, H);
    bool first = true;

    for (const MaskComponent& c : layer.components) {
        if (c.enabled) {
            applyComponent(c, frame, lab, policy, reference, mask, comp, false, first, multiThread);
        }
    }

    refineLayerMask(layer, frame, mask, first, multiThread);
}

// ---------------------------------------------------------------------------
// Cache of the interactive pipelines (see maskcache.h)

using MaskBuf = std::unique_ptr<array2D<float>>;

array2D<float>& ensureBuf(MaskBuf& b, int W, int H)
{
    if (!b) {
        b.reset(new array2D<float>(W, H));
    } else if (b->getWidth() != W || b->getHeight() != H) {
        (*b)(W, H);
    }

    return *b;
}

size_t bufBytes(const MaskBuf& b)
{
    return b ? size_t(b->getWidth()) * b->getHeight() * sizeof(float) : 0;
}

void copyBuf(const array2D<float>& src, array2D<float>& dst, bool multiThread)
{
    const int W = src.getWidth();
    const int H = src.getHeight();

#ifdef _OPENMP
    #pragma omp parallel for if (multiThread)
#endif
    for (int y = 0; y < H; ++y) {
        std::memcpy(dst[y], src[y], sizeof(float) * W);
    }
}

size_t labBytes(const std::unique_ptr<LabImage>& l)
{
    return l ? size_t(l->W) * l->H * 3 * sizeof(float) : 0;
}

/// true if both images have the same size and bit-identical content.
bool sameLab(const LabImage* a, const LabImage* b, bool multiThread)
{
    if (!a || !b || a->W != b->W || a->H != b->H) {
        return false;
    }

    const int W = a->W;
    const int H = a->H;
    bool same = true;

#ifdef _OPENMP
    #pragma omp parallel for if (multiThread)
#endif
    for (int y = 0; y < H; ++y) {
        if (!same) {
            continue;
        }

        if (std::memcmp(a->L[y], b->L[y], sizeof(float) * W) || std::memcmp(a->a[y], b->a[y], sizeof(float) * W) || std::memcmp(a->b[y], b->b[y], sizeof(float) * W)) {
            same = false;
        }
    }

    return same;
}

void copyLab(const LabImage* src, std::unique_ptr<LabImage>& dst, bool multiThread)
{
    if (!dst || dst->W != src->W || dst->H != src->H) {
        dst.reset(new LabImage(src->W, src->H, false, multiThread));
    }

    dst->CopyFrom(src, multiThread);
}

/// What, besides its parameters and the buffer geometry, a component's raster depends on.
enum class Dependency {
    GEOMETRY,   ///< nothing
    INPUT,      ///< the Lab input of the stage
    INPUT_REF,  ///< the Lab input and the full frame reference
    OUTPUT      ///< unknown (AI providers, future types): compare the rasterized result itself
};

Dependency dependencyOf(MaskComponentType t)
{
    switch (t) {
        case MaskComponentType::BRUSH:
        case MaskComponentType::LINEAR_GRADIENT:
        case MaskComponentType::RADIAL_GRADIENT:
        case MaskComponentType::RECTANGLE:
        case MaskComponentType::POLYGON:
            return Dependency::GEOMETRY;

        case MaskComponentType::COLOR_RANGE:
        case MaskComponentType::LUMINANCE_RANGE:
            return Dependency::INPUT;

        case MaskComponentType::MAGIC_WAND:
        case MaskComponentType::QUICK_SELECT:
            return Dependency::INPUT_REF;

        default:
            return Dependency::OUTPUT;
    }
}

/// 64 bit FNV-1a hash of the bits of a buffer (never 0).
uint64_t hashBuf(const array2D<float>& m, bool multiThread)
{
    const int W = m.getWidth();
    const int H = m.getHeight();
    std::vector<uint64_t> rows(H);

#ifdef _OPENMP
    #pragma omp parallel for if (multiThread)
#endif
    for (int y = 0; y < H; ++y) {
        uint64_t h = 1469598103934665603ULL;
        const unsigned char* p = reinterpret_cast<const unsigned char*>(m[y]);

        for (size_t i = 0; i < sizeof(float) * W; ++i) {
            h = (h ^ p[i]) * 1099511628211ULL;
        }

        rows[y] = h;
    }

    uint64_t h = 1469598103934665603ULL ^ (uint64_t(W) << 32) ^ uint64_t(H);

    for (const uint64_t r : rows) {
        h = (h ^ r) * 1099511628211ULL;
    }

    return h ? h : 1;
}

/// The part of a layer that defines its mask (adjustments neutralized).
MaskLayer maskDefinition(const MaskLayer& l)
{
    MaskLayer d = l;
    d.enabled = true;
    d.name.clear();
    d.resetAdjustments();
    return d;
}

/// The part of a layer that does not define its mask (everything else).
MaskLayer adjustmentPart(const MaskLayer& l)
{
    MaskLayer d = l;
    d.enabled = true;
    d.name.clear();
    d.components.clear();
    d.invert = false;
    d.expand = 0.0;
    d.smooth = 0.0;
    d.feather = 0.0;
    d.amount = 100.0;
    return d;
}

struct LayerCache {
    bool valid = false;                 ///< 'final' is the mask of 'def' / 'sigs'
    MaskLayer def;                      ///< mask definition of the last build
    std::vector<uint64_t> sigs;         ///< data signature of each component of the last build
    MaskBuf final;
    bool finalEmpty = true;
    uint64_t finalVersion = 0;          ///< incremented whenever 'final' changes
    MaskLayer lastAdjust;               ///< adjustments of the last run
    bool adjustKnown = false;
    uint64_t adjustVersion = 0;         ///< incremented whenever the adjustments change

    int snapIndex = -1;                 ///< 'snapshot' = selection before component snapIndex (== count: before the refinements)
    bool snapFirst = true;              ///< no enabled component before snapIndex
    MaskBuf snapshot;

    bool brushValid = false;            ///< base / cov / after describe the paint brush at snapIndex applied onto 'snapshot'
    MaskBuf base;                       ///< selection before its last stroke
    MaskBuf cov;                        ///< coverage of its last stroke (0 outside covBox)
    MaskBuf after;                      ///< selection after the brush
    maskgeom::MaskBox covBox;

    struct Raster {                     ///< raw raster of an unchanged component re-applied after the edited one
        size_t index = 0;
        MaskComponent comp;
        uint64_t sig = 0;
        MaskBuf buf;
    };
    static constexpr size_t MAX_RASTERS = 4;
    std::vector<Raster> rasters;

    uint64_t lastUse = 0;

    void dropIncremental()
    {
        snapIndex = -1;
        snapFirst = true;
        snapshot.reset();
        brushValid = false;
        base.reset();
        cov.reset();
        after.reset();
        covBox = maskgeom::MaskBox();
        rasters.clear();
    }

    void reset()
    {
        valid = false;
        def = MaskLayer();
        sigs.clear();
        final.reset();
        finalEmpty = true;
        ++finalVersion;
        dropIncremental();
    }

    size_t incrementalBytes() const
    {
        size_t b = bufBytes(snapshot) + bufBytes(base) + bufBytes(cov) + bufBytes(after);

        for (const auto& r : rasters) {
            b += bufBytes(r.buf);
        }

        return b;
    }

    size_t bytes() const
    {
        return bufBytes(final) + incrementalBytes();
    }
};

}   // namespace

struct MaskingCache::Data {
    int W = 0;
    int H = 0;
    MaskFrame frame;

    std::unique_ptr<LabImage> input;            ///< input of the last run
    uint64_t inputGen = 0;
    const LabImage* restoredFor = nullptr;      ///< restoreInput() target, consumed by the next run
    const LabImage* lastSample = nullptr;       ///< sampleSnapshot written by the last run

    std::unique_ptr<LabImage> ref;              ///< full frame reference of the last run
    bool hadRef = false;
    uint64_t refGen = 0;

    std::vector<LayerCache> layers;             ///< by layer index
    LayerCache overlay;                         ///< "this component only" overlay of a brush
    MaskBuf work;                               ///< scratch (also: previous final of the layer just rebuilt)
    MaskBuf comp;                               ///< scratch
    MaskBuf scratch;                            ///< scratch (feather)

    bool outputValid = false;                   ///< 'output' is the result of the last run (adjusted, not overlay)
    std::unique_ptr<LabImage> output;
    std::vector<MaskLayer> outAdjust;
    std::vector<char> outActive;
    Glib::ustring outProfile;
    double outScale = 0.0;
    MaskBuf blurredL;                           ///< clarity base of the last full run
    bool blurredValid = false;

    uint64_t clock = 0;

    void reset(int w, int h, const MaskFrame& f)
    {
        W = w;
        H = h;
        frame = f;
        input.reset();
        restoredFor = nullptr;
        lastSample = nullptr;
        ref.reset();
        hadRef = false;
        ++inputGen;
        ++refGen;
        layers.clear();
        overlay.reset();
        work.reset();
        comp.reset();
        scratch.reset();
        outputValid = false;
        output.reset();
        outAdjust.clear();
        outActive.clear();
        blurredL.reset();
        blurredValid = false;
    }

    size_t bytes() const
    {
        size_t b = labBytes(input) + labBytes(ref) + labBytes(output) + bufBytes(work) + bufBytes(comp) + bufBytes(scratch) + bufBytes(blurredL) + overlay.bytes();

        for (const auto& l : layers) {
            b += l.bytes();
        }

        return b;
    }

    /// Keeps the memory below 'budget': drops the output cache, then the incremental state and the masks of the least recently used layers.
    void trim(size_t budget)
    {
        if (bytes() <= budget) {
            return;
        }

        overlay.reset();
        scratch.reset();

        if (bytes() <= budget) {
            return;
        }

        output.reset();
        outputValid = false;
        blurredL.reset();
        blurredValid = false;

        while (bytes() > budget) {
            LayerCache* victim = nullptr;

            for (auto& l : layers) {
                if ((l.final || l.incrementalBytes()) && (!victim || l.lastUse < victim->lastUse)) {
                    victim = &l;
                }
            }

            if (!victim) {
                break;
            }

            victim->reset();
        }
    }
};

namespace
{

constexpr size_t MASKING_CACHE_BUDGET = size_t(768) << 20;

enum class BuildResult {
    UNCHANGED,      ///< E.final is still valid
    REBUILT,        ///< E.final rebuilt, the previous one is in D.work
    NEW,            ///< E.final built, no previous mask
    NEEDS_LAB       ///< nothing done: the build would read the Lab image, which is not available
};

inline uint64_t mixSig(uint64_t h, uint64_t v)
{
    return (h ^ v) * 1099511628211ULL + 0x9e3779b97f4a7c15ULL;
}

/**
 * The paint brush at component k of the layer, applied onto 'work' (the
 * selection before it, equal to E.snapshot). Incremental when only the last
 * stroke changed or a stroke was added since the previous build.
 */
void brushStep(MaskingCache::Data& D, LayerCache& E, const MaskComponent& c, size_t k, const MaskFrame& frame, array2D<float>& work, bool multiThread)
{
    const int W = work.getWidth();
    const int H = work.getHeight();
    const bool subtract = c.op == MaskBooleanOp::SUBTRACT;
    const MaskComponent* old = nullptr;

    // Cached rasters are only valid for the buffer size they were made for
    // (a detail window can change size between runs).
    if (E.base && (E.base->getWidth() != W || E.base->getHeight() != H)) {
        E.brushValid = false;
        E.covBox = maskgeom::MaskBox();
    }

    if (E.brushValid && E.base && E.cov && E.after && k < E.def.components.size()) {
        old = &E.def.components[k];

        if (!isPaintBrush(*old) || old->op != c.op || !old->enabled) {
            old = nullptr;
        }
    }

    array2D<float>& base = ensureBuf(E.base, W, H);
    array2D<float>& cov = ensureBuf(E.cov, W, H);
    array2D<float>& after = ensureBuf(E.after, W, H);
    const size_t ns = c.strokes.size();
    bool done = false;

    if (old && ns > 0) {
        const size_t os = old->strokes.size();
        const auto& last = c.strokes.back();
        const float flow = LIM01(float(last.flow));
        const bool erase = last.erase != subtract;

        if (ns == os && std::equal(c.strokes.begin(), c.strokes.end() - 1, old->strokes.begin())) {
            // Only the last stroke changed (the stroke being painted).
            const auto& ol = old->strokes.back();
            const size_t op = ol.pointCount();
            const bool sameBrush = last.size == ol.size && last.feather == ol.feather && last.flow == ol.flow && last.erase == ol.erase;

            if (sameBrush && op >= 2 && last.points.size() >= ol.points.size() && std::equal(ol.points.begin(), ol.points.end(), last.points.begin())) {
                // grown: rasterize the new segments only
                const maskgeom::MaskBox box = maskgeom::strokeCoverageSegments(last, frame, cov, op - 1, multiThread);
                maskgeom::applyStrokeCoverage(base, cov, box, flow, erase, after, multiThread);
                E.covBox.unite(box);
            } else {
                // redraw the last stroke
                const maskgeom::MaskBox oldBox = E.covBox;
                maskgeom::clearBox(cov, oldBox, multiThread);
                maskgeom::MaskBox box = maskgeom::strokeCoverageSegments(last, frame, cov, 0, multiThread);
                maskgeom::MaskBox both = oldBox;
                both.unite(box);
                maskgeom::applyStrokeCoverage(base, cov, both, flow, erase, after, multiThread);
                E.covBox = box;
            }

            done = true;
        } else if (ns == os + 1 && std::equal(old->strokes.begin(), old->strokes.end(), c.strokes.begin())) {
            // A new stroke: the brush result so far is its base.
            copyBuf(after, base, multiThread);
            maskgeom::clearBox(cov, E.covBox, multiThread);
            const maskgeom::MaskBox box = maskgeom::strokeCoverageSegments(last, frame, cov, 0, multiThread);
            maskgeom::applyStrokeCoverage(base, cov, box, flow, erase, after, multiThread);
            E.covBox = box;
            done = true;
        }
    }

    if (!done) {
        // All strokes, in order (same arithmetic as maskgeom::brushApply), keeping the last one apart.
        MProf mp_("brushFull", int(ns));
        copyBuf(work, after, multiThread);
        cov.fill(0.f, multiThread);

        for (size_t s = 0; s + 1 < ns; ++s) {
            const auto& stroke = c.strokes[s];
            const float flow = LIM01(float(stroke.flow));

            if (stroke.pointCount() == 0 || flow <= 0.f) {
                continue;
            }

            const maskgeom::MaskBox box = maskgeom::strokeBox(stroke, frame, W, H);

            if (box.empty()) {
                continue;
            }

            maskgeom::clearBox(cov, box, multiThread);
            maskgeom::strokeCoverageSegments(stroke, frame, cov, 0, multiThread);
            maskgeom::applyStrokeCoverage(after, cov, box, flow, stroke.erase != subtract, after, multiThread);
            maskgeom::clearBox(cov, box, multiThread);
        }

        copyBuf(after, base, multiThread);
        E.covBox = maskgeom::MaskBox();

        if (ns > 0) {
            const auto& last = c.strokes.back();
            E.covBox = maskgeom::strokeCoverageSegments(last, frame, cov, 0, multiThread);
            maskgeom::applyStrokeCoverage(base, cov, E.covBox, LIM01(float(last.flow)), last.erase != subtract, after, multiThread);
        }
    }

    E.brushValid = true;
    copyBuf(after, work, multiThread);
}

/**
 * Cached equivalent of buildLayerMask() (without onlyComponent): leaves the
 * mask of 'layer' in E.final. Bit-identical to buildLayerMask().
 */
/**
 * 'labSig' identifies the content of 'lab' (the stage input with the
 * adjustments of the layers before this one applied); when 'labAvailable' is
 * false, 'lab' does not hold that image and the build returns NEEDS_LAB
 * (without touching anything) if it would have to read it.
 */
BuildResult buildLayerMaskCached(MaskingCache::Data& D, LayerCache& E, const MaskLayer& layer, const MaskFrame& frame, const LabImage* lab, uint64_t labSig, bool labAvailable, SegmentationPolicy policy, const maskgeom::LabPlanes* reference, bool multiThread)
{
    const int W = lab->W;
    const int H = lab->H;
    const MaskLayer def = maskDefinition(layer);
    const auto& comps = def.components;
    const size_t n = comps.size();
    const bool known = E.valid && E.final && E.final->getWidth() == W && E.final->getHeight() == H;
    E.lastUse = ++D.clock;

    const uint64_t inputSig = labSig;
    const uint64_t refSig = mixSig(labSig, D.refGen);
    bool missingLab = false;

    // Signature of each component, and the first one (k0) that differs from the last build.
    std::vector<uint64_t> sigs(n, 0);
    std::vector<MaskBuf> evaluated(n);   // rasterized OUTPUT components, reused below

    auto signature = [&](size_t k) -> uint64_t {
        const MaskComponent& c = comps[k];

        if (!c.enabled) {
            return 0;
        }

        switch (dependencyOf(c.type)) {
            case Dependency::GEOMETRY:
                return 0;

            case Dependency::INPUT:
                return inputSig;

            case Dependency::INPUT_REF:
                return refSig;

            default: {
                if (!labAvailable) {
                    missingLab = true;
                    return 0;
                }

                if (!evaluated[k]) {
                    evaluated[k].reset(new array2D<float>(W, H));
                    MProf mp_("comp(hashed)", int(c.type));
                    evaluateComponent(c, frame, lab, policy, reference, *evaluated[k], multiThread);
                }

                return hashBuf(*evaluated[k], multiThread);
            }
        }
    };

    size_t k0 = 0;

    if (known) {
        for (; k0 < n; ++k0) {
            if (k0 >= E.def.components.size() || !(comps[k0] == E.def.components[k0])) {
                break;
            }

            sigs[k0] = signature(k0);

            if (missingLab) {
                return BuildResult::NEEDS_LAB;
            }

            if (sigs[k0] != E.sigs[k0]) {
                break;
            }
        }

        if (k0 == n && def == E.def) {
            return BuildResult::UNCHANGED;
        }
    }

    const bool fromSnapshot = known && E.snapshot && E.snapIndex >= 0 && size_t(E.snapIndex) <= k0;

    if (!labAvailable) {
        // will any component to (re)apply read the image?
        for (size_t k = fromSnapshot ? E.snapIndex : 0; k < n; ++k) {
            if (comps[k].enabled && dependencyOf(comps[k].type) != Dependency::GEOMETRY) {
                return BuildResult::NEEDS_LAB;
            }
        }
    }

    MProf mpl_("buildLayerMask(cached)", int(k0));
    array2D<float>& work = ensureBuf(D.work, W, H);
    size_t start = 0;
    bool first = true;

    if (fromSnapshot) {
        start = E.snapIndex;
        first = E.snapFirst;
        copyBuf(*E.snapshot, work, multiThread);
    } else {
        E.dropIncremental();
        work.fill(0.f, multiThread);
    }

    for (size_t k = start; k <= n; ++k) {
        if (k == k0 && E.snapIndex != int(k)) {
            // remember the selection before the edited component (or before the refinements)
            copyBuf(work, ensureBuf(E.snapshot, W, H), multiThread);
            E.snapIndex = k;
            E.snapFirst = first;
            E.brushValid = false;
        }

        if (k == n) {
            break;
        }

        const MaskComponent& c = comps[k];

        if (!c.enabled) {
            sigs[k] = 0;
            continue;
        }

        if (int(k) == E.snapIndex && k != k0 && E.brushValid && E.after && isPaintBrush(c) && k < E.def.components.size() && c == E.def.components[k]) {
            // unchanged brush applied onto the unchanged snapshot: its cached result
            copyBuf(*E.after, work, multiThread);
            first = false;
            sigs[k] = 0;
            continue;
        }

        if (k == k0 && isPaintBrush(c)) {
            brushStep(D, E, c, k, frame, work, multiThread);
            first = false;
            sigs[k] = 0;
            continue;
        }

        if (int(k) == E.snapIndex) {
            E.brushValid = false;
        }

        const Dependency dep = dependencyOf(c.type);
        sigs[k] = signature(k);

        if (dep == Dependency::OUTPUT) {
            applyComponent(c, frame, lab, policy, reference, work, *evaluated[k], true, first, multiThread);
        } else if (k != k0 && !isPaintBrush(c) && !isQuickSelectEditing(c)) {
            // an unchanged component re-applied after the edited one: keep its raster
            array2D<float>& comp = ensureBuf(D.comp, W, H);
            LayerCache::Raster* cached = nullptr;

            for (auto& r : E.rasters) {
                if (r.index == k) {
                    cached = &r;
                }
            }

            if (cached && cached->sig == sigs[k] && cached->comp == c && cached->buf) {
                copyBuf(*cached->buf, comp, multiThread);
            } else {
                MProf mp_("comp(kept)", int(c.type));
                evaluateComponent(c, frame, lab, policy, reference, comp, multiThread);

                if (!cached) {
                    if (E.rasters.size() >= LayerCache::MAX_RASTERS) {
                        E.rasters.erase(E.rasters.begin());
                    }

                    E.rasters.emplace_back();
                    cached = &E.rasters.back();
                }

                cached->index = k;
                cached->comp = c;
                cached->sig = sigs[k];
                copyBuf(comp, ensureBuf(cached->buf, W, H), multiThread);
            }

            applyComponent(c, frame, lab, policy, reference, work, comp, true, first, multiThread);
        } else {
            applyComponent(c, frame, lab, policy, reference, work, ensureBuf(D.comp, W, H), false, first, multiThread);
        }
    }

    refineLayerMask(def, frame, work, first, multiThread, &ensureBuf(D.scratch, W, H));

    // new mask -> E.final, previous one -> D.work
    E.final.swap(D.work);
    ++E.finalVersion;
    E.def = def;
    E.sigs = sigs;
    E.valid = true;
    // Kept with the mask itself: a layer first built by an overlay run is
    // reused as UNCHANGED by the following runs, which never recompute it.
    E.finalEmpty = maskgeom::isEmpty(*E.final);
    return known && D.work ? BuildResult::REBUILT : BuildResult::NEW;
}

/// Extends [x0[y], x1[y]] to every pixel where a and b differ; true if they differ anywhere.
bool unionDiff(const array2D<float>& a, const array2D<float>& b, std::vector<int>& x0, std::vector<int>& x1, bool multiThread)
{
    const int W = a.getWidth();
    const int H = a.getHeight();
    int any = 0;

#ifdef _OPENMP
    #pragma omp parallel for reduction(|:any) if (multiThread)
#endif
    for (int y = 0; y < H; ++y) {
        const float* ra = a[y];
        const float* rb = b[y];

        if (!std::memcmp(ra, rb, sizeof(float) * W)) {
            continue;
        }

        int f = 0;

        while (f < W && !std::memcmp(ra + f, rb + f, sizeof(float))) {
            ++f;
        }

        int l = W - 1;

        while (l > f && !std::memcmp(ra + l, rb + l, sizeof(float))) {
            --l;
        }

        x0[y] = std::min(x0[y], f);
        x1[y] = std::max(x1[y], l);
        any = 1;
    }

    return any;
}


/**
 * A layer's L*a*b* curves, evaluated like the L*a*b* Adjustments tool
 * (ImProcFunctions::chromiLuminanceCurve) with its default settings
 * (chromaticity 0, no red / skin tones protection, no Munsell correction),
 * except that L = f(C) acts on all hues. LUTs are built once per run, at
 * full precision on every pipeline.
 */
struct LayerCurves {
    bool lUse = false;
    bool aUse = false;
    bool bUse = false;
    bool ccUse = false;
    bool lcUse = false;
    bool clUse = false;
    LUTf lCurve;    // L = f(L), 0..32768
    LUTf aCurve;    // a + 32768 -> a + 32768
    LUTf bCurve;
    LUTf ccCurve;   // C = f(C), C * adjustr in 0..65535
    LUTf lcCurve;   // L = f(C)
    LUTf clCurve;   // C = f(L), L in 0..65535
    std::unique_ptr<FlatCurve> chCurve;   // C = f(H)
    std::unique_ptr<FlatCurve> lhCurve;   // L = f(H)
    std::unique_ptr<FlatCurve> hhCurve;   // H = f(H)
    float adjustr = 1.f;
    float amountChroma = 40.f;

    /// Returns nullptr when every curve is an identity.
    static std::unique_ptr<LayerCurves> create(const MaskLayer& l, const Glib::ustring& workingProfile)
    {
        if (!l.hasCurves()) {
            return nullptr;
        }

        std::unique_ptr<LayerCurves> c(new LayerCurves());
        c->lUse = diagonalLut(l.lcurve, c->lCurve, true);
        c->aUse = diagonalLut(l.acurve, c->aCurve, false);
        c->bUse = diagonalLut(l.bcurve, c->bCurve, false);
        c->ccUse = diagonalLut(l.cccurve, c->ccCurve, false);
        c->lcUse = diagonalLut(l.lccurve, c->lcCurve, false);
        c->clUse = diagonalLut(l.clcurve, c->clCurve, false);
        c->chCurve = flat(l.chcurve);
        c->lhCurve = flat(l.lhcurve);
        c->hhCurve = flat(l.hhcurve);

        if (!(c->lUse || c->aUse || c->bUse || c->ccUse || c->lcUse || c->clUse || c->chCurve || c->lhCurve || c->hhCurve)) {
            return nullptr;
        }

        // same gamut related scaling of the chroma curves as the global tool
        if (workingProfile == "ProPhoto" || workingProfile == "WideGamut") {
            c->adjustr = 1.2f;
        } else if (workingProfile == "Adobe RGB" || workingProfile == "BruceRGB") {
            c->adjustr = 1.8f;
        } else if (workingProfile == "sRGB") {
            c->adjustr = 2.0f;
        } else if (workingProfile == "Beta RGB" || workingProfile == "BestRGB") {
            c->adjustr = 1.4f;
        }

        if (settings && settings->amchroma > 0) {
            c->amountChroma = settings->amchroma;
        }

        return c;
    }

    /// Lab L (0..32768) or, for the others, the 0..65535 domain of CurveFactory's LUTs.
    static bool diagonalLut(const std::vector<double>& points, LUTf& lut, bool luminance)
    {
        if (points.empty() || points[0] == 0) {
            return false;
        }

        const DiagonalCurve curve(points, CURVES_MIN_POLY_POINTS);

        if (curve.isIdentity()) {
            return false;
        }

        if (luminance) {
            // as CurveFactory::complexLCurve without brightness / contrast
            lut(32770, LUT_CLIP_OFF);

            for (int i = 0; i < 32768; ++i) {
                lut[i] = 32767.f * curve.getVal(i / 32767.f);
            }

            lut[32768] = 32768.f;
            lut[32769] = 32769.f;
        } else {
            lut(65536, LUT_CLIP_OFF);

            for (int i = 0; i < 65536; ++i) {
                lut[i] = 65535.f * curve.getVal(i / 65535.f);
            }
        }

        return true;
    }

    static std::unique_ptr<FlatCurve> flat(const std::vector<double>& points)
    {
        if (points.empty() || points[0] == 0) {
            return nullptr;
        }

        std::unique_ptr<FlatCurve> curve(new FlatCurve(points));
        return curve->isIdentity() ? nullptr : std::move(curve);
    }

    /// L in 0..32768, a / b in Lab * 327.68; iws: working space inverse matrix (gamut check of L = f(H))
    void apply(float& L, float& a, float& b, const TMatrix& iws) const
    {
        const float LL = L / 327.68f;   // input of C = f(L)
        float Lprov = (lUse ? lCurve[std::max(L, 0.f)] : L) / 327.68f;

        if (aUse) {
            a = aCurve[a + 32768.f] - 32768.f;
        }

        if (bUse) {
            b = bCurve[b + 32768.f] - 32768.f;
        }

        const float CC = std::sqrt(SQR(a) + SQR(b)) / 327.68f;
        float HH = xatan2f(b, a);
        bool rotated = false;

        if (lhCurve || chCurve || hhCurve) {
            const float hsvHue = Color::huelab_to_huehsv2(HH);

            if (lhCurve) {
                float lr = Lprov / 100.f;
                float valparam = lhCurve->getVal(hsvHue) - 0.5f;
                float valparamneg = valparam;
                const float kcc = CC / amountChroma;
                valparam *= 2.f * kcc;
                valparamneg *= kcc;

                if (valparam > 0.f) {
                    lr = (1.f - valparam) * lr + valparam * (1.f - SQR(SQR(1.f - std::min(lr, 1.0f))));
                } else {
                    lr *= 1.f + 1.9f * valparamneg;
                }

                Lprov = lr * 100.f;

                // keep the color inside the working space by reducing the chroma
                if (CC > 0.f) {
                    const float cosH = a / (CC * 327.68f);
                    const float sinH = b / (CC * 327.68f);
                    float C2 = CC;

                    for (int iter = 0; iter < 200; ++iter) {
                        const float fy = Color::c1By116 * Lprov + Color::c16By116;
                        const float fx = 0.002f * C2 * cosH + fy;
                        const float fz = fy - 0.005f * C2 * sinH;
                        const float x = 65535.f * Color::f2xyz(fx) * Color::D50x;
                        const float z = 65535.f * Color::f2xyz(fz) * Color::D50z;
                        const float y = Lprov > Color::epskapf ? 65535.f * fy * fy * fy : 65535.f * Lprov / Color::kappaf;
                        float R, G, B;
                        Color::xyz2rgb(x, y, z, R, G, B, iws);

                        if (R < 0.f || G < 0.f || B < 0.f) {
                            Lprov = std::max(Lprov, 0.1f);
                        } else if (R > 65535.f || G > 65535.f || B > 65535.f) {
                            Lprov = std::min(Lprov, 99.98f);
                        } else {
                            break;
                        }

                        C2 *= 0.95f;
                    }

                    a = 327.68f * C2 * cosH;
                    b = 327.68f * C2 * sinH;
                }
            }

            if (chCurve) {
                const float k = 1.f + 2.f * (chCurve->getVal(hsvHue) - 0.5f);
                a *= k;
                b *= k;
            }

            if (hhCurve) {
                HH += (hhCurve->getVal(hsvHue) - 0.5f) * 1.7f;
                rotated = true;
            }
        }

        if (clUse && LL > 0.f) {
            const float k = clCurve[LL * 655.35f] / (LL * 655.35f);
            a = LIM(a * k, std::min(-42000.f, a), std::max(42000.f, a));
            b = LIM(b * k, std::min(-42000.f, b), std::max(42000.f, b));
        }

        if (ccUse) {
            const float chroma = std::sqrt(SQR(a) + SQR(b) + 0.001f) * adjustr;
            const float k = ccCurve[chroma] / chroma;
            a *= k;
            b *= k;
        }

        if (lcUse) {
            // reduced action at low chroma, as in the global tool
            const float zz = CC < 50.f ? SQR(CC / 50.f) * 0.25f : 0.25f;
            const float chroma = std::sqrt(SQR(a) + SQR(b) + 0.001f) * adjustr;
            const float k = lcCurve[chroma] / chroma;
            Lprov *= (k - 1.f) * zz + 1.f;
        }

        if (rotated) {
            const float C = std::sqrt(SQR(a) + SQR(b));
            const float2 sc = xsincosf(HH);
            a = C * sc.y;
            b = C * sc.x;
        }

        L = Lprov * 327.68f;
    }
};

/// Precomputed per-layer adjustment constants.
struct LayerAdjust {
    bool needsRGB = false;
    float exposureMul = 1.f;
    float contrastK = 1.f;     // slope at mid gray of the S-curve
    float highlights = 0.f;    // -1..1
    float midtones = 0.f;
    float shadows = 0.f;
    float whites = 0.f;
    float blacks = 0.f;
    float rMul = 1.f;          // temperature / tint
    float gMul = 1.f;
    float bMul = 1.f;
    float satK = 1.f;          // 0..2
    bool needsTone = false;    // highlights / midtones / shadows / whites / blacks / contrast
    bool needsHue = false;
    float hueCos = 1.f;
    float hueSin = 0.f;
    float clarityK = 0.f;
    std::unique_ptr<LayerCurves> curves;   // nullptr: no (non identity) curve

    LayerAdjust(const MaskLayer& l, const Glib::ustring& workingProfile) :
        curves(LayerCurves::create(l, workingProfile))
    {
        exposureMul = std::pow(2.f, float(l.exposure));
        contrastK = std::pow(2.f, float(l.contrast / 100.0));
        highlights = float(l.highlights / 100.0);
        midtones = float(l.midtones / 100.0);
        shadows = float(l.shadows / 100.0);
        whites = float(l.whites / 100.0);
        blacks = float(l.blacks / 100.0);

        const float t = float(l.temperature / 100.0);
        const float g = float(l.tint / 100.0);
        rMul = 1.f + 0.25f * t;
        bMul = 1.f - 0.25f * t;
        gMul = 1.f - 0.20f * g;
        satK = 1.f + float(l.saturation / 100.0);

        needsRGB = l.exposure != 0.0 || l.contrast != 0.0 || l.highlights != 0.0 || l.midtones != 0.0 || l.shadows != 0.0
                   || l.whites != 0.0 || l.blacks != 0.0 || l.temperature != 0.0 || l.tint != 0.0
                   || l.saturation != 0.0;

        needsTone = highlights != 0.f || midtones != 0.f || shadows != 0.f || whites != 0.f || blacks != 0.f || contrastK != 1.f;

        needsHue = l.hue != 0.0;
        const float rad = float(l.hue * RT_PI_180);
        hueCos = std::cos(rad);
        hueSin = std::sin(rad);

        clarityK = float(l.clarity / 100.0) * 0.6f;
    }

    /// Tone mapping of a perceptual (gamma 2.2) luminance in [0, 1+].
    float tone(float yp) const
    {
        if (highlights > 0.f) {
            const float w = maskgeom::smoothstep(0.45f, 0.95f, yp);
            yp += highlights * 0.35f * w * std::max(1.f - yp, 0.f);
        } else if (highlights < 0.f) {
            // Recovery scales with the tone itself so that near-white and
            // over-range (yp > 1) highlights are pulled down the most. The wide
            // transition keeps the curve monotonic at -100.
            const float w = maskgeom::smoothstep(0.3f, 1.f, yp);
            yp += highlights * 0.35f * w * yp;
        }

        if (midtones != 0.f) {
            // Bell centred on mid gray, zero at black and white (and beyond
            // white); monotonic at +/-100.
            const float u = std::max(4.f * yp * (1.f - yp), 0.f);
            yp += midtones * 0.2f * u * u;
        }

        if (shadows != 0.f) {
            // Gain on the tone itself: pure black stays anchored (that is what
            // Blacks is for). Transition mirrors the highlight recovery above;
            // both directions stay monotonic at +/-100.
            const float w = 1.f - maskgeom::smoothstep(0.f, 0.7f, yp);
            yp += shadows * (shadows > 0.f ? 1.f : 0.65f) * w * std::max(yp, 0.f);
        }

        if (whites != 0.f) {
            const float w = maskgeom::smoothstep(0.55f, 1.f, yp);
            yp += whites * 0.25f * w;
        }

        if (blacks != 0.f) {
            const float w = 1.f - maskgeom::smoothstep(0.f, 0.4f, yp);
            yp += blacks * 0.25f * w;
        }

        yp = std::max(yp, 0.f);

        if (contrastK != 1.f) {
            // Symmetric S-curve around mid gray; slope at the middle = contrastK.
            if (yp < 0.5f) {
                yp = 0.5f * pow_F(2.f * yp, contrastK);
            } else if (yp < 1.f) {
                yp = 1.f - 0.5f * pow_F(2.f * (1.f - yp), contrastK);
            }
        }

        return yp;
    }
};

/**
 * Applies a layer's adjustments to lab where mask > 0. Rows y are processed
 * from xs0[y] to xs1[y] (inclusive) when given, entirely otherwise.
 */
void applyLayerAdjust(LabImage* lab, const array2D<float>& mask, const LayerAdjust& adj, const array2D<float>* blurredL, const TMatrix& ws, const TMatrix& iws, const int* xs0, const int* xs1, bool multiThread)
{
    const int W = lab->W;
    const int H = lab->H;

#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic, 16) if (multiThread)
#endif
    for (int y = 0; y < H; ++y) {
        const int xa = xs0 ? std::max(xs0[y], 0) : 0;
        const int xb = xs1 ? std::min(xs1[y], W - 1) : W - 1;
        const float* mrow = mask[y];
        float* Lrow = lab->L[y];
        float* arow = lab->a[y];
        float* brow = lab->b[y];

        for (int x = xa; x <= xb; ++x) {
            const float m = mrow[x];

            if (m <= 0.f) {
                continue;   // adjustments only where the alpha channel is > 0
            }

            const float L0 = Lrow[x];
            const float a0 = arow[x];
            const float b0 = brow[x];
            float L = L0;
            float a = a0;
            float b = b0;

            if (adj.needsRGB) {
                float X, Y, Z;
                float r, g, bl;
                Color::Lab2XYZ(L, a, b, X, Y, Z);
                Color::xyz2rgb(X, Y, Z, r, g, bl, iws);

                r = std::max(r, 0.f) * adj.exposureMul * adj.rMul;
                g = std::max(g, 0.f) * adj.exposureMul * adj.gMul;
                bl = std::max(bl, 0.f) * adj.exposureMul * adj.bMul;

                const float lum = Color::rgbLuminance(r, g, bl, ws);

                if (adj.needsTone) {
                    const float yl = std::max(lum, 0.f) / 65535.f;
                    const float yp = yl > 0.f ? pow_F(yl, 1.f / 2.2f) : 0.f;
                    const float ypNew = adj.tone(yp);
                    const float ylNew = pow_F(std::max(ypNew, 0.f), 2.2f);

                    if (yl > 1e-6f) {
                        const float ratio = ylNew / yl;
                        r *= ratio;
                        g *= ratio;
                        bl *= ratio;
                    } else {
                        const float add = ylNew * 65535.f;
                        r += add;
                        g += add;
                        bl += add;
                    }
                }

                if (adj.satK != 1.f) {
                    const float Y2 = Color::rgbLuminance(r, g, bl, ws);
                    r = std::max(Y2 + adj.satK * (r - Y2), 0.f);
                    g = std::max(Y2 + adj.satK * (g - Y2), 0.f);
                    bl = std::max(Y2 + adj.satK * (bl - Y2), 0.f);
                }

                Color::rgbxyz(r, g, bl, X, Y, Z, ws);
                Color::XYZ2Lab(X, Y, Z, L, a, b);
            }

            if (adj.needsHue) {
                const float na = adj.hueCos * a - adj.hueSin * b;
                const float nb = adj.hueSin * a + adj.hueCos * b;
                a = na;
                b = nb;
            }

            if (adj.clarityK != 0.f) {
                L += (L0 - (*blurredL)[y][x]) * adj.clarityK;
            }

            if (adj.curves) {
                adj.curves->apply(L, a, b, iws);
            }

            Lrow[x] = LIM(intp(m, L, L0), 0.f, 32768.f);
            arow[x] = LIM(intp(m, a, a0), -MAX_AB, MAX_AB);
            brow[x] = LIM(intp(m, b, b0), -MAX_AB, MAX_AB);
        }
    }
}

/// Red overlay, Lightroom style: the image stays visible, selected areas turn red.
void applyOverlay(LabImage* lab, const array2D<float>& mask, bool multiThread)
{
    const int W = lab->W;
    const int H = lab->H;

#ifdef _OPENMP
    #pragma omp parallel for if (multiThread)
#endif
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const float m = mask[y][x];

            if (m <= 0.f) {
                continue;
            }

            lab->a[y][x] = LIM(intp(m, 24000.f, lab->a[y][x]), -MAX_AB, MAX_AB);
            lab->b[y][x] = LIM(intp(m, 12000.f, lab->b[y][x]), -MAX_AB, MAX_AB);
        }
    }
}

void computeClarityBase(const LabImage* lab, array2D<float>& blurredL, double scale, bool multiThread)
{
    const int W = lab->W;
    const int H = lab->H;
    blurredL(W, H);
    const double sigma = std::max(25.0 / scale, 1.0);
#ifdef _OPENMP
    #pragma omp parallel if (multiThread)
#endif
    gaussianBlur(lab->L, blurredL, W, H, sigma);
}

/// Masks stage of an interactive pipeline, with its cache (see maskcache.h). Same results as the uncached path.
void maskedAdjustmentsCached(MaskingCache::Data& D, const procparams::ProcParams* params, const procparams::MaskingParams& mp, double scale, bool multiThread, LabImage* lab, const MaskFrame& frame, LabImage* sampleSnapshot, const maskgeom::LabPlanes* referencePtr, const LabImage* fullFrameReference)
{
    const int W = lab->W;
    const int H = lab->H;

    if (D.W != W || D.H != H || D.frame.imageWidth != frame.imageWidth || D.frame.imageHeight != frame.imageHeight
            || D.frame.offsetX != frame.offsetX || D.frame.offsetY != frame.offsetY) {
        D.reset(W, H, frame);
    }

    if (!mp.enabled || mp.masks.empty()) {
        // nothing to do: no input tracking either (a later masking-only
        // refresh must then run the stages before the masks)
        if (sampleSnapshot && sampleSnapshot->W == W && sampleSnapshot->H == H) {
            sampleSnapshot->CopyFrom(lab, multiThread);
        }

        D.input.reset();
        D.restoredFor = nullptr;
        D.lastSample = nullptr;
        D.outputValid = false;
        ++D.inputGen;
        return;
    }

    // Did the stage's input change since the last run?
    bool inputChanged;

    if (D.restoredFor == lab && D.input) {
        inputChanged = false;   // restored by restoreInput() right before this run
    } else {
        MProf mp_("compareInput");
        inputChanged = !sameLab(lab, D.input.get(), multiThread);
    }

    D.restoredFor = nullptr;

    if (inputChanged) {
        copyLab(lab, D.input, multiThread);
        ++D.inputGen;
        D.outputValid = false;
    }

    if (sampleSnapshot && sampleSnapshot->W == W && sampleSnapshot->H == H && (inputChanged || D.lastSample != sampleSnapshot)) {
        sampleSnapshot->CopyFrom(lab, multiThread);
        D.lastSample = sampleSnapshot;
    }

    // Full frame reference (wand / quick selection connectivity on detail crops)
    if (referencePtr) {
        if (!D.hadRef || !sameLab(fullFrameReference, D.ref.get(), multiThread)) {
            copyLab(fullFrameReference, D.ref, multiThread);
            D.hadRef = true;
            ++D.refGen;
        }
    } else if (D.hadRef) {
        D.ref.reset();
        D.hadRef = false;
        ++D.refGen;
    }

    const SegmentationPolicy policy = SegmentationPolicy::ASYNC;
    const bool showOverlay = mp.showMask >= 0 && mp.showMask < int(mp.masks.size());

    if (D.layers.size() != mp.masks.size()) {
        D.layers.resize(mp.masks.size());
    }

    if (showOverlay) {
        D.outputValid = false;
        const MaskLayer& layer = mp.masks[mp.showMask];
        const int oc = mp.showComponent;
        const array2D<float>* mask = nullptr;

        if (oc >= 0 && oc < int(layer.components.size())) {
            const MaskComponent& c = layer.components[oc];

            if (c.type == MaskComponentType::BRUSH) {
                // the component alone, as a layer of its own (bit-identical to evaluating it), so that painting stays incremental
                MaskLayer only = maskDefinition(MaskLayer());
                only.components.push_back(c);
                only.components.back().op = MaskBooleanOp::ADD;
                only.components.back().enabled = true;
                only.components.back().invert = false;
                only.invert = c.invert;
                only.expand = 0.0;
                only.smooth = 0.0;
                only.feather = 0.0;
                only.amount = 100.0;
                buildLayerMaskCached(D, D.overlay, only, frame, lab, mixSig(D.inputGen, 0), true, policy, referencePtr, multiThread);
                mask = D.overlay.final.get();
            } else {
                array2D<float>& m = ensureBuf(D.work, W, H);
                buildLayerMask(layer, oc, frame, lab, policy, referencePtr, m, multiThread);
                mask = &m;
            }
        } else {
            LayerCache& E = D.layers[mp.showMask];
            buildLayerMaskCached(D, E, layer, frame, lab, mixSig(D.inputGen, 0), true, policy, referencePtr, multiThread);
            mask = E.final.get();
        }

        applyOverlay(lab, *mask, multiThread);
        D.trim(MASKING_CACHE_BUDGET);
        return;
    }

    const TMatrix ws = ICCStore::getInstance()->workingSpaceMatrix(params->icm.workingProfile);
    const TMatrix iws = ICCStore::getInstance()->workingSpaceInverseMatrix(params->icm.workingProfile);
    const size_t n = mp.masks.size();

    std::vector<char> active(n, 0);
    std::vector<MaskLayer> adjust(n);

    for (size_t i = 0; i < n; ++i) {
        const MaskLayer& layer = mp.masks[i];
        LayerCache& E = D.layers[i];
        active[i] = layer.enabled && layer.hasAdjustments() && layer.amount > 0.0;
        adjust[i] = adjustmentPart(layer);

        if (!E.adjustKnown || !(E.lastAdjust == adjust[i])) {
            E.lastAdjust = adjust[i];
            E.adjustKnown = true;
            ++E.adjustVersion;
        }
    }

    std::vector<int> x0(H, W), x1(H, -1);   // per row: pixels where some mask changed
    int firstChanged = int(n);

    /*
     * Layers are applied in order and the data dependent components of a
     * layer (ranges, wand, AI...) see the image as left by the layers before
     * it, exactly like the uncached code. 'chain' identifies that image.
     *
     * applyAdjust == false: only (re)build the masks, the image is not
     * modified, so 'lab' is the image a layer sees only as long as no layer
     * before it has a non empty mask; returns false as soon as a build needs
     * the image (the caller then runs the full sequential pass).
     */
    auto runLayers = [&](bool applyAdjust) -> bool {
        uint64_t chain = mixSig(D.inputGen, 0);
        bool labIsCurrent = true;

        for (size_t i = 0; i < n; ++i) {
            if (!active[i]) {
                continue;
            }

            LayerCache& E = D.layers[i];
            const BuildResult r = buildLayerMaskCached(D, E, mp.masks[i], frame, lab, chain, applyAdjust || labIsCurrent, SegmentationPolicy::ASYNC, referencePtr, multiThread);

            if (r == BuildResult::NEEDS_LAB) {
                return false;
            }

            if (r != BuildResult::UNCHANGED) {
                bool changed = true;

                if (r == BuildResult::REBUILT) {
                    changed = unionDiff(*E.final, *D.work, x0, x1, multiThread);
                } else {
                    std::fill(x0.begin(), x0.end(), 0);
                    std::fill(x1.begin(), x1.end(), W - 1);
                }

                if (changed) {
                    firstChanged = std::min(firstChanged, int(i));
                }
            }

            chain = mixSig(mixSig(chain, E.finalVersion), E.adjustVersion);

            if (E.finalEmpty) {
                continue;
            }

            labIsCurrent = false;

            if (applyAdjust) {
                const LayerAdjust adj(mp.masks[i], params->icm.workingProfile);
                MProf mpa_("adjust", int(i));

                if (adj.clarityK != 0.f && !D.blurredValid) {
                    computeClarityBase(lab, ensureBuf(D.blurredL, W, H), scale, multiThread);
                    D.blurredValid = true;
                }

                applyLayerAdjust(lab, *E.final, adj, D.blurredL.get(), ws, iws, nullptr, nullptr, multiThread);
            }
        }

        return true;
    };

    // The output of the last run can be reused where no mask changed if
    // nothing else changed and no mask has to be re-evaluated on the image.
    bool reuse = D.outputValid && D.output && D.output->W == W && D.output->H == H && D.outActive == active && D.outAdjust == adjust
                 && D.outProfile == params->icm.workingProfile && D.outScale == scale;

    if (reuse) {
        MProf mpb_("buildAll(reuse)");
        reuse = runLayers(false);
    }

    if (reuse) {
        // Clarity blurs the L channel as left by the layers before the first
        // clarity layer: only reusable if those did not change.
        for (size_t i = 0; i < n; ++i) {
            if (active[i] && !D.layers[i].finalEmpty && mp.masks[i].clarity != 0.0) {
                if (int(i) > firstChanged || !D.blurredValid) {
                    reuse = false;
                }
            }
        }
    }

    if (reuse) {
        MProf mpr_("adjust(dirty rows)");
        // outside of the changed pixels: the previous output
        const LabImage* out = D.output.get();

#ifdef _OPENMP
        #pragma omp parallel for if (multiThread)
#endif
        for (int y = 0; y < H; ++y) {
            const int a = x0[y];
            const int b = x1[y];

            if (a > b) {
                std::memcpy(lab->L[y], out->L[y], sizeof(float) * W);
                std::memcpy(lab->a[y], out->a[y], sizeof(float) * W);
                std::memcpy(lab->b[y], out->b[y], sizeof(float) * W);
            } else {
                std::memcpy(lab->L[y], out->L[y], sizeof(float) * a);
                std::memcpy(lab->a[y], out->a[y], sizeof(float) * a);
                std::memcpy(lab->b[y], out->b[y], sizeof(float) * a);
                std::memcpy(lab->L[y] + b + 1, out->L[y] + b + 1, sizeof(float) * (W - 1 - b));
                std::memcpy(lab->a[y] + b + 1, out->a[y] + b + 1, sizeof(float) * (W - 1 - b));
                std::memcpy(lab->b[y] + b + 1, out->b[y] + b + 1, sizeof(float) * (W - 1 - b));
            }
        }

        // inside: all layers again, from the input
        for (size_t i = 0; i < n; ++i) {
            if (!active[i] || D.layers[i].finalEmpty) {
                continue;
            }

            const LayerAdjust adj(mp.masks[i], params->icm.workingProfile);
            applyLayerAdjust(lab, *D.layers[i].final, adj, D.blurredL.get(), ws, iws, x0.data(), x1.data(), multiThread);
        }

        LabImage* o = D.output.get();

#ifdef _OPENMP
        #pragma omp parallel for if (multiThread)
#endif
        for (int y = 0; y < H; ++y) {
            const int a = x0[y];
            const int b = x1[y];

            if (a <= b) {
                std::memcpy(o->L[y] + a, lab->L[y] + a, sizeof(float) * (b - a + 1));
                std::memcpy(o->a[y] + a, lab->a[y] + a, sizeof(float) * (b - a + 1));
                std::memcpy(o->b[y] + a, lab->b[y] + a, sizeof(float) * (b - a + 1));
            }
        }
    } else {
        MProf mpr_("layers(full)");
        D.blurredValid = false;
        runLayers(true);
        copyLab(lab, D.output, multiThread);
        D.outputValid = true;
        D.outActive = active;
        D.outAdjust = adjust;
        D.outProfile = params->icm.workingProfile;
        D.outScale = scale;
    }

    D.trim(MASKING_CACHE_BUDGET);

    if (!D.output) {
        D.outputValid = false;
    }
}

}

void ImProcFunctions::maskedAdjustments(LabImage* lab, int cx, int cy, int oW, int oH, MaskingContext context, LabImage* sampleSnapshot, const LabImage* fullFrameReference, MaskingCache* cache)
{
    if (!lab || lab->W <= 0 || lab->H <= 0) {
        return;
    }

    const int W = lab->W;
    const int H = lab->H;
    const MaskFrame frame(std::max(oW, 1), std::max(oH, 1), cx, cy);

    // The masks are stored relative to the image before the coarse and
    // geometric transforms; express them in the frame of this buffer.
    procparams::MaskingParams mapped;
    const procparams::MaskingParams* mpp = &params->masking;

    if (mpp->enabled && !mpp->masks.empty() && !mpp->outputFrame) {
        const MaskTransform transform(*params, frame.imageWidth, frame.imageHeight);

        if (!transform.isIdentity()) {
            mapped = *mpp;
            transform.masksToOutput(mapped.masks);
            mpp = &mapped;
        }
    }

    const procparams::MaskingParams& mp = *mpp;

    // Full frame (lower resolution) planes giving a detail crop the
    // connectivity information the magic wand needs.
    maskgeom::LabPlanes reference;
    const maskgeom::LabPlanes* referencePtr = nullptr;

    if (fullFrameReference && fullFrameReference->W > 0 && fullFrameReference->H > 0 && (cx != 0 || cy != 0 || W != frame.imageWidth || H != frame.imageHeight)) {
        reference.L = fullFrameReference->L;
        reference.a = fullFrameReference->a;
        reference.b = fullFrameReference->b;
        reference.W = fullFrameReference->W;
        reference.H = fullFrameReference->H;
        reference.frame = MaskFrame(fullFrameReference->W, fullFrameReference->H, 0, 0);
        referencePtr = &reference;
    }

    if (cache && context == MaskingContext::PREVIEW && !maskCacheDisabled()) {
        // Interactive pipeline: incremental, bit-identical to the code below.
        MProf mpt_("maskedAdjustments(cached)", W);
        std::lock_guard<std::mutex> lock(cache->mutex);

        if (!cache->data) {
            cache->data.reset(new MaskingCache::Data());
        }

        maskedAdjustmentsCached(*cache->data, params, mp, scale, multiThread, lab, frame, sampleSnapshot, referencePtr, fullFrameReference);
        return;
    }

    // Keep a copy of the input of this stage so that the GUI can sample
    // colors for the Color Range component (see ImProcCoordinator::getMaskingLabSample).
    if (sampleSnapshot && sampleSnapshot->W == lab->W && sampleSnapshot->H == lab->H) {
        sampleSnapshot->CopyFrom(lab, multiThread);
    }

    if (!mp.enabled || mp.masks.empty()) {
        return;
    }

    MProf mpt_("maskedAdjustments", lab->W);

    const bool showOverlay = context == MaskingContext::PREVIEW && mp.showMask >= 0 && mp.showMask < int(mp.masks.size());
    const SegmentationPolicy policy = context == MaskingContext::EXPORT ? SegmentationPolicy::SYNC
                                      : (context == MaskingContext::THUMBNAIL ? SegmentationPolicy::CACHE_ONLY : SegmentationPolicy::ASYNC);

    const TMatrix ws = ICCStore::getInstance()->workingSpaceMatrix(params->icm.workingProfile);
    const TMatrix iws = ICCStore::getInstance()->workingSpaceInverseMatrix(params->icm.workingProfile);

    array2D<float> mask(W, H);
    array2D<float> blurredL;   // allocated lazily for clarity

    for (size_t i = 0; i < mp.masks.size(); ++i) {
        const MaskLayer& layer = mp.masks[i];

        if (showOverlay) {
            if (int(i) != mp.showMask) {
                continue;
            }
        } else if (!layer.enabled || !layer.hasAdjustments() || layer.amount <= 0.0) {
            continue;
        }

        buildLayerMask(layer, showOverlay ? mp.showComponent : -1, frame, lab, policy, referencePtr, mask, multiThread);

        if (showOverlay) {
            applyOverlay(lab, mask, multiThread);
            return;
        }

        if (maskgeom::isEmpty(mask)) {
            continue;
        }

        const LayerAdjust adj(layer, params->icm.workingProfile);
        MProf mpa_("adjust", int(i));

        if (adj.clarityK != 0.f && !blurredL) {
            computeClarityBase(lab, blurredL, scale, multiThread);
        }

        applyLayerAdjust(lab, mask, adj, &blurredL, ws, iws, nullptr, nullptr, multiThread);
    }
}

// ---------------------------------------------------------------------------

MaskingCache::MaskingCache() = default;

MaskingCache::~MaskingCache() = default;

void MaskingCache::clear()
{
    std::lock_guard<std::mutex> lock(mutex);
    data.reset();
}

size_t MaskingCache::memoryUsage() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return data ? data->bytes() : 0;
}

bool MaskingCache::restoreInput(LabImage* lab) const
{
    std::lock_guard<std::mutex> lock(mutex);

    if (!lab || !data || !data->input || data->input->W != lab->W || data->input->H != lab->H) {
        return false;
    }

    lab->CopyFrom(data->input.get());
    data->restoredFor = lab;
    return true;
}

}
