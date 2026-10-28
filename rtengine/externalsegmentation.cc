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

#include "externalsegmentation.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include <cjson/cJSON.h>
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>

#ifndef _WIN32
#include <unistd.h>
#include <signal.h>
#endif

#include "color.h"
#include "settings.h"

namespace rtengine
{

using procparams::MaskComponentType;

namespace
{

constexpr float LAB_SCALE = 327.68f;

inline float clamp01(float v)
{
    return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}

inline float srgbGamma(float lin)
{
    lin = clamp01(lin);
    return lin <= 0.0031308f ? 12.92f * lin : 1.055f * std::pow(lin, 1.f / 2.4f) - 0.055f;
}

std::string targetName(MaskComponentType t)
{
    switch (t) {
        case MaskComponentType::AI_SKY:
            return "sky";

        case MaskComponentType::AI_BACKGROUND:
            return "background";

        case MaskComponentType::AI_OBJECT:
            return "object";

        case MaskComponentType::AI_SUBJECT:
        default:
            return "subject";
    }
}

std::string jsonString(const cJSON* obj, const char* key, const std::string& fallback = "")
{
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (v && cJSON_IsString(v) && v->valuestring) ? std::string(v->valuestring) : fallback;
}

bool jsonBool(const cJSON* obj, const char* key, bool fallback = false)
{
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (v && cJSON_IsBool(v)) ? cJSON_IsTrue(v) : fallback;
}

double jsonNumber(const cJSON* obj, const char* key, double fallback = 0.0)
{
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (v && cJSON_IsNumber(v)) ? v->valuedouble : fallback;
}

/// Reads a binary PGM (P5, maxval 255) into a 0..1 float buffer.
bool readPGM(const std::string& path, int& width, int& height, std::vector<float>& data, std::string& error)
{
    std::ifstream f(path, std::ios::binary);

    if (!f) {
        error = "cannot open mask file " + path;
        return false;
    }

    std::string magic;
    int maxval = 0;
    f >> magic >> width >> height >> maxval;

    if (magic != "P5" || width <= 0 || height <= 0 || maxval <= 0 || maxval > 255) {
        error = "unexpected mask format in " + path;
        return false;
    }

    f.get();   // single whitespace after maxval
    std::vector<unsigned char> raw(size_t(width) * height);
    f.read(reinterpret_cast<char*>(raw.data()), raw.size());

    if (!f) {
        error = "truncated mask file " + path;
        return false;
    }

    data.resize(raw.size());
    const float scale = 1.f / maxval;

    for (size_t i = 0; i < raw.size(); ++i) {
        data[i] = raw[i] * scale;
    }

    return true;
}

bool writePPM(const std::string& path, int width, int height, const std::vector<unsigned char>& rgb, std::string& error)
{
    std::ofstream f(path, std::ios::binary);

    if (!f) {
        error = "cannot write image file " + path;
        return false;
    }

    f << "P6\n" << width << " " << height << "\n255\n";
    f.write(reinterpret_cast<const char*>(rgb.data()), rgb.size());
    return bool(f);
}

std::string tempPath(const char* suffix)
{
    std::string name;
    const int fd = Glib::file_open_tmp(name, "rtseg-");

    if (fd >= 0) {
#ifndef _WIN32
        close(fd);
#endif
    }

    return name + suffix;
}

}

// ---------------------------------------------------------------------------
// ExternalSegmentationBackend

ExternalSegmentationBackend& ExternalSegmentationBackend::getInstance()
{
    static ExternalSegmentationBackend instance;
    return instance;
}

ExternalSegmentationBackend::~ExternalSegmentationBackend()
{
    std::lock_guard<std::mutex> lock(mutex);
    stopLocked();
}

void ExternalSegmentationBackend::configure(const std::string& cmd, const std::string& scriptPath, const std::string& dev)
{
    std::lock_guard<std::mutex> lock(mutex);

    if (cmd != command || scriptPath != script || dev != device) {
        stopLocked();
    }

    command = cmd;
    script = scriptPath;
    device = dev;
}

bool ExternalSegmentationBackend::isConfigured() const
{
    return !command.empty() && !script.empty();
}

bool ExternalSegmentationBackend::start(std::string& error)
{
    if (toChild && fromChild) {
        return true;
    }

    stopLocked();

    if (!isConfigured()) {
        error = "segmentation backend is not configured";
        return false;
    }

    if (!Glib::file_test(script, Glib::FILE_TEST_EXISTS)) {
        error = "segmentation backend script not found: " + script;
        return false;
    }

    std::vector<std::string> argv {command, script, "--device", device.empty() ? std::string("auto") : device};
    int inFd = -1;
    int outFd = -1;

    try {
        Glib::spawn_async_with_pipes(Glib::get_current_dir(), argv,
                                     Glib::SPAWN_SEARCH_PATH | Glib::SPAWN_DO_NOT_REAP_CHILD,
                                     Glib::SlotSpawnChildSetup(), &pid, &inFd, &outFd, nullptr);
    } catch (const Glib::Error& e) {
        error = "cannot start segmentation backend (" + command + "): " + e.what();
        pid = 0;
        return false;
    }

    toChild = fdopen(inFd, "w");
    fromChild = fdopen(outFd, "r");

    if (!toChild || !fromChild) {
        error = "cannot open pipes to the segmentation backend";
        stopLocked();
        return false;
    }

    if (settings && settings->verbose) {
        printf("Segmentation backend started: %s %s\n", command.c_str(), script.c_str());
    }

    return true;
}

void ExternalSegmentationBackend::stopLocked()
{
    if (toChild) {
        fputs("{\"cmd\":\"quit\"}\n", toChild);
        fflush(toChild);
        fclose(toChild);
        toChild = nullptr;
    }

    if (fromChild) {
        fclose(fromChild);
        fromChild = nullptr;
    }

    if (pid) {
#ifndef _WIN32
        kill(pid, SIGTERM);
#endif
        Glib::spawn_close_pid(pid);
        pid = 0;
    }
}

void ExternalSegmentationBackend::shutdown()
{
    std::lock_guard<std::mutex> lock(mutex);
    stopLocked();
}

bool ExternalSegmentationBackend::roundtrip(const std::string& request, std::string& response, std::string& error)
{
    if (!start(error)) {
        return false;
    }

    if (fputs(request.c_str(), toChild) == EOF || fputc('\n', toChild) == EOF || fflush(toChild) != 0) {
        error = "segmentation backend is not accepting requests";
        stopLocked();
        return false;
    }

    response.clear();
    char buffer[4096];

    while (true) {
        if (!fgets(buffer, sizeof(buffer), fromChild)) {
            error = "segmentation backend exited unexpectedly";
            stopLocked();
            return false;
        }

        response += buffer;

        if (!response.empty() && response.back() == '\n') {
            break;
        }
    }

    return true;
}

std::vector<ExternalSegmentationBackend::ModelInfo> ExternalSegmentationBackend::queryModels(std::string& defaultModel, std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<ModelInfo> result;
    std::string response;

    if (!roundtrip("{\"cmd\":\"info\"}", response, error)) {
        return result;
    }

    const std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(response.c_str()), cJSON_Delete);

