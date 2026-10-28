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
 * Pluggable "AI" segmentation for the Masks tool (Subject / Sky /
 * Background / Object).
 *
 * The engine never talks to a model directly. It asks the registry for a
 * provider and hands it a SegmentationRequest describing the processing
 * buffer (Lab planes at the current scale plus the MaskFrame). The provider
 * returns a confidence map in [0, 1] at the same resolution. Heavy backends
 * (ONNX Runtime, TensorFlow Lite, ...) only need to implement
 * AISegmentationProvider and register themselves once at startup; nothing
 * else in the pipeline or the GUI has to change.
 *
 * Providers advertise whether they are usable (isAvailable()) so the GUI can
 * list models that exist but cannot run yet (gated weights, missing package)
 * greyed out with the reason. Hidden providers (isHidden()) are usable when
 * requested by id but never listed; the heuristic DummySegmentationProvider
 * is such a provider and is only registered by tests.
 *
 * Thread safety: segment() may be called concurrently from the preview
 * thread and from detail-crop threads, so providers must be reentrant or
 * lock internally. The registry itself is thread safe.
 */

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "array2D.h"
#include "maskgeometry.h"
#include "maskparams.h"

namespace rtengine
{

/// How much waiting a request tolerates.
enum class SegmentationPolicy {
    SYNC,        ///< compute now, block until the mask is available (exports)
    ASYNC,       ///< return the cached mask (or nothing) and compute in the background (preview)
    CACHE_ONLY   ///< never compute, only reuse cached results (thumbnails)
};

struct SegmentationRequest {
    const float* const* L = nullptr;   ///< Lab planes, RawTherapee internal scale (L 0..32768, a/b +-42000)
    const float* const* a = nullptr;
    const float* const* b = nullptr;
    int width = 0;                     ///< size of the planes (and of the output map)
    int height = 0;
    MaskFrame frame;                   ///< how the planes map onto the whole image
    procparams::MaskComponentType target = procparams::MaskComponentType::AI_SUBJECT;
    double hintX = 0.5;                ///< normalized point hint (AI_OBJECT)
    double hintY = 0.5;
    std::string prompt;                ///< optional concept text ("person", "red car", ...)
    double threshold = 0.5;            ///< confidence threshold, 0..1
    bool refineEdges = false;          ///< ask the backend to refine the mask edges with a matting model (slower)
    SegmentationPolicy policy = SegmentationPolicy::SYNC;
    bool multiThread = true;
};

class AISegmentationProvider
{
public:
    virtual ~AISegmentationProvider() = default;

    /// Stable identifier stored in the pp3 (e.g. "dummy", "onnx-u2net").
    virtual std::string id() const = 0;

    /// Human readable name shown in the GUI.
    virtual std::string displayName() const = 0;

    /// Whether the provider can produce masks for the given target.
    virtual bool supports(procparams::MaskComponentType target) const = 0;

    /// Whether the provider can run right now (weights accessible, dependencies installed).
    virtual bool isAvailable() const
    {
        return true;
    }

    /// Why the provider is unavailable (or a short note about it, e.g. "downloads weights on first use"); "" if none.
    virtual std::string availabilityReason() const
    {
        return "";
    }

    /// Hidden providers can be requested by id but are not offered in the GUI (placeholders for tests).
    virtual bool isHidden() const
    {
        return false;
    }

    /**
     * Compute a confidence map for req.target.
     * @param out  array sized req.width x req.height by the caller; values in [0, 1]
     * @return false if the provider could not produce a result (the mask is then empty)
     */
    virtual bool segment(const SegmentationRequest& req, array2D<float>& out) = 0;
};

/// One row of the provider list shown in the GUI.
struct SegmentationProviderInfo {
    std::string id;
    std::string name;
    bool available = true;
    std::string reason;   ///< why it is unavailable (empty when available)
};

/**
 * Fallback provider using simple image heuristics (position, lightness and
 * chroma). It exists so that the masking pipeline, the pp3 format and the
 * GUI can be exercised end to end without any ML dependency. Expect rough
 * results; it is meant to be replaced, not tuned. It is hidden (never listed)
 * and is not registered by default; tests register it themselves.
 */
class DummySegmentationProvider final : public AISegmentationProvider
{
public:
    static const std::string ID;

    std::string id() const override;
    std::string displayName() const override;
    bool supports(procparams::MaskComponentType target) const override;
    bool isHidden() const override;
    bool segment(const SegmentationRequest& req, array2D<float>& out) override;

private:
    void skyScore(const SegmentationRequest& req, array2D<float>& out) const;
    void subjectScore(const SegmentationRequest& req, array2D<float>& out) const;
    void objectScore(const SegmentationRequest& req, array2D<float>& out) const;
};

class AISegmentationRegistry
{
public:
    static AISegmentationRegistry& getInstance();

    /// Adds (or replaces, by id) a provider. The first registered available, non-hidden provider becomes the default.
    void registerProvider(std::unique_ptr<AISegmentationProvider> provider);

    /**
     * Returns the provider with the given id (even if it is unavailable, so that its
     * segment() can report why), or the default provider when id is empty or unknown.
     * The default is never unavailable or hidden. Returns nullptr when no usable
     * provider is registered; callers must then produce an empty mask.
     */
    AISegmentationProvider* getProvider(const std::string& id) const;

    /// Makes id the default; ignored for unknown, unavailable or hidden providers.
    void setDefaultProvider(const std::string& id);
    std::string getDefaultProviderId() const;

    /// (id, display name) of the usable providers: available and not hidden. Kept for callers that only need selectable entries.
    std::vector<std::pair<std::string, std::string>> listProviders() const;

    /// All non-hidden providers, including unavailable ones (with the reason), in registration order; for the GUI's Provider list.
    std::vector<SegmentationProviderInfo> listProviderInfos() const;

    /// Listeners called (from any thread) when an asynchronous segmentation starts (finished=false,
    /// progress feedback) or finishes (finished=true, the GUI refreshes the preview). Several panels
    /// (editor tabs, the batch tool panel) can listen at the same time; each removes its own entry.
    int addResultListener(std::function<void(bool finished)> callback);
    void removeResultListener(int id);
    /// Compatibility: a single callback (replaced by the next call; nullptr removes it).
    void setResultCallback(std::function<void(bool finished)> callback);
    void notifyResult() const;
    void notifyProgress() const;

    /// Last backend error message, for display in the GUI ("" if none).
    void setLastError(const std::string& error);
    std::string getLastError() const;

    /// Number of asynchronous segmentations currently running.
    void addPending(int delta);
    int getPending() const;

private:
    AISegmentationRegistry() = default;

    AISegmentationProvider* findDefaultLocked() const;   // caller holds mutex

    mutable std::mutex mutex;
    std::vector<std::unique_ptr<AISegmentationProvider>> providers;
    std::string defaultId;
    std::map<int, std::function<void(bool)>> resultListeners;
    int nextListenerId = 1;
    int compatListenerId = 0;
    std::string lastError;
    int pendingCount = 0;
};

}
