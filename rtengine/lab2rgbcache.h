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

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include "iccstore.h"
#include "image8.h"
#include "improcfun.h"
#include "labimage.h"
#include "procparams.h"
#include "settings.h"

namespace rtengine
{

/**
 * Incremental ImProcFunctions::lab2rgb(lab, 0, 0, cw, ch, icm) (the "true"
 * image of the interactive pipelines, in the output profile): keeps the
 * previous Lab input and result, and converts only the bounding box of the
 * pixels that changed since the previous call. Bit-identical to a full
 * conversion (the lcms transform works pixel by pixel). Not thread safe: one
 * instance per pipeline, used under that pipeline's lock.
 */
class Lab2RgbCache
{
public:
    /// Returns a new image (owned by the caller), like ImProcFunctions::lab2rgb.
    Image8* convert(ImProcFunctions& ipf, LabImage* lab, int cw, int ch, const procparams::ColorManagementParams& icm)
    {
        const int W = std::min(cw, lab->W);
        const int H = std::min(ch, lab->H);

        // lab2rgb converts a sub-rectangle pixel by pixel only when it uses the output profile
        const Glib::ustring profile = icm.outputProfile.empty() || icm.outputProfile == procparams::ColorManagementParams::NoICMString ? Glib::ustring("sRGB") : icm.outputProfile;
        const bool incremental = !settings->HistogramWorking && ICCStore::getInstance()->getProfile(profile);

        const bool reuse = incremental && lastLab && lastImg && lastIcm && *lastIcm == icm
                           && lastLab->W == W && lastLab->H == H && lastImg->getWidth() == W && lastImg->getHeight() == H;

        if (!reuse) {
            Image8* img = ipf.lab2rgb(lab, 0, 0, cw, ch, icm);

            if (incremental && img->getWidth() == W && img->getHeight() == H) {
                if (!lastLab || lastLab->W != W || lastLab->H != H) {
                    lastLab.reset(new LabImage(W, H));
                }

                copyRows(lab, lastLab.get(), 0, 0, W, H);
                lastImg.reset(new Image8(W, H));
                std::memcpy(lastImg->data, img->data, size_t(W) * H * 3);
                lastIcm.reset(new procparams::ColorManagementParams(icm));
            } else {
                clear();
            }

            return img;
        }

        // bounding box of the changed pixels
        std::vector<int> rx0(H, W), rx1(H, -1);

#ifdef _OPENMP
        #pragma omp parallel for
#endif
        for (int y = 0; y < H; ++y) {
            const float* cur[3] = {lab->L[y], lab->a[y], lab->b[y]};
            const float* old[3] = {lastLab->L[y], lastLab->a[y], lastLab->b[y]};

            for (int c = 0; c < 3; ++c) {
                if (!std::memcmp(cur[c], old[c], sizeof(float) * W)) {
                    continue;
                }

                int f = 0;

                while (f < W && !std::memcmp(cur[c] + f, old[c] + f, sizeof(float))) {
                    ++f;
                }

                int l = W - 1;

                while (l > f && !std::memcmp(cur[c] + l, old[c] + l, sizeof(float))) {
                    --l;
                }

                rx0[y] = std::min(rx0[y], f);
                rx1[y] = std::max(rx1[y], l);
            }
        }

        int bx0 = W, bx1 = -1, by0 = H, by1 = -1;

        for (int y = 0; y < H; ++y) {
            if (rx0[y] <= rx1[y]) {
                bx0 = std::min(bx0, rx0[y]);
                bx1 = std::max(bx1, rx1[y]);
                by0 = std::min(by0, y);
                by1 = y;
            }
        }

        if (bx0 <= bx1) {
            const int bw = bx1 - bx0 + 1;
            const int bh = by1 - by0 + 1;
            const std::unique_ptr<Image8> part(ipf.lab2rgb(lab, bx0, by0, bw, bh, icm));

            for (int y = 0; y < bh; ++y) {
                std::memcpy(lastImg->data + 3 * (size_t(by0 + y) * W + bx0), part->data + 3 * size_t(y) * bw, 3 * size_t(bw));
            }

            copyRows(lab, lastLab.get(), bx0, by0, bw, bh);
        }

        Image8* img = new Image8(W, H);
        std::memcpy(img->data, lastImg->data, size_t(W) * H * 3);
        return img;
    }

    void clear()
    {
        lastLab.reset();
        lastImg.reset();
        lastIcm.reset();
    }

private:
    static void copyRows(const LabImage* src, LabImage* dst, int x, int y0, int w, int h)
    {
        for (int y = y0; y < y0 + h; ++y) {
            std::memcpy(dst->L[y] + x, src->L[y] + x, sizeof(float) * w);
            std::memcpy(dst->a[y] + x, src->a[y] + x, sizeof(float) * w);
            std::memcpy(dst->b[y] + x, src->b[y] + x, sizeof(float) * w);
        }
    }

    std::unique_ptr<LabImage> lastLab;
    std::unique_ptr<Image8> lastImg;
    std::unique_ptr<procparams::ColorManagementParams> lastIcm;
};

}
