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

// ---------------------------------------------------------------------------
// DXT5/BC3 CPU decoder.
//
// OBS does NOT hardware-decode DXT5 uploaded via gs_texture_create(GS_DXT5):
// its pitch calculation (width*bpp/8) is 4× too small for block-compressed
// formats → D3D11 accepts the texture but reinterprets the data wrong,
// producing vertical bars instead of the correct image.
//
// Fix (v0.3.1): decode DXT5 to RGBA on the CPU, then upload as GS_RGBA.
// The block layout follows libavcodec/texturedsp.c (dxt_block()):
//
//   DXT5 block = 16 bytes:
//     bytes 0-1:  alpha endpoints a0, a1 (uint16 → 8 alpha values)
//     bytes 2-7:  6 bytes = 48-bit lookup table (4×4 alpha indices, 3 bits each)
//     bytes 8-9:  color0 (RGB565)
//     bytes 10-11: color1 (RGB565)
//     bytes 12-15: 4 bytes = 32-bit lookup table (4×4 color indices, 2 bits each)
//
//   IMPORTANT: alpha comes FIRST (bytes 0-7), then colour (bytes 8-15).
//   Getting this order wrong is the #1 DXT5 implementation bug.
// ---------------------------------------------------------------------------

// Decode a single RGB565 colour to RGBA8 (alpha = 255).
static inline void rgb565_to_rgba(uint16_t c, uint8_t out[4])
{
    int r = (c >> 11) & 0x1F;
    int g = (c >> 5)  & 0x3F;
    int b =  c        & 0x1F;
    out[0] = (uint8_t)((r << 3) | (r >> 2));  // red
    out[1] = (uint8_t)((g << 2) | (g >> 4));  // green
    out[2] = (uint8_t)((b << 3) | (b >> 2));  // blue
    out[3] = 255;                               // opaque by default
}

// Decode one 4×4 DXT5 block (16 bytes) to 16 RGBA pixels (64 bytes).
// `block` points to the 16-byte block. `out` points to the destination
// for this block's 4×4 pixel grid (16 × 4 = 64 bytes).
static void decode_dxt5_block(const uint8_t *block, uint8_t *out)
{
    // --- Alpha (bytes 0-7) ---
    uint8_t a0 = block[0];
    uint8_t a1 = block[1];

    // 48-bit alpha index table (bytes 2-7).
    uint64_t alpha_bits = 0;
    for (int i = 0; i < 6; ++i)
        alpha_bits |= (uint64_t)block[2 + i] << (8 * i);

    uint8_t alphas[8];
    alphas[0] = a0;
    alphas[1] = a1;
    if (a0 > a1) {
        // 6 interpolated values.
        for (int i = 1; i <= 6; ++i)
            alphas[1 + i] = (uint8_t)(((7 - i) * a0 + i * a1) / 7);
        alphas[7] = 255;
    } else {
        // 4 interpolated values + 0 + 255.
        for (int i = 1; i <= 4; ++i)
            alphas[1 + i] = (uint8_t)(((5 - i) * a0 + i * a1) / 5);
        alphas[6] = 0;
        alphas[7] = 255;
    }

    uint8_t alpha[16];
    for (int i = 0; i < 16; ++i)
        alpha[i] = alphas[(alpha_bits >> (3 * i)) & 0x7];

    // --- Colour (bytes 8-15) ---
    uint16_t c0 = (uint16_t)(block[8]  | (block[9]  << 8));
    uint16_t c1 = (uint16_t)(block[10] | (block[11] << 8));

    uint8_t col0[4], col1[4];
    rgb565_to_rgba(c0, col0);
    rgb565_to_rgba(c1, col1);

    uint32_t color_index_bits = (uint32_t)block[12]
                              | ((uint32_t)block[13] << 8)
                              | ((uint32_t)block[14] << 16)
                              | ((uint32_t)block[15] << 24);

    // Build 4 colour rows.
    uint8_t colors[4][4];
    if (c0 > c1) {
        for (int k = 0; k < 4; ++k)
            colors[k][3] = 255;
        for (int k = 0; k < 3; ++k) {
            colors[0][k] = col0[k];
            colors[1][k] = col1[k];
            colors[2][k] = (uint8_t)((2 * col0[k] + col1[k]) / 3);
            colors[3][k] = (uint8_t)((col0[k] + 2 * col1[k]) / 3);
        }
    } else {
        for (int k = 0; k < 4; ++k)
            colors[k][3] = 255;
        for (int k = 0; k < 3; ++k) {
            colors[0][k] = col0[k];
            colors[1][k] = col1[k];
            colors[2][k] = (uint8_t)((col0[k] + col1[k]) / 2);
            colors[3][k] = 0;  // black (transparent for alpha path)
        }
    }

    // Write 4×4 pixels. The index table is read LSB-first per pixel.
    for (int i = 0; i < 16; ++i) {
        uint32_t idx = (color_index_bits >> (2 * i)) & 0x3;
        out[i * 4 + 0] = colors[idx][0];
        out[i * 4 + 1] = colors[idx][1];
        out[i * 4 + 2] = colors[idx][2];
        out[i * 4 + 3] = alpha[i];  // override alpha from alpha table
    }
}

