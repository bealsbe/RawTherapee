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
 * rt_segment_server.py). The backend runs the actual models;
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
 *
 * That refresh issues the same requests again, so a request must never
 * start a job every time it is seen, or the preview refreshes for ever:
 *
 *  - the cache keeps several images per key. The same key is legitimately
 *    asked for different images within one refresh: a layer sees the image
 *    as left by the layers before it, the mask overlay shows the stage
 *    input, and each editor tab has its own image. With one entry per key
 *    those requests would evict each other's result in turn.
 *  - a job that failed is not started again for RETRY_SECONDS.
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
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
        std::string key;
        std::vector<float> signature;   // SIGNATURE_SIZE x SIGNATURE_SIZE mean L*
        int width = 0;                  // mask size (normalized full-image coordinates)
        int height = 0;
        std::vector<float> mask;        // row major, 0..1
        uint64_t lastUse = 0;
    };

    struct Job {
        uint64_t id = 0;
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

    /// A background job: in 'running' until its result (or failure) is recorded, in 'workers' until its thread is joined.
    struct Running {
        uint64_t id = 0;
        std::string key;
        std::vector<float> signature;
    };

    struct Worker {
        uint64_t id = 0;
        std::thread thread;
        bool done = false;   ///< the thread no longer needs 'mutex': it can be joined
    };

    struct Failure {
        std::string key;
        std::vector<float> signature;
        std::chrono::steady_clock::time_point when;
    };

    static constexpr int SIGNATURE_SIZE = 24;
    static constexpr int MAX_SIDE = 1024;
    static constexpr size_t MAX_CACHE_ENTRIES = 16;   ///< masks kept, least recently used dropped first (up to 4 MB each)
    static constexpr int RETRY_SECONDS = 10;          ///< a failed job is not started again before that

    std::string makeKey(const SegmentationRequest& req) const;
    static std::vector<float> computeSignature(const SegmentationRequest& req);
    static bool signatureMatches(const std::vector<float>& a, const std::vector<float>& b);
    static void prepareImage(const SegmentationRequest& req, Job& job);
    bool runJob(const Job& job, std::string& error);
    void sampleCached(const CacheEntry& entry, const SegmentationRequest& req, array2D<float>& out) const;

    // callers hold mutex
    CacheEntry* findEntry(const std::string& key, const std::vector<float>& signature);
    CacheEntry* findLatestEntry(const std::string& key);
    bool isRunning(const std::string& key, const std::vector<float>& signature) const;
    bool failedRecently(const std::string& key, const std::vector<float>& signature);
    void takeFinishedThreads(std::vector<std::thread>& finished);

    ExternalSegmentationBackend::ModelInfo info_;

    std::mutex mutex;
    std::vector<CacheEntry> cache;
    std::vector<Running> running;
    std::vector<Worker> workers;
    std::vector<Failure> failures;
    uint64_t useClock = 0;
    uint64_t nextJobId = 1;
};

/**
 * Registers one provider per model reported by the backend, including unavailable
 * ones (so the GUI can show them greyed out with the reason); the backend's
 * default becomes the registry default.
 */
void registerExternalSegmentationProviders();

}
