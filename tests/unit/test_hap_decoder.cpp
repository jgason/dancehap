// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// Unit tests for HapDecoder (Phase 1.3 + spec-compliance fix 2026-06-22).
//
// Tests run in STUB mode (no Snappy, no GPU). The decoder parses the HAP
// frame header correctly per the Vidvox HAP spec and returns false from
// decode() without crashing.
//
// Frame format (spec-compliant since 2026-06-22 fix):
//   Section header (4 or 8 bytes):
//     [3B LE uint24 size][1B type]            — 4-byte header if size != 0
//     [3B zero][1B type][4B LE uint32 size]   — 8-byte header if size == 0
//   Followed by `size` bytes of payload.
//
//   Top-level Snappy types we recognise:
//     0xBB HAP   DXT1 Snappy    (MOV codec_tag "Hap1")
//     0xBE HAPA  DXT5 Snappy    (MOV codec_tag "Hap5")  ← our test asset
//     0xBF HAPQ  DXT5 Snappy    (MOV codec_tag "HapY")
//     0x0D multi-image container (MOV codec_tag "HapM")
//
// Tests:
//   Header parsing:
//   1.  Parse valid single-section HAP (DXT1) header → HAP variant, DXT1 format
//   2.  Parse valid single-section HAPA (DXT5) header → HAPA variant
//   3.  Parse valid single-section HAPQ header → HAPQ variant
//   4.  Parse unknown section type → invalid
//   5.  Parse truncated data (< 4 bytes) → invalid
//   6.  Parse empty data → invalid
//   7.  Compressed data pointer + size correct (4-byte header)
//   8.  Compressed data pointer + size correct (8-byte extended header)
//   9.  Multi-image (HapM 0x0D) container with nested colour section
//
//   Real payload (spec compliance — regression for Hephaistos black video):
//  10.  Parse the FIRST frame of tests/assets/sample_hapa_5s.mov (Hap5)
//       → variant HAPA, format DXT5, valid=true. This is the exact payload
//       that caused "invalid frame header" before the fix.
//
//   Decoder lifecycle:
//  11.  getTexture() nullptr before any decode()
//  12.  decode() with invalid packet (valid=false) returns false
//  13.  decode() stub mode returns false but doesn't crash
//  14.  getFormat() reflects parsed variant after decode attempt
//  15.  Multiple consecutive decode() calls don't leak or crash
//  16.  setVideoInfo sets dimensions, getWidth/getHeight reflect them
//
//   Integration:
//  17.  demuxer.readNextVideoPacket() → decoder.decode() chain works

#include <gtest/gtest.h>

#include "hap_decoder.hpp"
#include "hap_demuxer.hpp"
#include "hap_clip_source.hpp"
#include "obs_compat.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#ifndef DANCEHAP_TEST_ASSET
#  define DANCEHAP_TEST_ASSET "tests/assets/sample_hapa_5s.mov"
#endif

// ===========================================================================
// Helpers — build spec-compliant HAP frames
// ===========================================================================

