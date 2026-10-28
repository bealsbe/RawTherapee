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

#include <cstddef>
#include <memory>
#include <mutex>

namespace rtengine
{

class LabImage;

/**
 * State the Masks stage keeps between two runs of one interactive pipeline
 * (the preview of an ImProcCoordinator, each detail Crop) so that an edit
 * only re-renders what it changed:
 *
 *  - the input of the stage (to detect whether upstream tools changed it, and
 *    to restart from it on a masking-only refresh, see restoreInput()),
 *  - per layer: the final (refined) mask, plus for the layer being edited a
 *    snapshot of the selection before the edited component and, when that
 *    component is a brush, the coverage of its last stroke so that a stroke
 *    being painted is rasterized incrementally,
 *  - the output of the stage, so that a mask change only re-applies the
 *    adjustments where some mask actually changed.
 *
 * Everything is keyed by the exact mask parameters, the buffer geometry and
 * the content of the input, so the results are bit-identical to an uncached
 * run. Batch export and thumbnails do not use a cache.
 *
 * One instance per pipeline; thread safe.
 */
class MaskingCache final
{
public:
    MaskingCache();
    ~MaskingCache();
    MaskingCache(const MaskingCache&) = delete;
    MaskingCache& operator=(const MaskingCache&) = delete;

    /// Drops all buffers.
    void clear();

    /// Approximate memory held, in bytes.
    size_t memoryUsage() const;

    /**
     * Copies the input of the last run of the Masks stage into 'lab' if it has
     * the same size. Used to skip the Lab stages before the masks when only
     * the masking parameters changed. Returns false if there is no such input.
     */
    bool restoreInput(LabImage* lab) const;

    struct Data;   // defined in ipmasking.cc

private:
    friend class ImProcFunctions;
    std::unique_ptr<Data> data;
    mutable std::mutex mutex;
};

}
