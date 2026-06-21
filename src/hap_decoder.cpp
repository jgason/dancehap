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
// ===========================================================================

namespace dancehap {

// Build a 32-bit FourCC tag in little-endian byte order (matches FFmpeg MKTAG).
static constexpr uint32_t mk_tag(char a, char b, char c, char d)
{
    return (uint32_t)(uint8_t)(a)
         | ((uint32_t)(uint8_t)(b) << 8)
         | ((uint32_t)(uint8_t)(c) << 16)
         | ((uint32_t)(uint8_t)(d) << 24);
}

// HAP chunk FourCCs as stored in frame data (read as le32 on little-endian).
static constexpr uint32_t HAP_FCC_HAP1 = mk_tag('H', 'a', 'p', '1'); // HAP  DXT1
static constexpr uint32_t HAP_FCC_HAP5 = mk_tag('H', 'a', 'p', '5'); // HAPA DXT5
static constexpr uint32_t HAP_FCC_HAPY = mk_tag('H', 'a', 'p', 'Y'); // HAPQ DXT5-YCoCg
static constexpr uint32_t HAP_FCC_HAPA = mk_tag('H', 'a', 'p', 'A'); // HAPQ-A
static constexpr uint32_t HAP_FCC_HAPM = mk_tag('H', 'a', 'p', 'M'); // Multi-section container
static constexpr uint32_t HAP_FCC_HAPS = mk_tag('H', 'a', 'p', 'S'); // Texture section (in HapM)
static constexpr uint32_t HAP_FCC_HAPC = mk_tag('H', 'a', 'p', 'C'); // Compressor section

// Read big-endian uint32 (HAP stores sizes in big-endian per the spec).
static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24)
         | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)
         |  (uint32_t)p[3];
}

// Read little-endian uint32 (HAP FourCCs read as le32 on little-endian host).
static uint32_t read_le32(const uint8_t *p)
{
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

// Map a HAP chunk FourCC to a HapVariant.
static HapVariant fourcc_to_variant(uint32_t fcc)
{
    switch (fcc) {
    case HAP_FCC_HAP1: return HapVariant::HAP;
    case HAP_FCC_HAP5: return HapVariant::HAPA;
    case HAP_FCC_HAPY: return HapVariant::HAPQ;
    case HAP_FCC_HAPA: return HapVariant::HAPQ_A;
    default:           return HapVariant::Unknown;
    }
}

// Map a HapVariant to the GPU texture format.
// Per ARCHITECTURE.md §6 and PLAN-PHASE-1.md 1.3:
//   HAP  → DXT1, HAPA → DXT5, HAPQ/HAPQ-A → BC7
static HapTextureFormat variant_to_tex_format(HapVariant v)
{
    switch (v) {
    case HapVariant::HAP:    return HapTextureFormat::DXT1;
    case HapVariant::HAPA:   return HapTextureFormat::DXT5;
    case HapVariant::HAPQ:   return HapTextureFormat::BC7;
    case HapVariant::HAPQ_A: return HapTextureFormat::BC7;
    default:                 return HapTextureFormat::Unknown;
    }
}

// ---------------------------------------------------------------------------
// parse_hap_frame — public free function
// ---------------------------------------------------------------------------

HapFrameInfo parse_hap_frame(const uint8_t *data, size_t size)
{
    HapFrameInfo info;

    if (!data || size < 8) return info;  // need at least length + FourCC

    uint32_t frame_remaining = read_be32(data);     // bytes 0-3 (big-endian)
    uint32_t frame_fcc       = read_le32(data + 4);  // bytes 4-7 (FourCC)

    // Total meaningful data = 4 (length field) + frame_remaining.
    // Clamp to the actual buffer size to stay safe with truncated data.
    size_t frame_end = std::min(static_cast<size_t>(4u) + frame_remaining, size);

    if (frame_fcc == HAP_FCC_HAPM) {
        // --- Multi-section container ---
        // Iterate sub-sections looking for "HapS" (texture section).
        size_t offset = 8;  // skip length field + "HapM" FourCC
        while (offset + 8 <= frame_end) {
            uint32_t sec_remaining = read_be32(data + offset);
            uint32_t sec_fcc       = read_le32(data + offset + 4);

            // Section spans [offset, offset + 4 + sec_remaining)
            // (4 bytes for the length field, sec_remaining for the rest).
            size_t sec_end = offset + 4u + sec_remaining;
            if (sec_end > frame_end) sec_end = frame_end;

            if (sec_fcc == HAP_FCC_HAPS && offset + 12 <= sec_end) {
                // Texture section: [4B tex FourCC] [compressed data...]
                uint32_t tex_fcc = read_le32(data + offset + 8);
                info.variant = fourcc_to_variant(tex_fcc);
                if (info.variant != HapVariant::Unknown) {
                    info.format          = variant_to_tex_format(info.variant);
                    info.compressed_data = data + offset + 12;
                    info.compressed_size = sec_end - (offset + 12);
                    info.valid           = true;
                }
                return info;
            }

            // Advance to next section.
            offset = sec_end;
        }
        // No texture section found.
        return info;
    }

    // --- Single-chunk frame: FourCC IS the texture format ---
    info.variant = fourcc_to_variant(frame_fcc);
    if (info.variant == HapVariant::Unknown) return info;

    info.format          = variant_to_tex_format(info.variant);
    info.compressed_data = data + 8;
    // frame_remaining includes the 4-byte FourCC, so data = remaining - 4.
    size_t data_avail = (frame_end > 8) ? (frame_end - 8) : 0;
    if (frame_remaining >= 4) {
        info.compressed_size = std::min(
            static_cast<size_t>(frame_remaining - 4u), data_avail);
    } else {
        info.compressed_size = 0;
    }
    info.valid = true;
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
#ifdef DANCEHAP_HAVE_OBS
static gs_color_format map_to_gs_format(HapTextureFormat fmt)
{
    switch (fmt) {
    case HapTextureFormat::DXT1: return GS_DXT1;
    case HapTextureFormat::DXT5: return GS_DXT5;
    case HapTextureFormat::BC7:  return GS_BC7;
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
    pimpl_->tex_width    = pimpl_->video_info.width;
    pimpl_->tex_height   = pimpl_->video_info.height;

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