    if (!root || !jsonBool(root.get(), "ok")) {
        error = root ? jsonString(root.get(), "error", "backend error") : "invalid backend response";
        return result;
    }

    defaultModel = jsonString(root.get(), "default");
    const cJSON* models = cJSON_GetObjectItemCaseSensitive(root.get(), "models");

    if (models && cJSON_IsArray(models)) {
        for (const cJSON* m = models->child; m; m = m->next) {
            ModelInfo info;
            info.id = jsonString(m, "id");
            info.name = jsonString(m, "name", info.id);
            info.description = jsonString(m, "description");
            info.available = jsonBool(m, "available");
            info.reason = jsonString(m, "reason");
            info.hidden = jsonBool(m, "hidden");

            if (!info.id.empty()) {
                result.push_back(info);
            }
        }
    }

    return result;
}

bool ExternalSegmentationBackend::segment(const std::string& modelId, const std::string& imagePath, const std::string& maskPath,
        const std::string& target, const std::string& prompt, bool hasPoint, double hintX, double hintY,
        double threshold, bool refine, int& instances, std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex);

    const std::unique_ptr<cJSON, decltype(&cJSON_Delete)> req(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddStringToObject(req.get(), "cmd", "segment");
    cJSON_AddNumberToObject(req.get(), "id", nextId++);
    cJSON_AddStringToObject(req.get(), "model", modelId.c_str());
    cJSON_AddStringToObject(req.get(), "image", imagePath.c_str());
    cJSON_AddStringToObject(req.get(), "mask", maskPath.c_str());
    cJSON_AddStringToObject(req.get(), "target", target.c_str());
    cJSON_AddStringToObject(req.get(), "prompt", prompt.c_str());
    cJSON_AddNumberToObject(req.get(), "threshold", threshold);
    cJSON_AddBoolToObject(req.get(), "refine", refine);

    if (hasPoint) {
        cJSON* point = cJSON_AddArrayToObject(req.get(), "point");
        cJSON_AddItemToArray(point, cJSON_CreateNumber(hintX));
        cJSON_AddItemToArray(point, cJSON_CreateNumber(hintY));
    } else {
        cJSON_AddNullToObject(req.get(), "point");
    }

    char* text = cJSON_PrintUnformatted(req.get());
    const std::string request = text ? text : "";
    free(text);

    std::string response;

    if (!roundtrip(request, response, error)) {
        return false;
    }

    const std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(response.c_str()), cJSON_Delete);

    if (!root) {
        error = "invalid backend response";
        return false;
    }

    if (!jsonBool(root.get(), "ok")) {
        error = jsonString(root.get(), "error", "segmentation failed");
        return false;
    }

    instances = int(jsonNumber(root.get(), "instances"));

    if (settings && settings->verbose) {
        printf("Segmentation (%s, %s): %d instance(s) in %.2f s\n", modelId.c_str(), target.c_str(), instances, jsonNumber(root.get(), "elapsed"));
    }

    return true;
}

