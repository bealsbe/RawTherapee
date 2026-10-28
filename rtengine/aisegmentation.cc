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

#include "aisegmentation.h"

#include <algorithm>
#include <cmath>

namespace rtengine
{

namespace
{

constexpr float LAB_SCALE = 327.68f;

inline float clamp01(float v)
{
    return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}

}

using procparams::MaskComponentType;

// ---------------------------------------------------------------------------
// DummySegmentationProvider

const std::string DummySegmentationProvider::ID = "dummy";

std::string DummySegmentationProvider::id() const
{
    return ID;
}

std::string DummySegmentationProvider::displayName() const
{
    return "Heuristic (placeholder)";
}

bool DummySegmentationProvider::supports(MaskComponentType target) const
{
    return procparams::maskComponentIsAI(target);
}

bool DummySegmentationProvider::isHidden() const
{
    return true;
}

bool DummySegmentationProvider::segment(const SegmentationRequest& req, array2D<float>& out)
{
    if (!req.L || !req.a || !req.b || req.width <= 0 || req.height <= 0
            || out.getWidth() != req.width || out.getHeight() != req.height) {
        return false;
    }

    switch (req.target) {
        case MaskComponentType::AI_SKY:
            skyScore(req, out);
            return true;

        case MaskComponentType::AI_SUBJECT:
            subjectScore(req, out);
            return true;

        case MaskComponentType::AI_BACKGROUND:
            subjectScore(req, out);
            maskgeom::invert(out, req.multiThread);
            return true;

        case MaskComponentType::AI_OBJECT:
            objectScore(req, out);
            return true;

        default:
            return false;
    }
}

// Sky: bright, bluish (negative b*) or bright neutral pixels, weighted towards the top of the image.
void DummySegmentationProvider::skyScore(const SegmentationRequest& req, array2D<float>& out) const
{
    const int W = req.width;
    const int H = req.height;

#ifdef _OPENMP
    #pragma omp parallel for if (req.multiThread)
#endif
    for (int y = 0; y < H; ++y) {
        const float ny = float(req.frame.normY(y + 0.5));
        const float vertical = 1.f - maskgeom::smoothstep(0.45f, 0.9f, ny);
        float* row = out[y];

        for (int x = 0; x < W; ++x) {
            const float Ls = req.L[y][x] / LAB_SCALE;
            const float as = req.a[y][x] / LAB_SCALE;
            const float bs = req.b[y][x] / LAB_SCALE;
            const float chroma = std::sqrt(as * as + bs * bs);

            const float blue = maskgeom::smoothstep(3.f, 20.f, -bs) * maskgeom::smoothstep(25.f, 55.f, Ls);
            const float neutralBright = maskgeom::smoothstep(78.f, 95.f, Ls) * (1.f - maskgeom::smoothstep(6.f, 16.f, chroma));
            row[x] = clamp01(std::max(blue, neutralBright) * vertical);
        }
    }
}

// Subject: center weighted ellipse, excluding what looks like sky.
void DummySegmentationProvider::subjectScore(const SegmentationRequest& req, array2D<float>& out) const
{
    const int W = req.width;
    const int H = req.height;

    array2D<float> sky(W, H);
    skyScore(req, sky);

#ifdef _OPENMP
    #pragma omp parallel for if (req.multiThread)
#endif
    for (int y = 0; y < H; ++y) {
        const float ny = float(req.frame.normY(y + 0.5));
        float* row = out[y];

        for (int x = 0; x < W; ++x) {
            const float nx = float(req.frame.normX(x + 0.5));
            const float dx = (nx - 0.5f) / 0.5f;
            const float dy = (ny - 0.55f) / 0.5f;
            const float rho = std::sqrt(dx * dx + dy * dy);
            const float center = 1.f - maskgeom::smoothstep(0.35f, 0.85f, rho);
            row[x] = clamp01(center * (1.f - sky[y][x]));
        }
    }
}

// Object: pixels around the hint that are similar in color to the hinted pixel.
void DummySegmentationProvider::objectScore(const SegmentationRequest& req, array2D<float>& out) const
{
    const int W = req.width;
    const int H = req.height;

    const double hx = req.frame.bufferX(req.hintX);
    const double hy = req.frame.bufferY(req.hintY);
    const double diag = req.frame.diagonal();
    const double sigmaNear = 0.04 * diag;   // always selected around the hint
    const double sigmaFar = 0.18 * diag;    // selected when the color matches

    // Reference color: average of a small neighbourhood around the hint (clamped into the buffer).
    const int cx = std::min(std::max(int(hx), 0), W - 1);
    const int cy = std::min(std::max(int(hy), 0), H - 1);
    float refL = 0.f, refA = 0.f, refB = 0.f;
    int n = 0;

    for (int y = std::max(cy - 2, 0); y <= std::min(cy + 2, H - 1); ++y) {
        for (int x = std::max(cx - 2, 0); x <= std::min(cx + 2, W - 1); ++x) {
            refL += req.L[y][x];
            refA += req.a[y][x];
            refB += req.b[y][x];
            ++n;
        }
    }

    if (n > 0) {
        refL /= n * LAB_SCALE;
        refA /= n * LAB_SCALE;
        refB /= n * LAB_SCALE;
    }

#ifdef _OPENMP
    #pragma omp parallel for if (req.multiThread)
#endif
    for (int y = 0; y < H; ++y) {
        const double dy = (y + 0.5) - hy;
        float* row = out[y];

        for (int x = 0; x < W; ++x) {
            const double dx = (x + 0.5) - hx;
            const double d2 = dx * dx + dy * dy;
            const float near = float(std::exp(-d2 / (2.0 * sigmaNear * sigmaNear)));
            const float far = float(std::exp(-d2 / (2.0 * sigmaFar * sigmaFar)));

            const float dL = (req.L[y][x] / LAB_SCALE - refL) * 0.5f;
            const float da = req.a[y][x] / LAB_SCALE - refA;
            const float db = req.b[y][x] / LAB_SCALE - refB;
            const float dE = std::sqrt(dL * dL + da * da + db * db);
            const float similar = 1.f - maskgeom::smoothstep(8.f, 28.f, dE);

            row[x] = clamp01(std::max(near, far * similar));
        }
    }
}

// ---------------------------------------------------------------------------
// AISegmentationRegistry

AISegmentationRegistry& AISegmentationRegistry::getInstance()
{
    static AISegmentationRegistry instance;
    return instance;
}

void AISegmentationRegistry::registerProvider(std::unique_ptr<AISegmentationProvider> provider)
{
    if (!provider) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex);
    const std::string pid = provider->id();

