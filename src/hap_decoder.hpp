// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// hap_decoder.hpp — HAP frame decoder: Snappy decompress + GPU texture upload.
//
// Phase 1.3 deliverable (PLAN-PHASE-1.md etape 1.3):
//   Takes a raw HAP packet (output of HapDemuxer::readNextVideoPacket) and
//   produces an OBS gs_texture_t ready for rendering (Phase 1.4).
//
// Pipeline:
//   1. Parse HAP frame header (chunk FourCC, multi-section layout)
//   2. Snappy decompress the texture data
//   3. Upload decompressed DXT/BC data as a gs_texture (OBS graphics API)
//
// Two build modes (mirrors hap_demuxer pattern):
//   • DANCEHAP_HAVE_SNAPPY defined → real implementation using libsnappy +
//     OBS graphics API (gs_texture_create).
//   • Undefined (stub mode) → header parsing only (no Snappy, no GPU).
//     decode() returns false but never crashes. Tests can verify header
//     parsing and metadata extraction without Snappy or a GPU.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "hap_demuxer.hpp"  // DemuxPacket, VideoInfo, HapVariant
#include "obs_compat.hpp"   // gs_texture_t (opaque in stub mode)

namespace dancehap {

// ---------------------------------------------------------------------------
// GPU compressed texture format — maps to OBS gs_color_format values.
//   DXT1 → HAP  (S3TC, no alpha)
//   DXT5 → HAPA (S3TC with alpha)
//   BC7  → HAPQ / HAPQ-A (project design decision; see ARCHITECTURE.md §6)
// ---------------------------------------------------------------------------

enum class HapTextureFormat {
    Unknown,
    DXT1,    // HAP
    DXT5,    // HAPA
    BC7,     // HAPQ / HAPQ-A
};

inline const char *hap_tex_format_to_string(HapTextureFormat f)
{
    switch (f) {
    case HapTextureFormat::DXT1: return "DXT1";
    case HapTextureFormat::DXT5: return "DXT5";
    case HapTextureFormat::BC7:  return "BC7";
    default:                     return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// HAP frame header parse result.
//
// Exposed publicly so unit tests can verify header parsing without Snappy.
// The HAP frame format (from https://hap.video/ and FFmpeg libavcodec/hap.c):
//
//   Single-chunk frame:
//     [4B BE32: remaining]   length of rest of frame (excl. this field)
//     [4B FourCC: Hap1/Hap5/HapY/HapA]  texture format
//     [remaining-4 bytes]    Snappy-compressed DXT/BC data
//
//   Multi-section frame (HapM):
//     [4B BE32: remaining]
//     [4B FourCC: HapM]
//     [sections...]  each section: [4B BE32 sec_remaining][4B sec FourCC][data]
//       Section "HapS" (texture):
//         [4B texture FourCC (Hap1/Hap5/HapY/HapA)]
//         [compressed data...]
// ---------------------------------------------------------------------------

struct HapFrameInfo {
    HapVariant        variant          = HapVariant::Unknown;
    HapTextureFormat  format           = HapTextureFormat::Unknown;
    const uint8_t    *compressed_data  = nullptr;
    size_t            compressed_size  = 0;
    bool              valid            = false;
};

/// Parse a HAP frame header (byte-level, no Snappy or GPU needed).
/// Works in both stub and real modes — pure data inspection.
HapFrameInfo parse_hap_frame(const uint8_t *data, size_t size);

// ---------------------------------------------------------------------------
// HapDecoder — non-copyable, movable.
//
// Typical usage:
//   HapDecoder decoder;
//   decoder.setVideoInfo(demuxer.getVideoInfo());
//   DemuxPacket pkt = demuxer.readNextVideoPacket();
//   if (decoder.decode(pkt)) {
//       gs_texture_t *tex = decoder.getTexture();
//       // ... use tex in video_render
//   }
// ---------------------------------------------------------------------------

class HapDecoder {
public:
    HapDecoder();
    ~HapDecoder();

    HapDecoder(const HapDecoder &) = delete;
    HapDecoder &operator=(const HapDecoder &) = delete;

    HapDecoder(HapDecoder &&) noexcept;
    HapDecoder &operator=(HapDecoder &&) noexcept;

    /// Set expected video info (dimensions + variant from demuxer).
    /// Must be called before decode() for the decoder to know the
    /// texture dimensions (which are not stored in the HAP frame itself).
    void setVideoInfo(const VideoInfo &vi);

    /// Decode a HAP packet into a GPU texture.
    /// Returns true on success.
    /// On error: keeps the previous texture (no crash), sets last_error,
    /// returns false.
    bool decode(const DemuxPacket &packet);

    /// Current texture handle.
    /// Real mode: valid gs_texture_t after successful decode().
    /// Stub mode: always nullptr (no GPU).
    gs_texture_t *getTexture() const;

    /// Texture format of the last successfully decoded frame.
    HapTextureFormat getFormat() const;

    /// Dimensions of the current texture.
    int getWidth() const;
    int getHeight() const;

    /// Last error message (empty if no error).
    const std::string &getLastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace dancehap