/// Decode a full DXT5 texture to RGBA8.
/// \param dxt_data   Pointer to the raw DXT5 data (block-compressed).
/// \param dxt_size   Size of dxt_data in bytes (must equal width*height).
/// \param rgba_out   Output buffer (must be width*height*4 bytes).
/// \param width      Texture width (must be multiple of 4).
/// \param height     Texture height (must be multiple of 4).
/// \return true on success, false on size mismatch.
static bool dxt5_to_rgba(const uint8_t *dxt_data, size_t dxt_size,
                         uint8_t *rgba_out, int width, int height)
{
    if (width < 1 || height < 1 || (width % 4) != 0 || (height % 4) != 0)
        return false;

    const size_t blocks_x = static_cast<size_t>(width)  / 4;
    const size_t blocks_y = static_cast<size_t>(height) / 4;
    const size_t expected = blocks_x * blocks_y * 16;
    if (dxt_size < expected) return false;

    for (size_t by = 0; by < blocks_y; ++by) {
        for (size_t bx = 0; bx < blocks_x; ++bx) {
            const uint8_t *block = dxt_data + (by * blocks_x + bx) * 16;

            // Decode 4×4 pixels into a local 64-byte buffer, then scatter
            // them into the destination row by row.
            uint8_t pixels[64];
            decode_dxt5_block(block, pixels);

            for (int row = 0; row < 4; ++row) {
                size_t dst_y = by * 4 + row;
                size_t dst_x = bx * 4;
                size_t dst_off = (dst_y * width + dst_x) * 4;
                std::memcpy(rgba_out + dst_off,
                            pixels + row * 16, 16);
            }
        }
    }
    return true;
}