// ---------------------------------------------------------------------------
// ExternalSegmentationProvider

ExternalSegmentationProvider::ExternalSegmentationProvider(const ExternalSegmentationBackend::ModelInfo& info) :
    info_(info)
{
}

ExternalSegmentationProvider::~ExternalSegmentationProvider()
{
    // Make pending jobs fail quickly, then wait for their threads.
    ExternalSegmentationBackend::getInstance().shutdown();

    std::vector<std::thread> running;
    {
        std::lock_guard<std::mutex> lock(mutex);
        running.swap(threads);
    }

    for (auto& t : running) {
        if (t.joinable()) {
            t.join();
        }
    }
}

std::string ExternalSegmentationProvider::id() const
{
    return info_.id;
}

std::string ExternalSegmentationProvider::displayName() const
{
    return info_.name;
}

bool ExternalSegmentationProvider::supports(MaskComponentType target) const
{
    return procparams::maskComponentIsAI(target);
}

bool ExternalSegmentationProvider::isAvailable() const
{
    return info_.available;
}

std::string ExternalSegmentationProvider::availabilityReason() const
{
    return info_.reason;
}

bool ExternalSegmentationProvider::isHidden() const
{
    return info_.hidden;
}

std::string ExternalSegmentationProvider::makeKey(const SegmentationRequest& req) const
{
    std::ostringstream key;
    key << info_.id << '|' << targetName(req.target) << '|' << req.prompt << '|';

    if (req.target == MaskComponentType::AI_OBJECT) {
        key << int(std::lround(req.hintX * 1000)) << ',' << int(std::lround(req.hintY * 1000));
    }

    key << '|' << int(std::lround(req.threshold * 100)) << '|' << (req.refineEdges ? "refine" : "raw");
    return key.str();
}

std::vector<float> ExternalSegmentationProvider::computeSignature(const SegmentationRequest& req)
{
    std::vector<float> sig(SIGNATURE_SIZE * SIGNATURE_SIZE, 0.f);

    for (int sy = 0; sy < SIGNATURE_SIZE; ++sy) {
        const int y0 = sy * req.height / SIGNATURE_SIZE;
        const int y1 = std::max((sy + 1) * req.height / SIGNATURE_SIZE, y0 + 1);

        for (int sx = 0; sx < SIGNATURE_SIZE; ++sx) {
            const int x0 = sx * req.width / SIGNATURE_SIZE;
            const int x1 = std::max((sx + 1) * req.width / SIGNATURE_SIZE, x0 + 1);
            double sum = 0.0;
            int n = 0;

            for (int y = y0; y < y1 && y < req.height; y += 2) {
                for (int x = x0; x < x1 && x < req.width; x += 2) {
                    sum += req.L[y][x];
                    ++n;
                }
            }

            sig[sy * SIGNATURE_SIZE + sx] = n ? float(sum / n) / LAB_SCALE : 0.f;
        }
    }

    return sig;
}

bool ExternalSegmentationProvider::signatureMatches(const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size() || a.empty()) {
        return false;
    }

    double diff = 0.0;

    for (size_t i = 0; i < a.size(); ++i) {
        diff += std::fabs(a[i] - b[i]);
    }

    // mean absolute difference in L* units; small tonal edits keep the mask
    return diff / a.size() < 3.0;
}

