// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// Unit tests for HapDemuxer (Phase 1.2).
//
// Tests run in STUB mode (no FFmpeg installed). The stub validates file
// existence + ISO-BMFF container signature, then returns known metadata
// matching tests/assets/sample_hapa_5s.mov (HAPA 256x256 30fps 5s, AAC audio).
//
// Tests:
//   1. Open valid file → true
//   2. Open non-existent file → false (graceful, no crash)
//   3. hasVideo() true for the test asset
//   4. hasAudio() true for the test asset
//   5. getVideoInfo() returns HAPA 256x256 30fps ~5s
//   6. Variant detection: HAPA detected correctly
//   7. Close releases resources (state → Idle)
//   8. Open after close (reuse) works
//   9. Random-bytes file → false (unsupported format)
//  10. Integration: hap_clip_create with valid path opens demuxer (get_width=256)
//  11. Empty path → false
//  12. State machine: Idle before open, Ready after, Error on bad file
//  13. Packet reading: readNextVideoPacket returns valid packets
//  14. Packet reading: readNextVideoPacket eventually hits EOF

#include <gtest/gtest.h>

#include "hap_demuxer.hpp"
#include "hap_clip_source.hpp"
#include "obs_compat.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#ifndef DANCEHAP_TEST_ASSET
#  define DANCEHAP_TEST_ASSET "tests/assets/sample_hapa_5s.mov"
#endif

// ---------------------------------------------------------------------------
// Helper: create a temporary file with given contents.
// Returns the file path. Caller must remove it after use.
// Cross-platform: uses tmpnam() + ofstream (mkstemp is POSIX-only and not
// available on MSVC — caused Phase 1.2 CI failure on Windows).
// ---------------------------------------------------------------------------

static std::string make_temp_file(const std::string &suffix,
                                  const std::vector<uint8_t> &contents)
{
    char tmpl_buf[L_tmpnam];
    if (std::tmpnam(tmpl_buf) == nullptr) return "";

    std::string path(tmpl_buf);
    // Add a file extension if a suffix was requested (some platforms need it).
    if (!suffix.empty()) path += suffix;

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return "";
    f.write(reinterpret_cast<const char *>(contents.data()),
            static_cast<std::streamsize>(contents.size()));
    f.close();

    return path;
}

static void remove_temp_file(const std::string &path)
{
    if (!path.empty()) {
        std::remove(path.c_str());
    }
}

// ===========================================================================
// Open / close
// ===========================================================================

class HapDemuxerTest : public ::testing::Test {
protected:
    dancehap::HapDemuxer demuxer;
};

TEST_F(HapDemuxerTest, OpenValidFileReturnsTrue)
{
    EXPECT_TRUE(demuxer.open(DANCEHAP_TEST_ASSET));
    demuxer.close();
}

TEST_F(HapDemuxerTest, OpenNonExistentFileReturnsFalse)
{
    EXPECT_FALSE(demuxer.open("/tmp/does_not_exist_dancehap.mov"));
    // Must not crash, error message should be set.
    EXPECT_FALSE(demuxer.getLastError().empty());
}

TEST_F(HapDemuxerTest, OpenEmptyPathReturnsFalse)
{
    EXPECT_FALSE(demuxer.open(""));
    EXPECT_EQ(demuxer.getState(), dancehap::DemuxState::Error);
}

// ===========================================================================
// Stream detection
// ===========================================================================

TEST_F(HapDemuxerTest, HasVideoTrueForTestAsset)
{
    ASSERT_TRUE(demuxer.open(DANCEHAP_TEST_ASSET));
    EXPECT_TRUE(demuxer.hasVideo());
    demuxer.close();
}

TEST_F(HapDemuxerTest, HasAudioTrueForTestAsset)
{
    ASSERT_TRUE(demuxer.open(DANCEHAP_TEST_ASSET));
    EXPECT_TRUE(demuxer.hasAudio());
    demuxer.close();
}

// ===========================================================================
// Video info
// ===========================================================================

TEST_F(HapDemuxerTest, VideoInfoReturnsCorrectMetadata)
{
    ASSERT_TRUE(demuxer.open(DANCEHAP_TEST_ASSET));

    const auto &vi = demuxer.getVideoInfo();
    EXPECT_EQ(vi.variant, dancehap::HapVariant::HAPA);
    EXPECT_EQ(vi.width, 256);
    EXPECT_EQ(vi.height, 256);
    EXPECT_EQ(vi.fps_num, 30);
    EXPECT_EQ(vi.fps_den, 1);

    // Duration ~5 seconds (allow small tolerance for rounding).
    double duration = vi.duration_us / 1000000.0;
    EXPECT_NEAR(duration, 5.0, 0.1);

    demuxer.close();
}

