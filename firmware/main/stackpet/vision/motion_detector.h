/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>
#include <vector>

namespace stackpet::vision {

struct MotionResult {
    bool valid       = false;  // false when there is no reference or the size does not match
    float score      = 0.0f;   // fraction of cells that changed, 0..1
    float meanShift  = 0.0f;   // mean signed brightness change (current - reference), removed before thresholding
    int changedCells = 0;
    float cx         = 0.0f;   // centroid of changed cells in image coordinates, -1 (left) .. 1 (right)
    float cy         = 0.0f;   // -1 (top) .. 1 (bottom)
    // Largest 4-connected blob of changed cells: where "the" mover is when several people move at once
    int blobCells    = 0;
    float blobCx     = 0.0f;   // blob centre, same coordinates as cx / cy
    float blobCy     = 0.0f;
};

// Average brightness of a luma frame (0 for an empty frame), e.g. to warn that the room is too dark.
float meanLuma(const uint8_t* luma, int width, int height);

/**
 * @brief Pure C++ frame-difference motion detector on a small luma grid (host testable, no ESP-IDF deps).
 *
 * The global brightness change between frames (camera auto exposure) is compensated before a cell is
 * counted as changed (gain on the reference, then the remaining offset), so an exposure drift does not
 * look like motion. Isolated changed cells without a changed 4-neighbour are ignored as sensor noise.
 */
class MotionDetector {
public:
    void setCellThreshold(uint8_t threshold);  // abs luma diff to count a cell as changed, default 24
    uint8_t cellThreshold() const;

    void setReference(const uint8_t* luma, int width, int height);
    void clearReference();
    bool hasReference() const;

    // Compare a frame against the reference frame (the reference is not modified).
    MotionResult compare(const uint8_t* luma, int width, int height) const;

private:
    std::vector<uint8_t> _reference;
    mutable std::vector<uint8_t> _changed;  // scratch buffers reused by compare() to avoid per-frame allocations
    mutable std::vector<uint8_t> _kept;
    mutable std::vector<int> _stack;
    int _width         = 0;
    int _height        = 0;
    uint8_t _threshold = 24;
};

}  // namespace stackpet::vision