void ExternalSegmentationProvider::prepareImage(const SegmentationRequest& req, Job& job)
{
    const int longSide = std::max(req.width, req.height);
    const int step = std::max((longSide + MAX_SIDE - 1) / MAX_SIDE, 1);
    job.width = std::max(req.width / step, 1);
    job.height = std::max(req.height / step, 1);
    job.rgb.assign(size_t(job.width) * job.height * 3, 0);

#ifdef _OPENMP
    #pragma omp parallel for if (req.multiThread)
#endif
    for (int y = 0; y < job.height; ++y) {
        for (int x = 0; x < job.width; ++x) {
            // box filter over the step x step source block
            float L = 0.f, a = 0.f, b = 0.f;
            int n = 0;

            for (int sy = y * step; sy < std::min((y + 1) * step, req.height); ++sy) {
                for (int sx = x * step; sx < std::min((x + 1) * step, req.width); ++sx) {
                    L += req.L[sy][sx];
                    a += req.a[sy][sx];
                    b += req.b[sy][sx];
                    ++n;
                }
            }

            if (n > 0) {
                L /= n;
                a /= n;
                b /= n;
            }

            float X, Y, Z, r, g, bl;
            Color::Lab2XYZ(L, a, b, X, Y, Z);
            Color::xyz2srgb(X, Y, Z, r, g, bl);
            unsigned char* px = &job.rgb[(size_t(y) * job.width + x) * 3];
            px[0] = (unsigned char)std::lround(srgbGamma(r / 65535.f) * 255.f);
            px[1] = (unsigned char)std::lround(srgbGamma(g / 65535.f) * 255.f);
            px[2] = (unsigned char)std::lround(srgbGamma(bl / 65535.f) * 255.f);
        }
    }
}

bool ExternalSegmentationProvider::runJob(const Job& job, std::string& error)
{
    const std::string imagePath = tempPath(".ppm");
    const std::string maskPath = tempPath(".pgm");
    bool ok = writePPM(imagePath, job.width, job.height, job.rgb, error);
    int instances = 0;

    if (ok) {
        ok = ExternalSegmentationBackend::getInstance().segment(info_.id, imagePath, maskPath, job.target, job.prompt,
                job.hasPoint, job.hintX, job.hintY, job.threshold, job.refine, instances, error);
    }

    CacheEntry entry;

    if (ok) {
        ok = readPGM(maskPath, entry.width, entry.height, entry.mask, error);
    }

    std::remove(imagePath.c_str());
    std::remove(maskPath.c_str());

    if (!ok) {
        return false;
    }

    entry.signature = job.signature;
    std::lock_guard<std::mutex> lock(mutex);
    cache[job.key] = std::move(entry);
    return true;
}

void ExternalSegmentationProvider::sampleCached(const CacheEntry& entry, const SegmentationRequest& req, array2D<float>& out) const
{
    const int mw = entry.width;
    const int mh = entry.height;

#ifdef _OPENMP
    #pragma omp parallel for if (req.multiThread)
#endif
    for (int y = 0; y < req.height; ++y) {
        const double ny = req.frame.normY(y + 0.5);
        const double fy = std::min(std::max(ny * mh - 0.5, 0.0), double(mh - 1));
        const int y0 = int(fy);
        const int y1 = std::min(y0 + 1, mh - 1);
        const float wy = float(fy - y0);
        float* row = out[y];

        for (int x = 0; x < req.width; ++x) {
            const double nx = req.frame.normX(x + 0.5);
            const double fx = std::min(std::max(nx * mw - 0.5, 0.0), double(mw - 1));
            const int x0 = int(fx);
            const int x1 = std::min(x0 + 1, mw - 1);
            const float wx = float(fx - x0);
            const float top = entry.mask[size_t(y0) * mw + x0] * (1.f - wx) + entry.mask[size_t(y0) * mw + x1] * wx;
            const float bottom = entry.mask[size_t(y1) * mw + x0] * (1.f - wx) + entry.mask[size_t(y1) * mw + x1] * wx;
            row[x] = clamp01(top * (1.f - wy) + bottom * wy);
        }
    }
}

void ExternalSegmentationProvider::joinFinishedThreads()
{
    // caller holds mutex; threads of finished jobs are joined lazily
    for (auto it = threads.begin(); it != threads.end();) {
        if (it->joinable() && pending.empty()) {
            it->join();
            it = threads.erase(it);
        } else {
            ++it;
        }
    }
}