TEST_F(HapDemuxerTest, VariantDetectionIsHAPA)
{
    ASSERT_TRUE(demuxer.open(DANCEHAP_TEST_ASSET));

    const auto &vi = demuxer.getVideoInfo();
    EXPECT_EQ(vi.variant, dancehap::HapVariant::HAPA);

    // HAPA should have alpha.
    EXPECT_TRUE(dancehap::hap_variant_has_alpha(vi.variant));

    // String representation.
    EXPECT_STREQ(dancehap::hap_variant_to_string(vi.variant), "HAPA");

    demuxer.close();
}

// ===========================================================================
// Close / reuse
// ===========================================================================

TEST_F(HapDemuxerTest, CloseReleasesResourcesStateBecomesIdle)
{
    ASSERT_TRUE(demuxer.open(DANCEHAP_TEST_ASSET));
    EXPECT_EQ(demuxer.getState(), dancehap::DemuxState::Ready);

    demuxer.close();
    EXPECT_EQ(demuxer.getState(), dancehap::DemuxState::Idle);
    EXPECT_FALSE(demuxer.hasVideo());
    EXPECT_FALSE(demuxer.hasAudio());
}

TEST_F(HapDemuxerTest, OpenAfterCloseWorksReuse)
{
    // First open.
    ASSERT_TRUE(demuxer.open(DANCEHAP_TEST_ASSET));
    EXPECT_TRUE(demuxer.hasVideo());
    demuxer.close();

    // Reopen after close — should work.
    EXPECT_TRUE(demuxer.open(DANCEHAP_TEST_ASSET));
    EXPECT_TRUE(demuxer.hasVideo());
    EXPECT_EQ(demuxer.getVideoInfo().width, 256);
    demuxer.close();

    // Reopen via reopen() helper.
    EXPECT_TRUE(demuxer.reopen(DANCEHAP_TEST_ASSET));
    EXPECT_TRUE(demuxer.hasVideo());
    demuxer.close();
}

// ===========================================================================
// Rejection of non-container files
// ===========================================================================

TEST_F(HapDemuxerTest, RandomBytesFileReturnsFalse)
{
    // Create a file with 256 random bytes (no valid container signature).
    std::vector<uint8_t> random_bytes(256);
    for (size_t i = 0; i < random_bytes.size(); ++i) {
        random_bytes[i] = static_cast<uint8_t>(i * 37 + 13);
    }

    std::string path = make_temp_file(".mov", random_bytes);
    ASSERT_FALSE(path.empty());

    EXPECT_FALSE(demuxer.open(path));
    EXPECT_EQ(demuxer.getState(), dancehap::DemuxState::Error);

    remove_temp_file(path);
}

TEST_F(HapDemuxerTest, TinyFileReturnsFalse)
{
    // File with only 4 bytes — too small to be a valid container.
    std::vector<uint8_t> tiny = {0x00, 0x01, 0x02, 0x03};
    std::string path = make_temp_file(".dat", tiny);
    ASSERT_FALSE(path.empty());

    EXPECT_FALSE(demuxer.open(path));

    remove_temp_file(path);
}

// ===========================================================================
// State machine
// ===========================================================================

TEST_F(HapDemuxerTest, StateMachineIdleReadyError)
{
    // Before open: Idle.
    EXPECT_EQ(demuxer.getState(), dancehap::DemuxState::Idle);

    // Successful open: Ready.
    ASSERT_TRUE(demuxer.open(DANCEHAP_TEST_ASSET));
    EXPECT_EQ(demuxer.getState(), dancehap::DemuxState::Ready);

    demuxer.close();
    EXPECT_EQ(demuxer.getState(), dancehap::DemuxState::Idle);

    // Failed open: Error.
    EXPECT_FALSE(demuxer.open("/tmp/nope.mov"));
    EXPECT_EQ(demuxer.getState(), dancehap::DemuxState::Error);

    // Close resets to Idle.
    demuxer.close();
    EXPECT_EQ(demuxer.getState(), dancehap::DemuxState::Idle);
}

// ===========================================================================
// Packet reading
// ===========================================================================

