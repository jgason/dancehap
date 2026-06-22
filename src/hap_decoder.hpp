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
//
// HAP frame format — per Vidvox Hap Video spec
// (https://github.com/Vidvox/hap/blob/master/documentation/HapVideoDRAFT.md)
// and mirrored by FFmpeg libavcodec/hap.c::ff_hap_parse_section_header:
//
//   Section header (variable length):
//     [3 bytes LE uint24: section_size]  if non-zero → header is 4 bytes total
//     [1 byte: section_type]             (e.g. 0xBE = RGBA DXT5 Snappy)
//     -- OR --
//     [3 bytes zero]                     if section_size==0 → header is 8 bytes
//     [1 byte: section_type]
//     [4 bytes LE uint32: real section_size]
//
//   Top-level section types (only the relevant ones for our MVP):
//     0xBB  RGB  DXT1 Snappy   (Hap1)
//     0xBE  RGBA DXT5 Snappy   (Hap5)  ← our test asset uses this
//     0xBF  YCoCg DXT5 Snappy  (HapY)
//     0xAE/0xAF/0xAC  …None variants
//     0xCE/0xCF/0xCC  …Complex (decode instructions) variants
//     0x0D  Multiple-image container (HapM)
//
//   After the header, section_size bytes of payload follow. For Snappy
//   top-level types, the payload is a single Snappy stream that decompresses
//   to the DXT/BC texture data. For 0x0D containers, payload is a sequence
//   of nested top-level sections (one per image plane).
//
// IMPORTANT: the codec variant (HAP / HAPA / HAPQ / HAPQ-A) is stored in
// the MOV container's stsd atom as a codec_tag (Hap1/Hap5/HapY/HapA/HapM),
// NOT inside each frame. parse_hap_frame() therefore maps the section_type
// byte to a (variant, format) pair using the spec table. A mismatch between
// the container's codec_tag and the frame's section_type is a decode error.
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

    /// Decode a HAP packet (CPU-only: Snappy decompress).
    /// Safe to call from any thread (typically the video_tick thread).
    /// Does NOT touch the GPU — the decompressed DXT/BC data is buffered
    /// internally and uploaded on the next uploadToGpu() call.
    /// Returns true on success.
    bool decode(const DemuxPacket &packet);

    /// Upload the latest decompressed frame to the GPU as a gs_texture.
    /// MUST be called from the OBS graphics thread (i.e. from
    /// hap_clip_video_render). Calling it from video_tick will fail on
    /// Windows OBS 31 because no graphics context is active there.
    /// No-op if no frame is pending or OBS graphics are unavailable.
    void uploadToGpu();

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