namespace {

// HAP top-level section type bytes (Vidvox Hap Video spec).
constexpr uint8_t HAP_TYPE_HAP_DXT1_SNAPPY  = 0xBB;  // Hap1
constexpr uint8_t HAP_TYPE_HAPA_DXT5_SNAPPY = 0xBE;  // Hap5
constexpr uint8_t HAP_TYPE_HAPQ_DXT5_SNAPPY = 0xBF;  // HapY
constexpr uint8_t HAP_TYPE_MULTI_IMAGE      = 0x0D;  // HapM

/// Build a single-section HAP frame with the 4-byte header.
///   [3B LE uint24 size][1B type][payload...]
/// Used when size fits in 24 bits (< 16 MB).
std::vector<uint8_t> make_frame_4byte_header(uint8_t type,
                                             const std::vector<uint8_t> &payload)
{
    std::vector<uint8_t> frame;
    uint32_t sz = static_cast<uint32_t>(payload.size());
    // The short header needs size ≤ 24 bits. Tests use tiny payloads so this
    // is always satisfied; if it ever fails we want the parser, not the helper,
    // to surface the issue.
    if (sz > 0xFFFFFFu) sz = 0xFFFFFFu;
    frame.push_back(static_cast<uint8_t>(sz & 0xFF));
    frame.push_back(static_cast<uint8_t>((sz >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>((sz >> 16) & 0xFF));
    frame.push_back(type);
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

/// Build a single-section HAP frame with the 8-byte extended header.
///   [3B zero][1B type][4B LE uint32 size][payload...]
/// Used when size >= 16 MB (the 3-byte size field is zero to signal extension).
std::vector<uint8_t> make_frame_8byte_header(uint8_t type,
                                             const std::vector<uint8_t> &payload)
{
    std::vector<uint8_t> frame;
    uint32_t sz = static_cast<uint32_t>(payload.size());
    frame.push_back(0); frame.push_back(0); frame.push_back(0);  // size24 = 0
    frame.push_back(type);
    frame.push_back(static_cast<uint8_t>(sz & 0xFF));
    frame.push_back(static_cast<uint8_t>((sz >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>((sz >> 16) & 0xFF));
    frame.push_back(static_cast<uint8_t>((sz >> 24) & 0xFF));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

/// Build a multi-image HapM (0x0D) container with one nested colour section.
///   [3B LE uint24 outer_size][0x0D]
///     [3B LE uint24 inner_size][inner_type][inner_payload...]
std::vector<uint8_t> make_multisection_frame(uint8_t inner_type,
                                             const std::vector<uint8_t> &inner_payload)
{
    // Inner section first.
    std::vector<uint8_t> inner;
    uint32_t inner_sz = static_cast<uint32_t>(inner_payload.size());
    inner.push_back(static_cast<uint8_t>(inner_sz & 0xFF));
    inner.push_back(static_cast<uint8_t>((inner_sz >> 8) & 0xFF));
    inner.push_back(static_cast<uint8_t>((inner_sz >> 16) & 0xFF));
    inner.push_back(inner_type);
    inner.insert(inner.end(), inner_payload.begin(), inner_payload.end());

    // Outer container.
    std::vector<uint8_t> outer;
    uint32_t outer_sz = static_cast<uint32_t>(inner.size());
    outer.push_back(static_cast<uint8_t>(outer_sz & 0xFF));
    outer.push_back(static_cast<uint8_t>((outer_sz >> 8) & 0xFF));
    outer.push_back(static_cast<uint8_t>((outer_sz >> 16) & 0xFF));
    outer.push_back(HAP_TYPE_MULTI_IMAGE);
    outer.insert(outer.end(), inner.begin(), inner.end());
    return outer;
}

// Minimal fake compressed data (64 bytes of pattern).
const std::vector<uint8_t> FAKE_COMPRESSED(64, 0xAB);

} // anonymous namespace

// ===========================================================================
// Header parsing tests — single-section
// ===========================================================================

TEST(HapDecoderParseTest, SingleSectionHAPReturnsCorrectVariant)
{
    auto frame = make_frame_4byte_header(HAP_TYPE_HAP_DXT1_SNAPPY, FAKE_COMPRESSED);
    auto info = dancehap::parse_hap_frame(frame.data(), frame.size());

    EXPECT_TRUE(info.valid);
    EXPECT_EQ(info.variant, dancehap::HapVariant::HAP);
    EXPECT_EQ(info.format, dancehap::HapTextureFormat::DXT1);
}

TEST(HapDecoderParseTest, SingleSectionHAPAReturnsCorrectVariant)
{
    auto frame = make_frame_4byte_header(HAP_TYPE_HAPA_DXT5_SNAPPY, FAKE_COMPRESSED);
    auto info = dancehap::parse_hap_frame(frame.data(), frame.size());

    EXPECT_TRUE(info.valid);
    EXPECT_EQ(info.variant, dancehap::HapVariant::HAPA);
    EXPECT_EQ(info.format, dancehap::HapTextureFormat::DXT5);
}

TEST(HapDecoderParseTest, SingleSectionHAPQReturnsCorrectVariant)
{
    auto frame = make_frame_4byte_header(HAP_TYPE_HAPQ_DXT5_SNAPPY, FAKE_COMPRESSED);
    auto info = dancehap::parse_hap_frame(frame.data(), frame.size());

    EXPECT_TRUE(info.valid);
    EXPECT_EQ(info.variant, dancehap::HapVariant::HAPQ);
}

TEST(HapDecoderParseTest, UnknownSectionTypeReturnsInvalid)
{
    // 0x00 is not a recognised top-level Snappy type.
    auto frame = make_frame_4byte_header(0x00, FAKE_COMPRESSED);
    auto info = dancehap::parse_hap_frame(frame.data(), frame.size());

    EXPECT_FALSE(info.valid);
    EXPECT_EQ(info.variant, dancehap::HapVariant::Unknown);
}

TEST(HapDecoderParseTest, TruncatedDataReturnsInvalid)
{
    // Only 3 bytes — too small for a 4-byte header.
    std::vector<uint8_t> tiny = {0x01, 0x02, 0x03};
    auto info = dancehap::parse_hap_frame(tiny.data(), tiny.size());
    EXPECT_FALSE(info.valid);
}

TEST(HapDecoderParseTest, EmptyDataReturnsInvalid)
{
    auto info = dancehap::parse_hap_frame(nullptr, 0);
    EXPECT_FALSE(info.valid);

    std::vector<uint8_t> empty;
    info = dancehap::parse_hap_frame(empty.data(), empty.size());
    EXPECT_FALSE(info.valid);
}

TEST(HapDecoderParseTest, CompressedDataPointerAndSize4ByteHeader)
{
    auto frame = make_frame_4byte_header(HAP_TYPE_HAPA_DXT5_SNAPPY, FAKE_COMPRESSED);
    auto info = dancehap::parse_hap_frame(frame.data(), frame.size());

    ASSERT_TRUE(info.valid);
    EXPECT_NE(info.compressed_data, nullptr);
    EXPECT_EQ(info.compressed_size, FAKE_COMPRESSED.size());

    // The compressed data starts at offset 4 (after the 4-byte header).
    EXPECT_EQ(info.compressed_data, frame.data() + 4);
}

TEST(HapDecoderParseTest, CompressedDataPointerAndSize8ByteHeader)
{
    // Force the extended header by using payload ≥ 16 MB? That's too big.
    // Instead, build manually with size24=0 to trigger the 8-byte path.
    std::vector<uint8_t> frame;
    frame.push_back(0); frame.push_back(0); frame.push_back(0);  // size24 = 0
    frame.push_back(HAP_TYPE_HAPA_DXT5_SNAPPY);
    uint32_t sz = static_cast<uint32_t>(FAKE_COMPRESSED.size());
    frame.push_back(static_cast<uint8_t>(sz & 0xFF));
    frame.push_back(static_cast<uint8_t>((sz >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>((sz >> 16) & 0xFF));
    frame.push_back(static_cast<uint8_t>((sz >> 24) & 0xFF));
    frame.insert(frame.end(), FAKE_COMPRESSED.begin(), FAKE_COMPRESSED.end());

    auto info = dancehap::parse_hap_frame(frame.data(), frame.size());

    ASSERT_TRUE(info.valid);
    EXPECT_EQ(info.variant, dancehap::HapVariant::HAPA);
    EXPECT_NE(info.compressed_data, nullptr);
    EXPECT_EQ(info.compressed_size, FAKE_COMPRESSED.size());
    // 8-byte header → payload at offset 8.
    EXPECT_EQ(info.compressed_data, frame.data() + 8);
}

// ===========================================================================
// Header parsing tests — multi-image container (HapM)
// ===========================================================================

TEST(HapDecoderParseTest, MultiImageHapMReturnsColourTextureInfo)
{
    auto frame = make_multisection_frame(HAP_TYPE_HAPQ_DXT5_SNAPPY, FAKE_COMPRESSED);
    auto info = dancehap::parse_hap_frame(frame.data(), frame.size());

    EXPECT_TRUE(info.valid);
    // HapM container reports HAPQ_A as the overall variant.
    EXPECT_EQ(info.variant, dancehap::HapVariant::HAPQ_A);
    EXPECT_EQ(info.format, dancehap::HapTextureFormat::BC7);
    EXPECT_EQ(info.compressed_size, FAKE_COMPRESSED.size());
}

TEST(HapDecoderParseTest, MultiImageHapMWithUnknownInnerReturnsInvalid)
{
    // Inner section with an unknown type.
    auto frame = make_multisection_frame(0x00, FAKE_COMPRESSED);
    auto info = dancehap::parse_hap_frame(frame.data(), frame.size());

    EXPECT_FALSE(info.valid);
}

// ===========================================================================
// Spec compliance — real HAP frame header bytes from sample_hapa_5s.mov
//
// This is the regression test for the Hephaistos black-video bug (2026-06-22).
// The first video packet of sample_hapa_5s.mov starts with the 8-byte
// extended header: [00 00 00][BE][EB 0F 00 00]. The pre-fix parser expected
// a 4-byte BE length followed by an ASCII FourCC, which never matched and
// produced "invalid frame header" on every frame.
//
// We hardcode the first 32 bytes of the real packet rather than shelling
// out to ffmpeg at test runtime — keeps the test hermetic and CI-friendly.
// ===========================================================================

TEST(HapDecoderParseTest, RealHapaPayloadFromSampleAsset)
{
    // First 32 bytes of frame 0 of tests/assets/sample_hapa_5s.mov.
    // [3B size=0][1B type=0xBE][4B LE size=0x00000FEB = 4075][snappy data...]
    static const uint8_t REAL_FRAME_HEAD[] = {
        0x00, 0x00, 0x00, 0xBE,  // extended header marker + section type Hap5
        0xEB, 0x0F, 0x00, 0x00,  // section size = 4075 (LE uint32)
        // Start of Snappy-compressed DXT5 payload (decompresses to 65536 bytes).
        0x80, 0x80, 0x04, 0x08, 0xFF, 0xFF, 0x00, 0x05,
        0x01, 0x1C, 0x34, 0xE3, 0x52, 0xC1, 0xAA, 0xAA,
        0xAA, 0xAA, 0xFE, 0x10, 0x00, 0xBE, 0x10, 0x00,
    };
    constexpr size_t REAL_FRAME_HEAD_LEN = sizeof(REAL_FRAME_HEAD);

    auto info = dancehap::parse_hap_frame(REAL_FRAME_HEAD, REAL_FRAME_HEAD_LEN);

    EXPECT_TRUE(info.valid)
        << "First byte = 0x" << std::hex << (int)REAL_FRAME_HEAD[0];
    EXPECT_EQ(info.variant, dancehap::HapVariant::HAPA);
    EXPECT_EQ(info.format, dancehap::HapTextureFormat::DXT5);

    // The parser should point to the payload at offset 8 (after the 8-byte
    // extended header) and report the full declared section size.
    EXPECT_EQ(info.compressed_data, REAL_FRAME_HEAD + 8);
    // We only have 24 bytes of payload in this truncated fixture, but the
    // parser clamps to the buffer size — so compressed_size ≤ 24.
    EXPECT_LE(info.compressed_size, REAL_FRAME_HEAD_LEN - 8);
    EXPECT_GT(info.compressed_size, 0u);
}

// Full-payload variant: shell out to ffmpeg to extract the entire first frame.
// Skipped if ffmpeg is not available (e.g. CI stub mode without ffmpeg).
TEST(HapDecoderParseTest, RealHapaPayloadFullFrameFromFfmpeg)
{
    std::vector<uint8_t> payload;
    {
        FILE *pipe = popen(
            "ffmpeg -i " DANCEHAP_TEST_ASSET
            " -map 0:v -c copy -vframes 1 -f data - 2>/dev/null",
            "rb");
        if (pipe == nullptr) {
            GTEST_SKIP() << "ffmpeg not available in PATH";
        }
        constexpr size_t CHUNK = 4096;
        uint8_t buf[CHUNK];
        size_t n;
        while ((n = fread(buf, 1, CHUNK, pipe)) > 0) {
            payload.insert(payload.end(), buf, buf + n);
        }
        int rc = pclose(pipe);
        if (rc != 0 || payload.empty()) {
            GTEST_SKIP() << "ffmpeg failed (rc=" << rc << ")";
        }
    }
    ASSERT_GT(payload.size(), 8u);

    auto info = dancehap::parse_hap_frame(payload.data(), payload.size());

    EXPECT_TRUE(info.valid);
    EXPECT_EQ(info.variant, dancehap::HapVariant::HAPA);
    EXPECT_EQ(info.format, dancehap::HapTextureFormat::DXT5);

    // Full frame: declared size (4075) should match available payload after
    // the 8-byte header.
    EXPECT_EQ(info.compressed_data, payload.data() + 8);
    EXPECT_EQ(info.compressed_size, payload.size() - 8);

    // The declared section size in the header is 0x0FEB = 4075, and the
    // full packet is 4083 = 8 (header) + 4075 (payload).
    EXPECT_EQ(payload.size(), 4083u);
}

// ===========================================================================
// Decoder lifecycle tests (stub mode)
// ===========================================================================

class HapDecoderTest : public ::testing::Test {
protected:
    dancehap::HapDecoder decoder;
};

TEST_F(HapDecoderTest, GetTextureNullBeforeDecode)
{
    EXPECT_EQ(decoder.getTexture(), nullptr);
}

TEST_F(HapDecoderTest, GetFormatUnknownBeforeDecode)
{
    EXPECT_EQ(decoder.getFormat(), dancehap::HapTextureFormat::Unknown);
}

TEST_F(HapDecoderTest, DecodeInvalidPacketReturnsFalse)
{
    dancehap::DemuxPacket pkt;  // valid=false by default
    EXPECT_FALSE(decoder.decode(pkt));
    EXPECT_FALSE(decoder.getLastError().empty());
}

TEST_F(HapDecoderTest, DecodeStubModeReturnsFalseNoCrash)
{
    auto frame = make_frame_4byte_header(HAP_TYPE_HAPA_DXT5_SNAPPY, FAKE_COMPRESSED);
    dancehap::DemuxPacket pkt;
    pkt.data = frame;
    pkt.valid = true;

    bool result = decoder.decode(pkt);

    // Stub mode: Snappy not available → returns false.
    EXPECT_FALSE(result);

    // But the decoder should have parsed the variant.
    EXPECT_EQ(decoder.getFormat(), dancehap::HapTextureFormat::DXT5);

    // No texture in stub mode.
    EXPECT_EQ(decoder.getTexture(), nullptr);

    // Error message should be set.
    EXPECT_FALSE(decoder.getLastError().empty());
}

TEST_F(HapDecoderTest, SetVideoInfoSetsDimensions)
{
    dancehap::VideoInfo vi;
    vi.width  = 512;
    vi.height = 256;
    vi.variant = dancehap::HapVariant::HAPA;
    decoder.setVideoInfo(vi);

    auto frame = make_frame_4byte_header(HAP_TYPE_HAPA_DXT5_SNAPPY, FAKE_COMPRESSED);
    dancehap::DemuxPacket pkt;
    pkt.data = frame;
    pkt.valid = true;
    decoder.decode(pkt);  // returns false in stub

    EXPECT_EQ(decoder.getWidth(), 512);
    EXPECT_EQ(decoder.getHeight(), 256);
}

TEST_F(HapDecoderTest, MultipleDecodesDoNotLeakOrCrash)
{
    decoder.setVideoInfo({dancehap::HapVariant::HAPA, 256, 256, 30, 1, 0});

    auto frame = make_frame_4byte_header(HAP_TYPE_HAPA_DXT5_SNAPPY, FAKE_COMPRESSED);
    dancehap::DemuxPacket pkt;
    pkt.data = frame;
    pkt.valid = true;

    for (int i = 0; i < 100; ++i) {
        decoder.decode(pkt);
    }

    SUCCEED();
}

TEST_F(HapDecoderTest, DimensionsChangeHandledGracefully)
{
    dancehap::VideoInfo vi1;
    vi1.width = 256;
    vi1.height = 256;
    vi1.variant = dancehap::HapVariant::HAPA;
    decoder.setVideoInfo(vi1);

    auto frame = make_frame_4byte_header(HAP_TYPE_HAPA_DXT5_SNAPPY, FAKE_COMPRESSED);
    dancehap::DemuxPacket pkt;
    pkt.data = frame;
    pkt.valid = true;
    decoder.decode(pkt);
    EXPECT_EQ(decoder.getWidth(), 256);
    EXPECT_EQ(decoder.getHeight(), 256);

    dancehap::VideoInfo vi2;
    vi2.width = 512;
    vi2.height = 512;
    vi2.variant = dancehap::HapVariant::HAPA;
    decoder.setVideoInfo(vi2);
    decoder.decode(pkt);
    EXPECT_EQ(decoder.getWidth(), 512);
    EXPECT_EQ(decoder.getHeight(), 512);

    SUCCEED();
}

// ===========================================================================
// Integration: demuxer → decoder chain
// ===========================================================================

class HapDecoderIntegrationTest : public ::testing::Test {
protected:
    dancehap::HapDemuxer demuxer;
    dancehap::HapDecoder decoder;
};

TEST_F(HapDecoderIntegrationTest, DemuxerToDecoderChainWorks)
{
    ASSERT_TRUE(demuxer.open(DANCEHAP_TEST_ASSET));
    decoder.setVideoInfo(demuxer.getVideoInfo());

    auto pkt = demuxer.readNextVideoPacket();
    ASSERT_TRUE(pkt.valid);

    bool result = decoder.decode(pkt);
    EXPECT_FALSE(result);  // stub mode: no Snappy

    EXPECT_FALSE(decoder.getLastError().empty());
    demuxer.close();
}

TEST_F(HapDecoderIntegrationTest, MultipleFramesFromDemuxerDecodeChain)
{
    ASSERT_TRUE(demuxer.open(DANCEHAP_TEST_ASSET));
    decoder.setVideoInfo(demuxer.getVideoInfo());

    for (int i = 0; i < 10; ++i) {
        auto pkt = demuxer.readNextVideoPacket();
        if (!pkt.valid) break;
        decoder.decode(pkt);
    }

    SUCCEED();
    demuxer.close();
}