TEST_F(HapDemuxerTest, ReadNextVideoPacketReturnsValidData)
{
    ASSERT_TRUE(demuxer.open(DANCEHAP_TEST_ASSET));

    auto pkt = demuxer.readNextVideoPacket();
    EXPECT_TRUE(pkt.valid);
    EXPECT_FALSE(pkt.data.empty());
    EXPECT_EQ(pkt.stream_index, 0);  // video stream
    EXPECT_GE(pkt.pts_us, 0);

    demuxer.close();
}

TEST_F(HapDemuxerTest, ReadNextAudioPacketReturnsValidData)
{
    ASSERT_TRUE(demuxer.open(DANCEHAP_TEST_ASSET));

    auto pkt = demuxer.readNextAudioPacket();
    EXPECT_TRUE(pkt.valid);
    EXPECT_FALSE(pkt.data.empty());

    demuxer.close();
}

TEST_F(HapDemuxerTest, VideoPacketsEventuallyHitEOF)
{
    ASSERT_TRUE(demuxer.open(DANCEHAP_TEST_ASSET));

    // In stub mode, the test asset simulates 150 video frames (5s * 30fps).
    int count = 0;
    while (true) {
        auto pkt = demuxer.readNextVideoPacket();
        if (!pkt.valid) break;
        ++count;
    }

    // Should have read approximately 150 frames.
    EXPECT_EQ(count, 150);

    demuxer.close();
}

TEST_F(HapDemuxerTest, ReadBeforeOpenReturnsInvalid)
{
    auto pkt = demuxer.readNextVideoPacket();
    EXPECT_FALSE(pkt.valid);
}

// ===========================================================================
// Integration: hap_clip_source + demuxer
// ===========================================================================

class HapClipDemuxIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override { obs_stub_reset(); }
};

TEST_F(HapClipDemuxIntegrationTest, CreateWithValidPathOpensDemuxer)
{
    const obs_source_info *info = hap_clip_source_get_info();
    ASSERT_NE(info, nullptr);
    ASSERT_NE(info->create, nullptr);
    ASSERT_NE(info->destroy, nullptr);

    obs_data_t *settings = obs_data_create();
    info->get_defaults(settings);
    settings->strings["path"] = DANCEHAP_TEST_ASSET;

    void *data = info->create(settings, nullptr);
    ASSERT_NE(data, nullptr);

    // get_width should report the clip width (256) if the demuxer opened.
    ASSERT_NE(info->get_width, nullptr);
    ASSERT_NE(info->get_height, nullptr);
    EXPECT_EQ(info->get_width(data), 256u);
    EXPECT_EQ(info->get_height(data), 256u);

    info->destroy(data);
    obs_data_release(settings);
}

TEST_F(HapClipDemuxIntegrationTest, CreateWithEmptyPathReturnsZeroDimensions)
{
    const obs_source_info *info = hap_clip_source_get_info();
    ASSERT_NE(info, nullptr);

    obs_data_t *settings = obs_data_create();
    info->get_defaults(settings);

    void *data = info->create(settings, nullptr);
    ASSERT_NE(data, nullptr);

    // No clip loaded → dimensions are 0.
    EXPECT_EQ(info->get_width(data), 0u);
    EXPECT_EQ(info->get_height(data), 0u);

    info->destroy(data);
    obs_data_release(settings);
}

TEST_F(HapClipDemuxIntegrationTest, CreateWithNonExistentPathDoesNotCrash)
{
    const obs_source_info *info = hap_clip_source_get_info();
    ASSERT_NE(info, nullptr);

    obs_data_t *settings = obs_data_create();
    info->get_defaults(settings);
    settings->strings["path"] = "/tmp/does_not_exist.mov";

    // Should not crash, source stays valid.
    void *data = info->create(settings, nullptr);
    ASSERT_NE(data, nullptr);

    // Dimensions 0 (demuxer failed to open).
    EXPECT_EQ(info->get_width(data), 0u);

    info->destroy(data);
    obs_data_release(settings);
}

TEST_F(HapClipDemuxIntegrationTest, UpdatePathTriggersDemuxerReload)
{
    const obs_source_info *info = hap_clip_source_get_info();
    ASSERT_NE(info, nullptr);

    // Start with empty path.
    obs_data_t *settings = obs_data_create();
    info->get_defaults(settings);

    void *data = info->create(settings, nullptr);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(info->get_width(data), 0u);

    // Change path to the test asset.
    settings->strings["path"] = DANCEHAP_TEST_ASSET;
    info->update(data, settings);

    // Now dimensions should be 256x256.
    EXPECT_EQ(info->get_width(data), 256u);
    EXPECT_EQ(info->get_height(data), 256u);

    info->destroy(data);
    obs_data_release(settings);
}
