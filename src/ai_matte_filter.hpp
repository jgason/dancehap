// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// ai_matte_filter.hpp — OBS filter that applies AI-based matting to a webcam
// source, producing an RGBA output with alpha (background removed).
//
// Phase 2.0: skeleton — registers the filter with OBS as a pass-through.
// Phase 2.2+: applies the actual matting pipeline via MatteEngine.
//
// See docs/plans/PLAN-PHASE-2.md §2.0.

#pragma once

#include "obs_compat.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Unique OBS filter identifier.
#define AI_MATTE_FILTER_ID   "dancehap_ai_matte"

// Human-readable name shown in OBS's "Add filter" menu.
#define AI_MATTE_FILTER_NAME "DanceHAP Matte"

#ifdef __cplusplus
extern "C" {
#endif

/// Register the ai_matte_filter with OBS.
/// Called from obs_module_load().
void register_ai_matte_filter(void);

/// Return a pointer to the static obs_source_info struct.
/// Available in both real-OBS and stub modes; in stub mode the struct
/// members are directly inspectable by unit tests.
const struct obs_source_info *ai_matte_filter_get_info(void);

/// Process a frame through the matte filter logic.
/// In Phase 2.0 (pass-through), returns the input data pointer unmodified.
/// In Phase 2.2+, applies the alpha mask (matting) to the frame.
///
/// @param input_data  Pointer to the input frame data (BGRA pixels, 4 bytes/pixel).
/// @param width       Input frame width in pixels.
/// @param height      Input frame height in pixels.
/// @param out_width   Output: processed frame width  (may equal input).
/// @param out_height  Output: processed frame height (may equal input).
/// @return            Pointer to the processed frame data (BGRA with alpha applied).
const void *ai_matte_filter_process_frame(const void *input_data,
                                           uint32_t width,
                                           uint32_t height,
                                           uint32_t *out_width,
                                           uint32_t *out_height);

#ifdef __cplusplus
} // extern "C"
#endif

namespace dancehap {

/// Apply an alpha mask to a BGRA frame.
/// Multiplies each pixel's alpha channel by the corresponding mask value.
/// Pure logic — testable without OBS.
///
/// @param bgra_data  Input BGRA frame (4 bytes/pixel, row-major).
/// @param width      Frame width.
/// @param height     Frame height.
/// @param mask       Alpha mask (width*height floats in [0,1]).
/// @return           Output BGRA frame with alpha applied (new allocation).
std::vector<uint8_t> apply_alpha_mask_to_bgra(
    const uint8_t *bgra_data, uint32_t width, uint32_t height,
    const std::vector<float> &mask);

} // namespace dancehap