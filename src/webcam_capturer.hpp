// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// webcam_capturer.hpp — Internal webcam capture for DanceHAP Phase 3.
//
// Phase 3, Étape 5 (ADR-011):
//   Wraps obs_source_create_private() to create an invisible OBS webcam
//   source and consume its async frames via obs_source_get_frame().
//
//   DLayer 2 = LIVE (capture webcam interne + matting). This class handles
//   the capture part; the matting is applied by MatteEngine integrated into
//   the composite source.
//
// Platform-specific source IDs:
//   Windows: "dshow_input" (DirectShow)
//   macOS:   "av_capture_input" (AVFoundation)
//
// Anti-patternes respectés (voir dancehap-brief):
//   • obs_source_get_frame() est l'API pour sources ASYNC (webcams) — CORRECT
//     ici. (C'est un bug sur un filtre sync, mais pas sur une source async.)
//   • Respecter le linesize (stride de ligne) — copier ligne par ligne,
//     JAMAIS bulk memcpy. Le linesize peut être > width*4 (padding GPU).
//   • #ifdef DANCEHAP_HAVE_OBS protège les appels OBS réels. En stub mode,
//     getFrame() produit des frames noires (testable sans webcam).
//   • Le matting est STATIQUE (ADR-010) — 1 config pour tout le show. Le
//     modèle est chargé au démarrage du show, pas par frame.

#pragma once

#include "obs_compat.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dancehap {

/// Captured webcam frame in BGRA format (row-major, packed).
/// The data vector owns a copy of the pixels with linesize == width*4
/// (no padding) so downstream code can do a single contiguous read.
struct WebcamFrame {
    uint32_t width  = 0;
    uint32_t height = 0;
    std::vector<uint8_t> bgra;  // width * height * 4 bytes, tightly packed
    int64_t  timestamp_ns = 0;

    bool valid() const { return width > 0 && height > 0 && !bgra.empty(); }
};

/// Internal webcam capturer — wraps a private OBS webcam source.
///
/// Lifecycle:
///   1. open(device_id, width, height, fps) — creates the private source
///   2. getFrame() — called from video_tick (CPU thread), copies the latest
///      async frame respecting linesize
///   3. close() — releases the private source
///
/// In stub mode (no DANCEHAP_HAVE_OBS), open() returns false and getFrame()
/// returns an invalid (empty) frame. Tests can verify the frame format
/// logic through the static helper WebcamCapturer::copyFrameRespectingLinesize().
class WebcamCapturer {
public:
    WebcamCapturer();
    ~WebcamCapturer();

    WebcamCapturer(const WebcamCapturer &) = delete;
    WebcamCapturer &operator=(const WebcamCapturer &) = delete;
    WebcamCapturer(WebcamCapturer &&) noexcept;
    WebcamCapturer &operator=(WebcamCapturer &&) noexcept;

    /// Open the webcam device. Creates a private OBS source.
    /// @param device_id  device identifier ("default" or device name)
    /// @param width   desired capture width (e.g. 1280)
    /// @param height  desired capture height (e.g. 720)
    /// @param fps     desired frame rate (e.g. 30)
    /// @return true if the source was created successfully
    bool open(const std::string &device_id, int width, int height, int fps);

    /// Close the webcam and release the private OBS source.
    void close();

    /// Whether the webcam is currently open and producing frames.
    bool isOpen() const;

    /// Get the latest frame from the webcam.
    /// Called from video_tick (CPU thread — NOT the render thread).
    /// Returns an invalid frame if no frame is available (stub mode or
    /// webcam not open). The returned frame is a COPY (owned by caller)
    /// with linesize == width*4 (no padding).
    WebcamFrame getFrame();

    /// Get the capture width.
    int width() const;

    /// Get the capture height.
    int height() const;

    // --- Static helpers (testable in stub mode) ----------------------------

    /// Copy a source frame respecting the linesize (row stride).
    /// Source may have linesize > width*4 (padding); output is tightly packed.
    /// This is the critical linesize-respecting copy that MUST be used
    /// when consuming obs_source_frame->data[0] (leçon bug 10 du brief).
    ///
    /// @param src_data    source pixel buffer (BGRA)
    /// @param src_linesize  row stride in bytes (may include padding)
    /// @param width       image width in pixels
    /// @param height      image height in pixels
    /// @param bpp         bytes per pixel (4 for BGRA)
    /// @return tightly packed copy (width * height * bpp bytes)
    static std::vector<uint8_t> copyFrameRespectingLinesize(
        const uint8_t *src_data, int src_linesize,
        int width, int height, int bpp = 4);

    /// Produce a black frame of the given dimensions (for stub mode / fallback).
    static WebcamFrame createBlackFrame(int width, int height);

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace dancehap