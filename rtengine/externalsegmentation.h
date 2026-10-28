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
 * AI segmentation through an external process (rtdata/segmentation/
 * rt_segment_server.py). The backend runs the actual models (SAM 3 first);
 * this side only converts the image, exchanges files and caches results.
 *
 * One ExternalSegmentationProvider is registered per model the backend
 * reports, so the user can switch models from the Masks panel. Models the
 * backend reports as unavailable (gated weights, missing package) are
 * registered too, flagged unavailable, so the GUI can explain why.
 *
 * Results are cached per (model, target, prompt, hint) together with a
 * small luminance signature of the image, so that moving unrelated sliders
 * does not re-run the model. Preview requests are served asynchronously:
 * the cached (or empty) mask is returned at once, the model runs in a
 * worker thread, and the registry's result callback asks the GUI to
 * refresh the preview when the mask is ready.
 */

#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <glibmm/spawn.h>

#include "aisegmentation.h"

namespace rtengine
{

class ExternalSegmentationBackend
{
public:
    struct ModelInfo {
        std::string id;
        std::string name;
        std::string description;
        bool available = false;
        std::string reason;     ///< why the model is unavailable, or a note (e.g. weights are downloaded on first use)
        bool hidden = false;    ///< usable by id (tests) but not offered in the GUI
    };

    static ExternalSegmentationBackend& getInstance();

    /// Interpreter (e.g. "python3"), server script path, torch device ("auto", "cuda", "cpu").
    void configure(const std::string& command, const std::string& script, const std::string& device);
    bool isConfigured() const;

    /// Starts the backend if needed and lists its models. Empty on failure (see error).
    std::vector<ModelInfo> queryModels(std::string& defaultModel, std::string& error);

    /// Runs one segmentation. imagePath is a binary PPM, the mask is written to maskPath as a binary PGM.
    bool segment(const std::string& modelId, const std::string& imagePath, const std::string& maskPath,
                 const std::string& target, const std::string& prompt, bool hasPoint, double hintX, double hintY,
                 double threshold, bool refine, int& instances, std::string& error);

    /// Stops the child process (it is restarted on the next request).
    void shutdown();

private:
    ExternalSegmentationBackend() = default;
    ~ExternalSegmentationBackend();

    bool start(std::string& error);   // caller holds mutex
    void stopLocked();                // caller holds mutex
    bool roundtrip(const std::string& request, std::string& response, std::string& error);   // caller holds mutex

    std::mutex mutex;
    std::string command;
    std::string script;
    std::string device;
    Glib::Pid pid = 0;
    FILE* toChild = nullptr;
    FILE* fromChild = nullptr;
    int nextId = 1;
};

class ExternalSegmentationProvider final : public AISegmentationProvider
{
public:
    explicit ExternalSegmentationProvider(const ExternalSegmentationBackend::ModelInfo& info);
    ~ExternalSegmentationProvider() override;

    std::string id() const override;
    std::string displayName() const override;
    bool supports(procparams::MaskComponentType target) const override;
    bool isAvailable() const override;
    std::string availabilityReason() const override;
    bool isHidden() const override;
    bool segment(const SegmentationRequest& req, array2D<float>& out) override;

private:
    struct CacheEntry {
        std::vector<float> signature;   // SIGNATURE_SIZE x SIGNATURE_SIZE mean L*
        int width = 0;                  // mask size (normalized full-image coordinates)
        int height = 0;
        std::vector<float> mask;        // row major, 0..1
    };

    struct Job {
        std::string key;
        std::string target;
        std::string prompt;
        bool hasPoint = false;
        double hintX = 0.5;
        double hintY = 0.5;
        double threshold = 0.5;
        bool refine = false;
        int width = 0;
        int height = 0;
        std::vector<unsigned char> rgb;   // 8 bit sRGB, row major
        std::vector<float> signature;
    };

    static constexpr int SIGNATURE_SIZE = 24;
    static constexpr int MAX_SIDE = 1024;

    std::string makeKey(const SegmentationRequest& req) const;
    static std::vector<float> computeSignature(const SegmentationRequest& req);
    static bool signatureMatches(const std::vector<float>& a, const std::vector<float>& b);
    static void prepareImage(const SegmentationRequest& req, Job& job);
    bool runJob(const Job& job, std::string& error);
    void sampleCached(const CacheEntry& entry, const SegmentationRequest& req, array2D<float>& out) const;
    void joinFinishedThreads();

    ExternalSegmentationBackend::ModelInfo info_;

    std::mutex mutex;
    std::map<std::string, CacheEntry> cache;
    std::set<std::string> pending;
    std::vector<std::thread> threads;
};

/**
 * Registers one provider per model reported by the backend, including unavailable
 * ones (so the GUI can show them greyed out with the reason); the backend's
 * default becomes the registry default.
 */
void registerExternalSegmentationProviders();

}
