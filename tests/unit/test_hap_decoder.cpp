// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// Unit tests for HapDecoder (Phase 1.3).
//
// Tests run in STUB mode (no Snappy, no GPU). The decoder parses the HAP
// frame header correctly and returns false from decode() without crashing.
//
// Tests:
//   Header parsing:
//   1.  Parse valid single-chunk HAP (DXT1) header → HAP variant, DXT1 format
//   2.  Parse valid single-chunk HAPA (DXT5) header → HAPA variant
//   3.  Parse valid single-chunk HAPQ header → HAPQ variant, BC7 format
//   4.  Parse valid single-chunk HAPQ-A header
//   5.  Parse invalid header (wrong FourCC) → invalid
//   6.  Parse truncated data (< 8 bytes) → invalid
//   7.  Parse empty data → invalid
//   8.  Parse multi-section (HapM) header with texture section
//   9.  Compressed data pointer + size correct for single-chunk
//  10.  Compressed data pointer + size correct for multi-section
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
//  18.  HapM frame with no HapS section → invalid

#include <gtest/gtest.h>

#include "hap_decoder.hpp"
#include "hap_demuxer.hpp"
#include "hap_clip_source.hpp"
#include "obs_compat.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifndef DANCEHAP_TEST_ASSET
#  define DANCEHAP_TEST_ASSET "tests/assets/sample_hapa_5s.mov"
#endif

// ===========================================================================
// Helpers — build synthetic HAP frames
// ===========================================================================

namespace {

// Write a big-endian uint32 into a byte vector.
void put_be32(std::vector<uint8_t> &v, uint32_t val)
{
    v.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    v.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>(val & 0xFF));
}

// Write a FourCC (4 ASCII chars, stored as raw bytes = le32 read).
void put_fourcc(std::vector<uint8_t> &v, const char *fcc)
{
    for (int i = 0; i < 4; ++i)
        v.push_back(static_cast<uint8_t>(fcc[i]));
}

/// Build a single-chunk HAP frame.
///   [4B BE32: remaining] [4B FourCC] [compressed_data...]
/// remaining = 4 (FourCC) + compressed_data.size()
std::vector<uint8_t> make_single_chunk_frame(const char *fcc,
                                             const std::vector<uint8_t> &compressed)
{
    std::vector<uint8_t> frame;
    uint32_t remaining = 4 + static_cast<uint32_t>(compressed.size());
    put_be32(frame, remaining);
    put_fourcc(frame, fcc);
    frame.insert(frame.end(), compressed.begin(), compressed.end());
    return frame;
}

/// Build a multi-section (HapM) frame with one texture section (HapS).
///   [4B BE32: container_remaining] [4B "HapM"]
///     [4B BE32: sec_remaining] [4B "HapS"] [4B tex_fcc] [compressed...]
std::vector<uint8_t> make_multisection_frame(const char *tex_fcc,
                                             const std::vector<uint8_t> &compressed)
{
    // Build the HapS section first.
    std::vector<uint8_t> haps_section;
    // Texture FourCC (4 bytes).
    put_fourcc(haps_section, tex_fcc);
    // Compressed data.
    haps_section.insert(haps_section.end(), compressed.begin(), compressed.end());

    // Now build the full HapM container.
    std::vector<uint8_t> sections;
    // Section: [4B BE32: sec_remaining=4+haps_section.size()] [4B "HapS"] [data]
    uint32_t sec_remaining = 4 + static_cast<uint32_t>(haps_section.size());
    put_be32(sections, sec_remaining);
    put_fourcc(sections, "HapS");
    sections.insert(sections.end(), haps_section.begin(), haps_section.end());

    // Top-level: [4B BE32: container_remaining=4+sections.size()] [4B "HapM"]
    std::vector<uint8_t> frame;
    uint32_t container_remaining = 4 + static_cast<uint32_t>(sections.size());
    put_be32(frame, container_remaining);
    put_fourcc(frame, "HapM");
    frame.insert(frame.end(), sections.begin(), sections.end());
    return frame;
}

// Minimal fake compressed data (64 bytes of pattern).
const std::vector<uint8_t> FAKE_COMPRESSED(64, 0xAB);

} // anonymous namespace

// ===========================================================================
// Header parsing tests
// ===========================================================================

