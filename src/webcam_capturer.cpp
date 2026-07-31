// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// webcam_capturer.cpp — Internal webcam capture for DanceHAP Phase 3.
//
// Phase 3, Étape 5 (ADR-011):
//   Wraps obs_source_create_private() to create an invisible OBS webcam
//   source and consume its async frames via obs_source_get_frame().
//
// CRITICAL: obs_source_get_frame() is the CORRECT API for async sources
// (webcams). It would be a bug on a sync filter (leçon bug 10), but here
// the webcam IS an async source. We must respect the linesize (row stride)
// when copying — the source frame may have linesize > width*4 due to GPU
// padding. We copy line by line, never bulk memcpy.

#include "webcam_capturer.hpp"

#include <algorithm>
#include <cstring>

namespace dancehap {

// ---------------------------------------------------------------------------+
// Static helpers — testable in stub mode (no OBS dependency)
// ---------------------------------------------------------------------------+

std::vector<uint8_t> WebcamCapturer::copyFrameRespectingLinesize(
    const uint8_t *src_data, int src_linesize,
    int width, int height, int bpp)
{
    if (!src_data || width <= 0 || height <= 0 || bpp <= 0) return {};
    if (src_linesize < width * bpp) return {};

    const int dst_row_size = width * bpp;
    std::vector<uint8_t> dst(static_cast<size_t>(dst_row_size) * height);

    // Copy line by line — the source may have padding at the end of each row
    // (src_linesize > dst_row_size). A single memcpy of width*height*bpp
    // bytes would misalign all rows after the first.
    for (int row = 0; row < height; ++row) {
        const uint8_t *src_row = src_data + static_cast<ptrdiff_t>(row) * src_linesize;
        uint8_t *dst_row = dst.data() + static_cast<ptrdiff_t>(row) * dst_row_size;
        std::memcpy(dst_row, src_row, static_cast<size_t>(dst_row_size));
    }

    return dst;
}

WebcamFrame WebcamCapturer::createBlackFrame(int width, int height)
{
    WebcamFrame frame;
    if (width <= 0 || height <= 0) return frame;
    frame.width  = static_cast<uint32_t>(width);
    frame.height = static_cast<uint32_t>(height);
    // BGRA black = {0, 0, 0, 255} (opaque black)
    frame.bgra.resize(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < frame.bgra.size(); i += 4) {
        frame.bgra[i]     = 0;   // B
        frame.bgra[i + 1] = 0;   // G
        frame.bgra[i + 2] = 0;   // R
        frame.bgra[i + 3] = 255; // A (opaque)
    }
    return frame;
}

// ---------------------------------------------------------------------------+
// Implementation struct (pimpl)
// ---------------------------------------------------------------------------+

struct WebcamCapturer::Impl {
    bool opened = false;
    int  cap_width  = 0;
    int  cap_height = 0;
    int  cap_fps    = 0;
    std::string device_id;

#ifdef DANCEHAP_HAVE_OBS
    obs_source_t *webcam_source = nullptr;