    for (auto& p : providers) {
        if (p->id() == pid) {
            p = std::move(provider);
            return;
        }
    }

    const bool usable = provider->isAvailable() && !provider->isHidden();
    providers.push_back(std::move(provider));

    if (defaultId.empty() && usable) {
        defaultId = pid;
    }
}

AISegmentationProvider* AISegmentationRegistry::findDefaultLocked() const
{
    for (const auto& p : providers) {
        if (p->id() == defaultId && p->isAvailable() && !p->isHidden()) {
            return p.get();
        }
    }

    for (const auto& p : providers) {
        if (p->isAvailable() && !p->isHidden()) {
            return p.get();
        }
    }

    return nullptr;
}

AISegmentationProvider* AISegmentationRegistry::getProvider(const std::string& id) const
{
    std::lock_guard<std::mutex> lock(mutex);

    if (!id.empty()) {
        for (const auto& p : providers) {
            if (p->id() == id) {
                return p.get();
            }
        }
    }

    return findDefaultLocked();
}

void AISegmentationRegistry::setDefaultProvider(const std::string& id)
{
    std::lock_guard<std::mutex> lock(mutex);

    for (const auto& p : providers) {
        if (p->id() == id && p->isAvailable() && !p->isHidden()) {
            defaultId = id;
            return;
        }
    }
}

std::string AISegmentationRegistry::getDefaultProviderId() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return defaultId;
}

int AISegmentationRegistry::addResultListener(std::function<void(bool)> callback)
{
    std::lock_guard<std::mutex> lock(mutex);
    const int id = nextListenerId++;
    resultListeners[id] = std::move(callback);
    return id;
}

void AISegmentationRegistry::removeResultListener(int id)
{
    std::lock_guard<std::mutex> lock(mutex);
    resultListeners.erase(id);
}

void AISegmentationRegistry::setResultCallback(std::function<void(bool)> callback)
{
    std::lock_guard<std::mutex> lock(mutex);

    if (compatListenerId) {
        resultListeners.erase(compatListenerId);
        compatListenerId = 0;
    }

    if (callback) {
        compatListenerId = nextListenerId++;
        resultListeners[compatListenerId] = std::move(callback);
    }
}

void AISegmentationRegistry::notifyResult() const
{
    std::vector<std::function<void(bool)>> cbs;
    {
        std::lock_guard<std::mutex> lock(mutex);

        for (const auto& l : resultListeners) {
            cbs.push_back(l.second);
        }
    }

    for (const auto& cb : cbs) {
        cb(true);
    }
}

void AISegmentationRegistry::notifyProgress() const
{
    std::vector<std::function<void(bool)>> cbs;
    {
        std::lock_guard<std::mutex> lock(mutex);

        for (const auto& l : resultListeners) {
            cbs.push_back(l.second);
        }
    }

    for (const auto& cb : cbs) {
        cb(false);
    }
}

void AISegmentationRegistry::setLastError(const std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex);
    lastError = error;
}

std::string AISegmentationRegistry::getLastError() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return lastError;
}

void AISegmentationRegistry::addPending(int delta)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        pendingCount += delta;
    }

    if (delta > 0) {
        // a job started: let the GUI show that the model is working
        notifyProgress();
    }
}

int AISegmentationRegistry::getPending() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return pendingCount;
}

std::vector<std::pair<std::string, std::string>> AISegmentationRegistry::listProviders() const
{
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::pair<std::string, std::string>> result;
    result.reserve(providers.size());

    for (const auto& p : providers) {
        if (p->isAvailable() && !p->isHidden()) {
            result.emplace_back(p->id(), p->displayName());
        }
    }

    return result;
}

std::vector<SegmentationProviderInfo> AISegmentationRegistry::listProviderInfos() const
{
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<SegmentationProviderInfo> result;
    result.reserve(providers.size());

    for (const auto& p : providers) {
        if (p->isHidden()) {
            continue;
        }

        SegmentationProviderInfo info;
        info.id = p->id();
        info.name = p->displayName();
        info.available = p->isAvailable();
        info.reason = p->availabilityReason();
        result.push_back(std::move(info));
    }

    return result;
}

}