TEST(HapDecoderParseTest, SingleChunkHAPReturnsCorrectVariant)
{
    auto frame = make_single_chunk_frame("Hap1", FAKE_COMPRESSED);
    auto info = dancehap::parse_hap_frame(frame.data(), frame.size());

    EXPECT_TRUE(info.valid);
    EXPECT_EQ(info.variant, dancehap::HapVariant::HAP);
    EXPECT_EQ(info.format, dancehap::HapTextureFormat::DXT1);
}

TEST(HapDecoderParseTest, SingleChunkHAPAReturnsCorrectVariant)
{
    auto frame = make_single_chunk_frame("Hap5", FAKE_COMPRESSED);
    auto info = dancehap::parse_hap_frame(frame.data(), frame.size());

    EXPECT_TRUE(info.valid);
    EXPECT_EQ(info.variant, dancehap::HapVariant::HAPA);
    EXPECT_EQ(info.format, dancehap::HapTextureFormat::DXT5);
}

TEST(HapDecoderParseTest, SingleChunkHAPQReturnsCorrectVariant)
{
    auto frame = make_single_chunk_frame("HapY", FAKE_COMPRESSED);
    auto info = dancehap::parse_hap_frame(frame.data(), frame.size());

    EXPECT_TRUE(info.valid);
    EXPECT_EQ(info.variant, dancehap::HapVariant::HAPQ);
    EXPECT_EQ(info.format, dancehap::HapTextureFormat::BC7);
}

TEST(HapDecoderParseTest, SingleChunkHAPQAReturnsCorrectVariant)
{
    auto frame = make_single_chunk_frame("HapA", FAKE_COMPRESSED);
    auto info = dancehap::parse_hap_frame(frame.data(), frame.size());

    EXPECT_TRUE(info.valid);
    EXPECT_EQ(info.variant, dancehap::HapVariant::HAPQ_A);
    EXPECT_EQ(info.format, dancehap::HapTextureFormat::BC7);
}

TEST(HapDecoderParseTest, InvalidFourCCReturnsInvalid)
{
    auto frame = make_single_chunk_frame("XXXX", FAKE_COMPRESSED);
    auto info = dancehap::parse_hap_frame(frame.data(), frame.size());

    EXPECT_FALSE(info.valid);
    EXPECT_EQ(info.variant, dancehap::HapVariant::Unknown);
}

