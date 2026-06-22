// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// hap_decoder.cpp — HAP frame decoder: Snappy decompress + GPU texture upload.
//
// Phase 1.3: take a raw HAP packet, parse the frame header, Snappy-decompress
// the texture data, and upload as a gs_texture_t (OBS compressed format).
//
// Build modes:
//   DANCEHAP_HAVE_SNAPPY → real implementation using libsnappy +
//                          OBS gs_texture_create (requires DANCEHAP_HAVE_OBS).
//   Otherwise            → stub: parse header only, decode() returns false,
//                          getTexture() returns nullptr.

#include "hap_decoder.hpp"
#include "obs_compat.hpp"   // blog()

#include <algorithm>
#include <cstring>

// ===========================================================================
//  Shared header parsing (works in both modes — pure byte inspection)
//
//  Implements the Vidvox HAP "section header" format, matching FFmpeg's
//  libavcodec/hap.c::ff_hap_parse_section_header.
//
//  Layout:
//    bytes 0-2 (LE uint24): section_size
//    byte  3:               section_type
//    if section_size == 0:
//        bytes 4-7 (LE uint32): real section_size  (extended header)
//
//  Top-level section type byte values we recognise (see HAP spec table):
//    0xBB HAP   RGB DXT1 Snappy    (codec_tag "Hap1")
//    0xBE HAPA  RGBA DXT5 Snappy   (codec_tag "Hap5")
//    0xBF HAPQ  YCoCg DXT5 Snappy  (codec_tag "HapY")
//    0xAE/0xAF  same formats with "None" compressor (not Snappy)
//    0x0D       HapM multi-image container (HapQ-A = YCoCg + Alpha)
//
//  Compressor codes (high nibble of section_type):
//    0xA0  None
//    0xB0  Snappy
//    0xC0  Complex (decode-instructions)
//
//  Pixel format codes (low nibble of section_type):
//    0x0B  RGB DXT1     (Hap1)
//    0x0E  RGBA DXT5    (Hap5)
//    0x0F  YCoCg DXT5   (HapY)
//    0x0C  RGBA BC7     (Hap7)
//    0x01  Alpha RGTC1  (HapA)
// ===========================================================================

namespace dancehap {

// HAP section type byte values (Vidvox Hap Video spec — Top-Level Sections table).
static constexpr uint8_t HAP_TYPE_HAP_DXT1_SNAPPY   = 0xBB;  // Hap1
static constexpr uint8_t HAP_TYPE_HAPA_DXT5_SNAPPY  = 0xBE;  // Hap5
static constexpr uint8_t HAP_TYPE_HAPQ_DXT5_SNAPPY  = 0xBF;  // HapY
static constexpr uint8_t HAP_TYPE_MULTI_IMAGE       = 0x0D;  // HapM container

// Read little-endian uint24 (3 bytes).
static uint32_t read_le24(const uint8_t *p)
{
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16);
}