    ~Impl()
    {
        if (webcam_source) {
            obs_source_release(webcam_source);
            webcam_source = nullptr;
        }
    }
#endif
};

// ---------------------------------------------------------------------------+
// Lifecycle
// ---------------------------------------------------------------------------+

WebcamCapturer::WebcamCapturer()
    : pimpl_(std::make_unique<Impl>()) {}

WebcamCapturer::~WebcamCapturer()
{
    close();
}

WebcamCapturer::WebcamCapturer(WebcamCapturer &&) noexcept = default;
WebcamCapturer &WebcamCapturer::operator=(WebcamCapturer &&) noexcept = default;

bool WebcamCapturer::open(const std::string &device_id, int width, int height, int fps)
{
    close();
    pimpl_->device_id = device_id;
    pimpl_->cap_width  = width;
    pimpl_->cap_height = height;
    pimpl_->cap_fps    = fps;

#ifdef DANCEHAP_HAVE_OBS
    // Create the private webcam source (invisible to the OBS UI).
    // Platform-specific source ID:
    //   Windows: "dshow_input" (DirectShow)
    //   macOS:   "av_capture_input" (AVFoundation)
    obs_data_t *settings = obs_data_create();
    if (!settings) return false;

    obs_data_set_string(settings, "device_id", device_id.c_str());
    if (width > 0 && height > 0) {
        std::string res = std::to_string(width) + "x" + std::to_string(height);
        obs_data_set_string(settings, "resolution", res.c_str());
    }
    if (fps > 0) {
        obs_data_set_string(settings, "frame_rate", std::to_string(fps).c_str());
    }

    // Select the correct source ID for the platform.
#if defined(_WIN32)
    const char *source_id = "dshow_input";
#elif defined(__APPLE__)
    const char *source_id = "av_capture_input";
#else
    // Linux is a non-goal (ADR-001) but we provide a fallback for dev.
    const char *source_id = "v4l2_input";
#endif

    pimpl_->webcam_source = obs_source_create_private(
        source_id, "dancehap_webcam_internal", settings);
    obs_data_release(settings);

    if (!pimpl_->webcam_source) {
        blog(LOG_WARNING, "[DanceHAP] Failed to create private webcam source "
             "(device='%s', %dx%d@%d)", device_id.c_str(), width, height, fps);
        return false;
    }

    blog(LOG_INFO, "[DanceHAP] Webcam source created (device='%s', %dx%d@%d)",
         device_id.c_str(), width, height, fps);
    pimpl_->opened = true;
    return true;
#else
    // Stub mode: no real webcam. Mark as "opened" but getFrame() returns
    // a black frame (so the compositor pipeline can be tested end-to-end).
    (void)device_id; (void)width; (void)height; (void)fps;
    pimpl_->opened = true;
    return true;
#endif
}

void WebcamCapturer::close()
{
#ifdef DANCEHAP_HAVE_OBS
    if (pimpl_->webcam_source) {
        obs_source_release(pimpl_->webcam_source);
        pimpl_->webcam_source = nullptr;
    }
#endif
    pimpl_->opened = false;
}

bool WebcamCapturer::isOpen() const
{
    return pimpl_->opened;
}

int WebcamCapturer::width() const
{
    return pimpl_->cap_width;
}

int WebcamCapturer::height() const
{
    return pimpl_->cap_height;
}

// ---------------------------------------------------------------------------+
// Frame capture
// ---------------------------------------------------------------------------+

WebcamFrame WebcamCapturer::getFrame()
{
    // If not open, return an invalid frame.
    if (!pimpl_->opened || pimpl_->cap_width <= 0 || pimpl_->cap_height <= 0)
        return WebcamFrame{};

#ifdef DANCEHAP_HAVE_OBS
    if (!pimpl_->webcam_source) return WebcamFrame{};

    // obs_source_get_frame() is the CORRECT API for async sources (webcams).
    // It returns the latest available frame (or nullptr if none ready yet).
    // The frame->data[0] is BGRA, and frame->linesize is the row stride
    // which may be > width*4 due to GPU padding.
    obs_source_frame *frame = obs_source_get_frame(pimpl_->webcam_source);
    if (!frame) return WebcamFrame{};

    WebcamFrame result;
    result.width  = frame->width  > 0 ? frame->width  : static_cast<uint32_t>(pimpl_->cap_width);
    result.height = frame->height > 0 ? frame->height : static_cast<uint32_t>(pimpl_->cap_height);

    // CRITICAL: copy respecting the linesize (row stride). The source frame
    // may have linesize > width*4 (padding). We copy line by line to produce
    // a tightly packed output. NEVER bulk memcpy width*height*4 bytes.
    if (frame->data[0] && frame->linesize[0] >= (int)(result.width * 4)) {
        result.bgra = copyFrameRespectingLinesize(
            frame->data[0],
            frame->linesize[0],
            static_cast<int>(result.width),
            static_cast<int>(result.height),
            4);
    }
    result.timestamp_ns = frame->timestamp;

    obs_source_release_frame(pimpl_->webcam_source, frame);
    return result;
#else
    // Stub mode: produce a black frame. This lets the composite source
    // pipeline be tested end-to-end without a real webcam.
    return createBlackFrame(pimpl_->cap_width, pimpl_->cap_height);
#endif
}

} // namespace dancehap