bool ExternalSegmentationProvider::segment(const SegmentationRequest& req, array2D<float>& out)
{
    if (!req.L || !req.a || !req.b || req.width <= 0 || req.height <= 0
            || out.getWidth() != req.width || out.getHeight() != req.height) {
        return false;
    }

    if (!info_.available) {
        AISegmentationRegistry::getInstance().setLastError(info_.name + ": " + info_.reason);
        out.fill(0.f, req.multiThread);
        return false;
    }

    const std::string key = makeKey(req);
    const bool fullFrame = req.frame.offsetX == 0 && req.frame.offsetY == 0
                           && req.width == req.frame.imageWidth && req.height == req.frame.imageHeight;

    std::vector<float> signature;

    if (fullFrame) {
        signature = computeSignature(req);
    }

    {
        std::lock_guard<std::mutex> lock(mutex);
        joinFinishedThreads();
        const auto it = cache.find(key);

        if (it != cache.end() && (!fullFrame || signatureMatches(it->second.signature, signature) || req.policy != SegmentationPolicy::SYNC)) {
            const bool fresh = !fullFrame || signatureMatches(it->second.signature, signature);
            sampleCached(it->second, req, out);

            if (fresh || req.policy == SegmentationPolicy::CACHE_ONLY || !fullFrame) {
                return true;
            }

            // stale cache on a preview request: return it now, recompute in the background
        } else if (!fullFrame || req.policy == SegmentationPolicy::CACHE_ONLY) {
            // a detail crop cannot be segmented on its own; the preview fills the cache
            out.fill(0.f, req.multiThread);
            return false;
        } else {
            out.fill(0.f, req.multiThread);
        }

        if (req.policy == SegmentationPolicy::ASYNC && pending.count(key)) {
            return it != cache.end();
        }
    }

    Job job;
    job.key = key;
    job.target = targetName(req.target);
    job.prompt = req.prompt;
    job.hasPoint = req.target == MaskComponentType::AI_OBJECT;
    job.hintX = req.hintX;
    job.hintY = req.hintY;
    job.threshold = req.threshold;
    job.refine = req.refineEdges;
    job.signature = signature;
    prepareImage(req, job);

    if (req.policy == SegmentationPolicy::SYNC) {
        std::string error;

        if (!runJob(job, error)) {
            AISegmentationRegistry::getInstance().setLastError(error);

            if (settings && settings->verbose) {
                printf("Segmentation failed: %s\n", error.c_str());
            }

            return false;
        }

        AISegmentationRegistry::getInstance().setLastError("");
        std::lock_guard<std::mutex> lock(mutex);
        sampleCached(cache[key], req, out);
        return true;
    }

    // ASYNC: compute in the background and let the GUI refresh when done.
    {
        std::lock_guard<std::mutex> lock(mutex);
        pending.insert(key);
        AISegmentationRegistry::getInstance().addPending(1);
        threads.emplace_back([this, job]() {
            std::string error;
            const bool ok = runJob(job, error);
            AISegmentationRegistry::getInstance().setLastError(ok ? "" : error);

            if (!ok && settings && settings->verbose) {
                printf("Segmentation failed: %s\n", error.c_str());
            }

            {
                std::lock_guard<std::mutex> lock2(mutex);
                pending.erase(job.key);
            }

            AISegmentationRegistry::getInstance().addPending(-1);
            AISegmentationRegistry::getInstance().notifyResult();
        });
    }

    std::lock_guard<std::mutex> lock(mutex);
    return cache.count(key) > 0;
}

// ---------------------------------------------------------------------------

void registerExternalSegmentationProviders()
{
    auto& backend = ExternalSegmentationBackend::getInstance();

    if (!backend.isConfigured()) {
        return;
    }

    std::string defaultModel;
    std::string error;
    const auto models = backend.queryModels(defaultModel, error);
    auto& registry = AISegmentationRegistry::getInstance();

    if (models.empty()) {
        registry.setLastError(error);

        if (settings && settings->verbose) {
            printf("No external segmentation models: %s\n", error.c_str());
        }

        return;
    }

    for (const auto& m : models) {
        if (!m.available && settings && settings->verbose) {
            printf("Segmentation model %s not available: %s\n", m.id.c_str(), m.reason.c_str());
        }

        registry.registerProvider(std::unique_ptr<AISegmentationProvider>(new ExternalSegmentationProvider(m)));
    }

    if (!defaultModel.empty()) {
        registry.setDefaultProvider(defaultModel);
    }

    if (!registry.getProvider("")) {
        std::string reasons;

        for (const auto& m : models) {
            if (!m.hidden) {
                reasons += (reasons.empty() ? "" : "; ") + m.name + ": " + m.reason;
            }
        }

        registry.setLastError("no usable segmentation model (" + reasons + ")");
    }
}

}