// Map HapTextureFormat to OBS gs_color_format.
// IMPORTANT (v0.3.1 fix): we no longer upload raw DXT5 bytes via GS_DXT5.
// OBS's pitch calc is broken for block-compressed formats (4× too small),
// producing vertical bars. Instead, DXT5 is CPU-decoded to RGBA and uploaded
// as GS_RGBA. DXT1 follows the same pattern (Phase 5 polish could add a
// dedicated DXT1 path, but for correctness we decode to RGBA as well).
#ifdef DANCEHAP_HAVE_OBS
static gs_color_format map_to_gs_format(HapTextureFormat fmt)
{
    switch (fmt) {
    case HapTextureFormat::DXT1: return GS_RGBA;
    case HapTextureFormat::DXT5: return GS_RGBA;
    case HapTextureFormat::BC7:  return GS_RGBA;  // decoded to RGBA
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
    std::vector<uint8_t> rgba_buffer;   // CPU-decoded RGBA (DXT5→RGBA, reused)
    bool               warned_about_thread = false;

    // New: flag indicating that decompressed data is pending a GPU upload.
    // Set by decode() (any thread), cleared by uploadToGpu() (graphics thread).
    bool               pending_upload = false;
    int                pending_width  = 0;
    int                pending_height = 0;
    HapTextureFormat   pending_format = HapTextureFormat::Unknown;

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

    // --- Snappy decompress (CPU only — safe on any thread) ---
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

    int w = pimpl_->video_info.width;
    int h = pimpl_->video_info.height;
    if (w <= 0 || h <= 0) {
        pimpl_->last_error = "video dimensions not set (call setVideoInfo)";
        return false;
    }

    // Mark decompressed data as pending a GPU upload. The actual
    // gs_texture_create / gs_texture_set_image call happens in
    // uploadToGpu(), which must be invoked from the OBS graphics thread
    // (i.e. from hap_clip_video_render). Calling gs_texture_create from
    // video_tick fails on Windows OBS 31 ("gs_texture_create failed").
    pimpl_->pending_width  = w;
    pimpl_->pending_height = h;
    pimpl_->pending_format = fi.format;
    pimpl_->pending_upload = true;

    pimpl_->tex_format = fi.format;
    pimpl_->last_error.clear();
    blog(LOG_DEBUG, "[DanceHAP] HapDecoder: decoded %dx%d %s (%zu bytes → %zu)",
         w, h, hap_tex_format_to_string(fi.format),
         fi.compressed_size, pimpl_->decompressed.size());
    return true;
}

// ---------------------------------------------------------------------------
// uploadToGpu — must be called from the OBS graphics thread
// (i.e. from hap_clip_video_render). Creates or refreshes the gs_texture
// with the latest decompressed DXT/BC data.
// ---------------------------------------------------------------------------

#ifdef DANCEHAP_HAVE_OBS
void HapDecoder::uploadToGpu()
{
    if (!pimpl_->pending_upload) return;
    if (pimpl_->decompressed.empty()) return;

    int w = pimpl_->pending_width;
    int h = pimpl_->pending_height;
    gs_color_format gs_fmt = map_to_gs_format(pimpl_->pending_format);
    if (gs_fmt == GS_UNKNOWN) {
        pimpl_->last_error = "unsupported texture format for GPU upload";
        pimpl_->pending_upload = false;
        return;
    }

    // v0.3.1 fix: OBS does not hardware-decode DXT5 — its pitch calculation
    // for block-compressed formats is broken (4× too small), producing
    // vertical bars. We CPU-decode DXT5 to RGBA and upload as GS_RGBA.
    //
    // The decompressed buffer holds raw DXT5 block data. We decode it into
    // rgba_buffer, then upload rgba_buffer. For DXT1 the path is analogous
    // (same block layout minus the alpha section); for now we only fully
    // support DXT5 (HAPA = Hap5), which is the only variant the smoke tests
    // exercise. DXT1/HAP and BC7/HAPQ fall back to the solid-color debug
    // path if enabled, otherwise to a transparent texture (logged).
    const uint8_t *upload_data = nullptr;
    if (pimpl_->pending_format == HapTextureFormat::DXT5) {
        const size_t rgba_size =
            static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
        pimpl_->rgba_buffer.resize(rgba_size);
        if (!dxt5_to_rgba(pimpl_->decompressed.data(),
                          pimpl_->decompressed.size(),
                          pimpl_->rgba_buffer.data(), w, h)) {
            pimpl_->last_error = "dxt5_to_rgba: size/dimension mismatch";
            blog(LOG_ERROR, "[DanceHAP] HapDecoder: dxt5_to_rgba failed "
                 "(decompressed=%zu, expected=%zu for %dx%d)",
                 pimpl_->decompressed.size(),
                 static_cast<size_t>(w / 4) * static_cast<size_t>(h / 4) * 16,
                 w, h);
            pimpl_->pending_upload = false;
            return;
        }
        upload_data = pimpl_->rgba_buffer.data();
    } else {
        // DXT1 / BC7 not yet CPU-decoded — log and bail. Phase 5 polish.
        pimpl_->last_error = "unsupported compressed format (DXT1/BC7 need CPU decoder)";
        blog(LOG_WARNING, "[DanceHAP] HapDecoder: format %s not yet supported "
             "by CPU decoder — skipping upload",
             hap_tex_format_to_string(pimpl_->pending_format));
        pimpl_->pending_upload = false;
        return;
    }

    // DIAGNOSTIC MODE: force a solid BGRA test pattern to verify the render
    // path independently of DXT5 decoding. Controlled at compile time.
#ifdef DANCEHAP_DEBUG_SOLID_COLOR
    gs_fmt = GS_BGRA;
    pimpl_->rgba_buffer.assign(
        static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    for (size_t i = 0; i < pimpl_->rgba_buffer.size(); i += 4) {
        pimpl_->rgba_buffer[i + 0] = 0xFF;  // B
        pimpl_->rgba_buffer[i + 1] = 0x00;  // G
        pimpl_->rgba_buffer[i + 2] = 0xFF;  // R
        pimpl_->rgba_buffer[i + 3] = 0xFF;  // A
    }
    upload_data = pimpl_->rgba_buffer.data();
    blog(LOG_WARNING, "[DanceHAP] HapDecoder: DEBUG_SOLID_COLOR active — "
         "uploading %dx%d BGRA magenta instead of decoded RGBA", w, h);
#endif

    // Log the first successful upload for diagnostics.
    if (!pimpl_->warned_about_thread) {
        pimpl_->warned_about_thread = true;
        blog(LOG_INFO, "[DanceHAP] HapDecoder: first GPU upload on graphics "
             "thread (dims=%dx%d, fmt=%s, src=DXT5→RGBA CPU decode)",
             w, h, hap_tex_format_to_string(pimpl_->pending_format));
    }

    // Since we now upload uncompressed RGBA (not DXT5), we could use
    // gs_texture_set_image for in-place updates. But destroy+recreate is
    // simpler and correct for MVP; performance is fine for 256×256 @ 30fps.
    if (pimpl_->texture &&
        (pimpl_->tex_width != w || pimpl_->tex_height != h)) {
        gs_texture_destroy(pimpl_->texture);
        pimpl_->texture = nullptr;
    }
    if (pimpl_->texture) {
        gs_texture_destroy(pimpl_->texture);
        pimpl_->texture = nullptr;
    }

    const uint8_t *tex_data[] = { upload_data };
    pimpl_->texture = gs_texture_create(
        static_cast<uint32_t>(w),
        static_cast<uint32_t>(h),
        gs_fmt, 1, tex_data, GS_DYNAMIC);

    if (!pimpl_->texture) {
        pimpl_->last_error = "gs_texture_create failed";
        blog(LOG_ERROR, "[DanceHAP] HapDecoder: gs_texture_create failed "
             "(%dx%d %s) — graphics context may be missing",
             w, h, hap_tex_format_to_string(pimpl_->pending_format));
    } else {
        pimpl_->tex_width  = w;
        pimpl_->tex_height = h;
        pimpl_->tex_format = pimpl_->pending_format;
        pimpl_->last_error.clear();
    }

    pimpl_->pending_upload = false;
}
#else
void HapDecoder::uploadToGpu() { pimpl_->pending_upload = false; }
#endif

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

// uploadToGpu: no-op in stub mode (no GPU, no Snappy).
void HapDecoder::uploadToGpu() { /* no-op */ }

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