TEST(HapDecoderParseTest, TruncatedDataReturnsInvalid)
{
    // Only 4 bytes — too small for a header.
    std::vector<uint8_t> tiny = {0x00, 0x00, 0x00, 0x04};
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

TEST(HapDecoderParseTest, MultiSectionHapMReturnsTextureInfo)
{
    auto frame = make_multisection_frame("Hap5", FAKE_COMPRESSED);
    auto info = dancehap::parse_hap_frame(frame.data(), frame.size());

    EXPECT_TRUE(info.valid);
    EXPECT_EQ(info.variant, dancehap::HapVariant::HAPA);
    EXPECT_EQ(info.format, dancehap::HapTextureFormat::DXT5);
}

TEST(HapDecoderParseTest, MultiSectionCompressedDataPointerAndSize)
{
    auto frame = make_multisection_frame("Hap1", FAKE_COMPRESSED);
    auto info = dancehap::parse_hap_frame(frame.data(), frame.size());

    ASSERT_TRUE(info.valid);
    EXPECT_NE(info.compressed_data, nullptr);
    EXPECT_EQ(info.compressed_size, FAKE_COMPRESSED.size());
}

TEST(HapDecoderParseTest, SingleChunkCompressedDataPointerAndSize)
{
    auto frame = make_single_chunk_frame("Hap1", FAKE_COMPRESSED);
    auto info = dancehap::parse_hap_frame(frame.data(), frame.size());

    ASSERT_TRUE(info.valid);
    EXPECT_NE(info.compressed_data, nullptr);
    EXPECT_EQ(info.compressed_size, FAKE_COMPRESSED.size());

    // The compressed data pointer should be at offset 8 in the frame.
    EXPECT_EQ(info.compressed_data, frame.data() + 8);
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
    // Build a valid HAPA frame and try to decode.
    auto frame = make_single_chunk_frame("Hap5", FAKE_COMPRESSED);
    dancehap::DemuxPacket pkt;
    pkt.data = frame;
    pkt.valid = true;

    bool result = decoder.decode(pkt);

    // Stub mode: Snappy not available → returns false.
    EXPECT_FALSE(result);

    // But the decoder should have parsed the variant.
    // getFormat() reflects the parsed texture format.
    EXPECT_EQ(decoder.getFormat(), dancehap::HapTextureFormat::DXT5);

    // No texture in stub mode.
    EXPECT_EQ(decoder.getTexture(), nullptr);

    // Error message should mention stub mode.
    EXPECT_FALSE(decoder.getLastError().empty());
}

TEST_F(HapDecoderTest, SetVideoInfoSetsDimensions)
{
    dancehap::VideoInfo vi;
    vi.width  = 512;
    vi.height = 256;
    vi.variant = dancehap::HapVariant::HAPA;
    decoder.setVideoInfo(vi);

    // After a decode attempt (even failed), dimensions come from video info.
    auto frame = make_single_chunk_frame("Hap5", FAKE_COMPRESSED);
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

    auto frame = make_single_chunk_frame("Hap5", FAKE_COMPRESSED);
    dancehap::DemuxPacket pkt;
    pkt.data = frame;
    pkt.valid = true;

    // Decode 100 times — should not crash or leak.
    for (int i = 0; i < 100; ++i) {
        decoder.decode(pkt);
    }

    // Still no crash.
    SUCCEED();
}

TEST_F(HapDecoderTest, DimensionsChangeHandledGracefully)
{
    // First with 256x256.
    dancehap::VideoInfo vi1;
    vi1.width = 256;
    vi1.height = 256;
    vi1.variant = dancehap::HapVariant::HAPA;
    decoder.setVideoInfo(vi1);

    auto frame = make_single_chunk_frame("Hap5", FAKE_COMPRESSED);
    dancehap::DemuxPacket pkt;
    pkt.data = frame;
    pkt.valid = true;
    decoder.decode(pkt);
    EXPECT_EQ(decoder.getWidth(), 256);
    EXPECT_EQ(decoder.getHeight(), 256);

    // Switch to 512x512.
    dancehap::VideoInfo vi2;
    vi2.width = 512;
    vi2.height = 512;
    vi2.variant = dancehap::HapVariant::HAPA;
    decoder.setVideoInfo(vi2);
    decoder.decode(pkt);
    EXPECT_EQ(decoder.getWidth(), 512);
    EXPECT_EQ(decoder.getHeight(), 512);

    // No crash.
    SUCCEED();
}

TEST_F(HapDecoderTest, HapMFrameWithoutHapSSectionIsInvalid)
{
    // Build an HapM frame with a non-texture section only.
    std::vector<uint8_t> frame;
    // Section data (just the "HapC" compressor section, no HapS).
    std::vector<uint8_t> sections;
    uint32_t sec_remaining = 4 + 4;  // FourCC + 4 bytes data
    put_be32(sections, sec_remaining);
    put_fourcc(sections, "HapC");
    sections.insert(sections.end(), 4, 0);  // fake compressor data

    uint32_t container_remaining = 4 + static_cast<uint32_t>(sections.size());
    put_be32(frame, container_remaining);
    put_fourcc(frame, "HapM");
    frame.insert(frame.end(), sections.begin(), sections.end());

    auto info = dancehap::parse_hap_frame(frame.data(), frame.size());
    EXPECT_FALSE(info.valid);
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

    // Set video info from demuxer.
    decoder.setVideoInfo(demuxer.getVideoInfo());

    // Read a packet and try to decode.
    auto pkt = demuxer.readNextVideoPacket();
    ASSERT_TRUE(pkt.valid);

    // In stub mode, decode returns false (no Snappy) but no crash.
    bool result = decoder.decode(pkt);
    EXPECT_FALSE(result);  // expected in stub mode

    // The decoder should not have crashed and error should be set.
    EXPECT_FALSE(decoder.getLastError().empty());

    demuxer.close();
}

TEST_F(HapDecoderIntegrationTest, MultipleFramesFromDemuxerDecodeChain)
{
    ASSERT_TRUE(demuxer.open(DANCEHAP_TEST_ASSET));
    decoder.setVideoInfo(demuxer.getVideoInfo());

    // Decode 10 frames in sequence.
    for (int i = 0; i < 10; ++i) {
        auto pkt = demuxer.readNextVideoPacket();
        if (!pkt.valid) break;
        decoder.decode(pkt);
    }

    // No crash, no hang.
    SUCCEED();

    demuxer.close();
}