// Read little-endian uint32 (4 bytes).
static uint32_t read_le32(const uint8_t *p)
{
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

// Parse a HAP section header at `data`. On success, fills `section_size`
// (payload size in bytes, excluding header) and `section_type`, and returns
// the number of header bytes consumed (4 or 8). Returns 0 on error.
static size_t parse_section_header(const uint8_t *data, size_t size,
                                   uint32_t &section_size, uint8_t &section_type)
{
    if (!data || size < 4) return 0;

    uint32_t sz = read_le24(data);
    uint8_t  ty = data[3];

    size_t header_len = 4;
    if (sz == 0) {
        // Extended 8-byte header: real size is in bytes 4-7 (LE uint32).
        if (size < 8) return 0;
        sz = read_le32(data + 4);
        header_len = 8;
    }

    section_size = sz;
    section_type = ty;
    return header_len;
}

// Map a HAP section_type byte (top-level Snappy variant) to a
// (HapVariant, HapTextureFormat) pair. Returns false if the type is not a
// recognised single-image top-level Snappy section.
static bool section_type_to_variant_format(uint8_t section_type,
                                           HapVariant &variant,
                                           HapTextureFormat &format)
{
    switch (section_type) {
    case HAP_TYPE_HAP_DXT1_SNAPPY:    // 0xBB
        variant = HapVariant::HAP;
        format  = HapTextureFormat::DXT1;
        return true;
    case HAP_TYPE_HAPA_DXT5_SNAPPY:   // 0xBE
        variant = HapVariant::HAPA;
        format  = HapTextureFormat::DXT5;
        return true;
    case HAP_TYPE_HAPQ_DXT5_SNAPPY:   // 0xBF
        variant = HapVariant::HAPQ;
        format  = HapTextureFormat::BC7;   // OBS 31 lacks BC7 → fallback DXT5 at upload
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------+
// parse_hap_frame — public free function
// ---------------------------------------------------------------------------+

HapFrameInfo parse_hap_frame(const uint8_t *data, size_t size)
{
    HapFrameInfo info;

    if (!data || size < 4) return info;  // need at least the 4-byte header

    uint32_t section_size = 0;
    uint8_t  section_type = 0;
    size_t   header_len   = parse_section_header(data, size,
                                                 section_size, section_type);
    if (header_len == 0) return info;

    // Clamp section_size to the bytes actually available after the header.
    size_t avail = (size > header_len) ? (size - header_len) : 0;
    size_t payload_size = std::min(static_cast<size_t>(section_size), avail);

    // HapM multi-image container: type 0x0D.
    // Per spec, the container holds 2 nested top-level sections (YCoCg DXT5 +
    // Alpha RGTC1). For MVP we decode only the first (colour) plane; the
    // alpha plane is Phase 5 polish. We still report variant=HAPQ_A to match
    // the container codec_tag.
    if (section_type == HAP_TYPE_MULTI_IMAGE) {
        // Iterate nested sections, pick the first we recognise.
        size_t offset = header_len;
        size_t end    = header_len + payload_size;
        while (offset + 4 <= end) {
            uint32_t sub_size = 0;
            uint8_t  sub_type = 0;
            size_t   sub_hlen = parse_section_header(data + offset, end - offset,
                                                     sub_size, sub_type);
            if (sub_hlen == 0) break;

            size_t sub_payload = std::min(static_cast<size_t>(sub_size),
                                          end - offset - sub_hlen);
            HapVariant v = HapVariant::Unknown;
            HapTextureFormat f = HapTextureFormat::Unknown;
            if (section_type_to_variant_format(sub_type, v, f)) {
                info.variant         = HapVariant::HAPQ_A;  // container variant
                info.format          = f;
                info.compressed_data = data + offset + sub_hlen;
                info.compressed_size = sub_payload;
                info.valid           = true;
                return info;
            }
            offset += sub_hlen + sub_payload;
        }
        // No recognised nested section.
        return info;
    }

    // Single-image top-level section.
    HapVariant v = HapVariant::Unknown;
    HapTextureFormat f = HapTextureFormat::Unknown;
    if (!section_type_to_variant_format(section_type, v, f)) {
        return info;  // unknown type (None/Complex compressors not yet supported)
    }

    info.variant         = v;
    info.format          = f;
    info.compressed_data = data + header_len;
    info.compressed_size = payload_size;
    info.valid           = (payload_size > 0);
    return info;
}

} // namespace dancehap

// ===========================================================================
//  Real implementation (Snappy + OBS graphics)
// ===========================================================================
#ifdef DANCEHAP_HAVE_SNAPPY

#include <snappy.h>

#ifdef DANCEHAP_HAVE_OBS
#include <graphics/graphics.h>
#endif

namespace dancehap {

// Map HapTextureFormat to OBS gs_color_format.
// OBS 31 gs_color_format supports GS_DXT1, GS_DXT3, GS_DXT5 but NOT BC7.
// HAPQ (DXT5-YCoCg) maps to DXT5 — the YCoCg color space conversion happens
// in a shader (Phase 5 polish). For now the raw DXT5 bytes are uploaded,
// which will display with wrong colors for HAPQ but won't crash.
#ifdef DANCEHAP_HAVE_OBS
static gs_color_format map_to_gs_format(HapTextureFormat fmt)
{
    switch (fmt) {
    case HapTextureFormat::DXT1: return GS_DXT1;
    case HapTextureFormat::DXT5: return GS_DXT5;
    case HapTextureFormat::BC7:  return GS_DXT5;  // TODO Phase 5: BC7 not in OBS, fallback DXT5
    default:                     return GS_UNKNOWN;
    }
}
#endif

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct HapDecoder::Impl {
    VideoInfo          video_info;
    gs_texture_t      *texture     = nullptr;
    HapTextureFormat   tex_format  = HapTextureFormat::Unknown;
    int                tex_width   = 0;
    int                tex_height  = 0;
    std::string        last_error;
    std::vector<uint8_t> decompressed;  // Snappy output buffer (reused)

    ~Impl()
    {
#ifdef DANCEHAP_HAVE_OBS
        if (texture) {
            gs_texture_destroy(texture);
            texture = nullptr;
        }
#endif
    }
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

HapDecoder::HapDecoder()
    : pimpl_(std::make_unique<Impl>())
{
}

HapDecoder::~HapDecoder() = default;

HapDecoder::HapDecoder(HapDecoder &&) noexcept = default;
HapDecoder &HapDecoder::operator=(HapDecoder &&) noexcept = default;

// ---------------------------------------------------------------------------
// setVideoInfo
// ---------------------------------------------------------------------------

void HapDecoder::setVideoInfo(const VideoInfo &vi)
{
    pimpl_->video_info = vi;
}

// ---------------------------------------------------------------------------
// decode
// ---------------------------------------------------------------------------

bool HapDecoder::decode(const DemuxPacket &packet)
{
    if (!packet.valid || packet.data.empty()) {
        pimpl_->last_error = "invalid or empty packet";
        return false;
    }

    // --- Parse HAP frame header ---
    HapFrameInfo fi = parse_hap_frame(packet.data.data(), packet.data.size());
    if (!fi.valid) {
        pimpl_->last_error = "invalid HAP frame header";
        blog(LOG_WARNING, "[DanceHAP] HapDecoder: invalid frame header");
        return false;
    }

    if (fi.compressed_size == 0) {
        pimpl_->last_error = "no compressed data in frame";
        return false;
    }

    // --- Snappy decompress ---
    size_t uncompressed_len = 0;
    if (!snappy::GetUncompressedLength(
            reinterpret_cast<const char *>(fi.compressed_data),
            fi.compressed_size,
            &uncompressed_len)) {
        pimpl_->last_error = "snappy GetUncompressedLength failed";
        blog(LOG_WARNING, "[DanceHAP] HapDecoder: snappy header parse failed");
        return false;
    }

    pimpl_->decompressed.resize(uncompressed_len);
    if (!snappy::RawUncompress(
            reinterpret_cast<const char *>(fi.compressed_data),
            fi.compressed_size,
            reinterpret_cast<char *>(pimpl_->decompressed.data()))) {
        pimpl_->last_error = "snappy RawUncompress failed";
        blog(LOG_WARNING, "[DanceHAP] HapDecoder: snappy decompress failed");
        return false;
    }

    // --- Upload to GPU texture ---
    int w = pimpl_->video_info.width;
    int h = pimpl_->video_info.height;
    if (w <= 0 || h <= 0) {
        pimpl_->last_error = "video dimensions not set (call setVideoInfo)";
        return false;
    }

#ifdef DANCEHAP_HAVE_OBS
    gs_color_format gs_fmt = map_to_gs_format(fi.format);

    // Recreate texture if dimensions changed or first decode.
    // OBS does not expose in-place update for compressed (DXT/BC) textures,
    // so we destroy + recreate on every decode. This is acceptable for the
    // MVP — Phase 5 can investigate staging-texture optimisation.
    bool dims_changed = (pimpl_->tex_width != w || pimpl_->tex_height != h);
    if (pimpl_->texture && dims_changed) {
        gs_texture_destroy(pimpl_->texture);
        pimpl_->texture = nullptr;
    }

    if (!pimpl_->texture) {
        const uint8_t *tex_data[] = { pimpl_->decompressed.data() };
        pimpl_->texture = gs_texture_create(
            static_cast<uint32_t>(w),
            static_cast<uint32_t>(h),
            gs_fmt, 1, tex_data, GS_DYNAMIC);
        if (!pimpl_->texture) {
            pimpl_->last_error = "gs_texture_create failed";
            blog(LOG_ERROR, "[DanceHAP] HapDecoder: gs_texture_create failed "
                 "(%dx%d %s)", w, h, hap_tex_format_to_string(fi.format));
            return false;
        }
        pimpl_->tex_width  = w;
        pimpl_->tex_height = h;
    } else {
        // Same dimensions: destroy + recreate to update compressed content.
        gs_texture_destroy(pimpl_->texture);
        const uint8_t *tex_data[] = { pimpl_->decompressed.data() };
        pimpl_->texture = gs_texture_create(
            static_cast<uint32_t>(w),
            static_cast<uint32_t>(h),
            gs_fmt, 1, tex_data, GS_DYNAMIC);
    }
#else
    // Snappy available but OBS graphics not (unusual build config).
    // Decompression succeeded but we can't create a texture.
    pimpl_->last_error = "OBS graphics not available for texture upload";
#endif

    pimpl_->tex_format = fi.format;
    pimpl_->last_error.clear();
    blog(LOG_DEBUG, "[DanceHAP] HapDecoder: decoded %dx%d %s (%zu bytes → %zu)",
         w, h, hap_tex_format_to_string(fi.format),
         fi.compressed_size, pimpl_->decompressed.size());
    return true;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

gs_texture_t *HapDecoder::getTexture() const  { return pimpl_->texture; }
HapTextureFormat HapDecoder::getFormat() const { return pimpl_->tex_format; }
int HapDecoder::getWidth() const               { return pimpl_->tex_width; }
int HapDecoder::getHeight() const              { return pimpl_->tex_height; }
const std::string &HapDecoder::getLastError() const { return pimpl_->last_error; }

} // namespace dancehap

// ===========================================================================
//  Stub implementation (no Snappy)
// ===========================================================================
#else // !DANCEHAP_HAVE_SNAPPY

namespace dancehap {

// ---------------------------------------------------------------------------
// Impl (stub: stores parsed metadata but no texture)
// ---------------------------------------------------------------------------

struct HapDecoder::Impl {
    VideoInfo          video_info;
    HapTextureFormat   tex_format  = HapTextureFormat::Unknown;
    int                tex_width   = 0;
    int                tex_height  = 0;
    std::string        last_error;
    HapVariant         last_variant = HapVariant::Unknown;  // last parsed
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

HapDecoder::HapDecoder()
    : pimpl_(std::make_unique<Impl>())
{
}

HapDecoder::~HapDecoder() = default;

HapDecoder::HapDecoder(HapDecoder &&) noexcept = default;
HapDecoder &HapDecoder::operator=(HapDecoder &&) noexcept = default;

// ---------------------------------------------------------------------------
// setVideoInfo
// ---------------------------------------------------------------------------

void HapDecoder::setVideoInfo(const VideoInfo &vi)
{
    pimpl_->video_info = vi;
}

// ---------------------------------------------------------------------------
// decode (stub: parse header, no Snappy, no texture)
// ---------------------------------------------------------------------------

bool HapDecoder::decode(const DemuxPacket &packet)
{
    // Always propagate the demuxer-provided video dimensions so callers can
    // rely on getWidth()/getHeight() even when the frame parse fails. The
    // dimensions come from the container, not the frame payload.
    pimpl_->tex_width  = pimpl_->video_info.width;
    pimpl_->tex_height = pimpl_->video_info.height;

    if (!packet.valid || packet.data.empty()) {
        pimpl_->last_error = "invalid or empty packet";
        return false;
    }

    // Parse the HAP frame header — this works without Snappy and lets us
    // verify the frame structure + variant in unit tests.
    HapFrameInfo fi = parse_hap_frame(packet.data.data(), packet.data.size());

    if (!fi.valid) {
        pimpl_->last_error = "invalid HAP frame header";
        return false;
    }

    // Record parsed metadata for test inspection.
    pimpl_->last_variant = fi.variant;
    pimpl_->tex_format   = fi.format;

    // Stub mode: no Snappy → cannot decompress → decode fails gracefully.
    pimpl_->last_error = "stub mode: Snappy not available, decode skipped";
    return false;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

gs_texture_t *HapDecoder::getTexture() const       { return nullptr; }
HapTextureFormat HapDecoder::getFormat() const      { return pimpl_->tex_format; }
int HapDecoder::getWidth() const                    { return pimpl_->tex_width; }
int HapDecoder::getHeight() const                   { return pimpl_->tex_height; }
const std::string &HapDecoder::getLastError() const { return pimpl_->last_error; }

} // namespace dancehap

#endif // DANCEHAP_HAVE_SNAPPY
