// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// hap_demuxer.hpp — Container demuxer for HAP video clips.
//
// Phase 1.2 deliverable (PLAN-PHASE-1.md etape 1.2):
//   Opens a .mov/.mp4 container, detects the HAP video stream variant
//   (HAP / HAPA / HAPQ / HAPQ-A) and optional audio stream, and provides
//   sequential raw packet reading. Decoding (Snappy decompression) arrives
//   in Phase 1.3 — this class only reads raw compressed packets.
//
// Two build modes (mirrors obs_compat.hpp pattern):
//   • DANCEHAP_HAVE_FFMPEG defined → real implementation using libavformat.
//   • Undefined (stub mode) → synthetic implementation that validates file
//     existence + ISO-BMFF container signature, then returns known metadata
//     matching tests/assets/sample_hapa_5s.mov. Enables unit testing without
//     FFmpeg installed.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dancehap {

// ---------------------------------------------------------------------------
// HAP codec variant — detected from container FourCC (codec_tag).
//   Hap1 → HAP     (DXT1, no alpha)
//   Hap5 → HAPA    (DXT5, alpha)
//   HapY → HAPQ    (DXT5-YCoCg, no alpha)
//   HapA → HAPQ-A  (DXT5-YCoCg, alpha)
// ---------------------------------------------------------------------------

enum class HapVariant {
    Unknown,
    HAP,
    HAPA,
    HAPQ,
    HAPQ_A,
};

inline const char *hap_variant_to_string(HapVariant v)
{
    switch (v) {
    case HapVariant::HAP:    return "HAP";
    case HapVariant::HAPA:   return "HAPA";
    case HapVariant::HAPQ:   return "HAPQ";
    case HapVariant::HAPQ_A: return "HAPQ-A";
    default:                 return "Unknown";
    }
}

inline bool hap_variant_has_alpha(HapVariant v)
{
    return v == HapVariant::HAPA || v == HapVariant::HAPQ_A;
}

// ---------------------------------------------------------------------------
// Stream info structs
// ---------------------------------------------------------------------------

struct VideoInfo {
    HapVariant variant   = HapVariant::Unknown;
    int  width           = 0;
    int  height          = 0;
    int  fps_num         = 0;   // frame rate numerator
    int  fps_den         = 0;   // frame rate denominator
    int64_t duration_us  = 0;   // duration in microseconds
};

struct AudioInfo {
    int  sample_rate     = 0;
    int  channels        = 0;
    std::string codec_name;
    int64_t duration_us  = 0;
};

// ---------------------------------------------------------------------------
// Raw compressed packet
// ---------------------------------------------------------------------------

struct DemuxPacket {
    std::vector<uint8_t> data;
    int64_t pts_us       = 0;    // presentation timestamp (microseconds)
    int64_t dts_us       = 0;    // decode timestamp (microseconds)
    int  stream_index    = -1;   // which stream this packet belongs to
    bool key_frame       = false;
    bool valid           = false; // false = EOF or error (no more packets)

    bool isValid() const { return valid; }
};

// ---------------------------------------------------------------------------
// Demuxer state machine
//
//   Idle → Loading → Ready    (open succeeded)
//                    ↘ Error  (open failed: file not found, bad format, ...)
//
//   Ready/Error → Idle         (close called)
//
//   Playing state arrives in Phase 1.4.
// ---------------------------------------------------------------------------

enum class DemuxState {
    Idle,
    Loading,
    Ready,
    Error,
};

// ---------------------------------------------------------------------------
// HapDemuxer — non-copyable, movable.
// ---------------------------------------------------------------------------

class HapDemuxer {
public:
    HapDemuxer();
    ~HapDemuxer();

    HapDemuxer(const HapDemuxer &) = delete;
    HapDemuxer &operator=(const HapDemuxer &) = delete;

    HapDemuxer(HapDemuxer &&) noexcept;
    HapDemuxer &operator=(HapDemuxer &&) noexcept;

    /// Open a container file and detect streams.
    /// Returns true on success (at least a video HAP stream found).
    /// On failure: state → Error, last_error set, returns false.
    bool open(const std::string &path);

    /// Close and release all resources. Safe to call on un-opened demuxer.
    void close();

    /// Close current file then open a new one.
    bool reopen(const std::string &path);

    // --- Stream queries (valid after open) ---

    bool hasVideo() const;
    bool hasAudio() const;
    const VideoInfo &getVideoInfo() const;
    const AudioInfo &getAudioInfo() const;
    DemuxState getState() const;
    const std::string &getLastError() const;

    /// Duration in seconds (0.0 if unknown).
    double getDuration() const;

    // --- Packet reading (sequential cursor, interleaved) ---

    /// Read the next video packet. Non-video packets encountered are
    /// internally buffered so a subsequent readNextAudioPacket() can return
    /// them. Returns DemuxPacket with valid=false at EOF.
    DemuxPacket readNextVideoPacket();

    /// Read the next audio packet. Non-audio packets are buffered for
    /// subsequent video reads. Returns DemuxPacket with valid=false at EOF
    /// or if no audio stream exists.
    DemuxPacket readNextAudioPacket();

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace dancehap
