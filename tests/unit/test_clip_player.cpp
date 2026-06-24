// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// Unit tests for ClipPlayer (Phase 1.4).
//
// Tests run in STUB mode (no FFmpeg, no Snappy, no GPU, no OBS). The demuxer
// returns synthetic metadata matching tests/assets/sample_hapa_5s.mov
// (HAPA 256x256, 30/1 fps, 5.0s, AAC 48000Hz stereo).
//
// Test groups:
//   1.4.1 State machine (8 tests)
//   1.4.2 Video timing / FPS pacing (4 tests)
//   1.4.3 Audio routing + A/V sync (4 tests)
//   1.4.4 Loop + EOF (3 tests)
//   Integration (2 tests)

#include <gtest/gtest.h>

#include "clip_player.hpp"
#include "hap_demuxer.hpp"
#include "obs_compat.hpp"

#include <algorithm>
#include <string>

#ifndef DANCEHAP_TEST_ASSET
#  define DANCEHAP_TEST_ASSET "tests/assets/sample_hapa_5s.mov"
#endif

// ===========================================================================
// Fixture
// ===========================================================================

class ClipPlayerTest : public ::testing::Test {
protected:
    dancehap::ClipPlayer player;

    // Known stub metadata (must match hap_demuxer.cpp stub constants).
    static constexpr int      WIDTH   = 256;
    static constexpr int      HEIGHT  = 256;
    static constexpr int      FPS_NUM = 30;
    static constexpr int      FPS_DEN = 1;
    static constexpr int64_t  DURATION_US = 5'000'000;  // 5 seconds
    static constexpr int      AUDIO_RATE  = 48000;
    static constexpr int      AUDIO_CH    = 2;
    static constexpr int64_t  FRAME_DUR_US = 33'333;  // 1/30s rounded

    // Total stub video packets: 30 * 5 = 150 frames.
    static constexpr int TOTAL_FRAMES = 150;
};

// ===========================================================================
// 1.4.1 — State machine
// ===========================================================================

TEST_F(ClipPlayerTest, NewPlayerIsIdle)
{
    EXPECT_EQ(player.getState(), dancehap::PlayerState::Idle);
}

TEST_F(ClipPlayerTest, LoadTransitionsToPlaying)
{
    ASSERT_TRUE(player.load(DANCEHAP_TEST_ASSET));
    EXPECT_EQ(player.getState(), dancehap::PlayerState::Playing);
}

TEST_F(ClipPlayerTest, LoadInvalidPathGoesToError)
{
    EXPECT_FALSE(player.load("/nonexistent/bad_clip.mov"));
    EXPECT_EQ(player.getState(), dancehap::PlayerState::Error);
    EXPECT_FALSE(player.getLastError().empty());
}

TEST_F(ClipPlayerTest, LoadEmptyPathGoesToError)
{
    EXPECT_FALSE(player.load(""));
    EXPECT_EQ(player.getState(), dancehap::PlayerState::Error);
}

TEST_F(ClipPlayerTest, StopReturnsToIdle)
{
    ASSERT_TRUE(player.load(DANCEHAP_TEST_ASSET));
    EXPECT_EQ(player.getState(), dancehap::PlayerState::Playing);

    player.stop();
    EXPECT_EQ(player.getState(), dancehap::PlayerState::Idle);
    EXPECT_FALSE(player.hasVideo());
    EXPECT_FALSE(player.hasAudio());
}

TEST_F(ClipPlayerTest, StopAfterErrorReturnsToIdle)
{
    EXPECT_FALSE(player.load("/nonexistent"));
    EXPECT_EQ(player.getState(), dancehap::PlayerState::Error);

    player.stop();
    EXPECT_EQ(player.getState(), dancehap::PlayerState::Idle);
}

TEST_F(ClipPlayerTest, DoubleLoadReplacesCurrent)
{
    ASSERT_TRUE(player.load(DANCEHAP_TEST_ASSET));
    // Tick a few frames.
    player.tick(0.1f);
    int frames_before = player.getFrameCount();
    EXPECT_GT(frames_before, 0);

    // Reload — should reset frame count and counters.
    ASSERT_TRUE(player.load(DANCEHAP_TEST_ASSET));
    EXPECT_EQ(player.getState(), dancehap::PlayerState::Playing);
    EXPECT_EQ(player.getFrameCount(), 0);
    EXPECT_EQ(player.getLoopCount(), 0);
    EXPECT_EQ(player.getMasterClockUs(), 0);
}

TEST_F(ClipPlayerTest, LoadPopulatesVideoAndAudioInfo)
{
    ASSERT_TRUE(player.load(DANCEHAP_TEST_ASSET));

    EXPECT_TRUE(player.hasVideo());
    EXPECT_TRUE(player.hasAudio());

    const auto &vi = player.getVideoInfo();
    EXPECT_EQ(vi.width, WIDTH);
    EXPECT_EQ(vi.height, HEIGHT);
    EXPECT_EQ(vi.variant, dancehap::HapVariant::HAPA);
    EXPECT_EQ(vi.fps_num, FPS_NUM);
    EXPECT_EQ(vi.fps_den, FPS_DEN);

    const auto &ai = player.getAudioInfo();
    EXPECT_EQ(ai.sample_rate, AUDIO_RATE);
    EXPECT_EQ(ai.channels, AUDIO_CH);
}

// ===========================================================================
// 1.4.2 — Video timing / FPS pacing
// ===========================================================================

TEST_F(ClipPlayerTest, TickBeforeNextPtsDoesNotDecode)
{
    ASSERT_TRUE(player.load(DANCEHAP_TEST_ASSET));

    // First tick with tiny dt — decodes frame 0 (pts=0, always due).
    player.tick(0.001f);  // 1000µs
    EXPECT_EQ(player.getFrameCount(), 1);

    // Next pts is now 33333µs. Tick with small dt — clock < 33333.
    player.tick(0.001f);  // clock = 2000µs
    EXPECT_EQ(player.getFrameCount(), 1);  // no new decode
}

TEST_F(ClipPlayerTest, TickAtNextPtsDecodesOneFrame)
{
    ASSERT_TRUE(player.load(DANCEHAP_TEST_ASSET));

    // Decode frame 0 with a small initial tick.
    player.tick(0.001f);   // clock = 1000, frame_count = 1
    ASSERT_EQ(player.getFrameCount(), 1);

    // Advance clock to just below next pts (33333µs).
    player.tick(0.031f);   // clock = 32000, still < 33333
    EXPECT_EQ(player.getFrameCount(), 1);

    // One more small tick crosses the threshold.
    player.tick(0.002f);   // clock = 34000 >= 33333
    EXPECT_EQ(player.getFrameCount(), 2);
}

TEST_F(ClipPlayerTest, TickBigJumpDecodesOnlyNeededFrames)
{
    ASSERT_TRUE(player.load(DANCEHAP_TEST_ASSET));

    // A huge dt (100 seconds) should NOT decode all 150 frames at once.
    // MAX_DECODE_PER_TICK caps it to 5 frames, then resyncs.
    player.tick(100.0f);

    EXPECT_LE(player.getFrameCount(), 5);
    EXPECT_GT(player.getFrameCount(), 0);
    EXPECT_EQ(player.getState(), dancehap::PlayerState::Playing);
}

TEST_F(ClipPlayerTest, TickRespectsFpsFromVideoInfo)
{
    ASSERT_TRUE(player.load(DANCEHAP_TEST_ASSET));

    // Frame duration for 30/1 fps = 33333µs.
    // Decode frame 0.
    player.tick(0.001f);
    ASSERT_EQ(player.getFrameCount(), 1);

    // Cumulative clock at 33000µs should NOT cross the 33333 threshold.
    // 1000 + 32000 = 33000 < 33333.
    player.tick(0.032f);
    EXPECT_EQ(player.getFrameCount(), 1);

    // Adding 1000µs more → 34000 >= 33333 → decode frame 1.
    player.tick(0.001f);
    EXPECT_EQ(player.getFrameCount(), 2);
}

// ===========================================================================
// 1.4.3 — Audio routing + A/V sync
// ===========================================================================

TEST_F(ClipPlayerTest, PullAudioReturnsSamplesInPlaying)
{
    ASSERT_TRUE(player.load(DANCEHAP_TEST_ASSET));

    auto audio = player.pullAudio(10'000);  // request 10ms

    EXPECT_TRUE(audio.valid);
    EXPECT_EQ(audio.channels, AUDIO_CH);
    EXPECT_EQ(audio.sample_rate, AUDIO_RATE);
    // 10ms at 48kHz = 480 frames.
    EXPECT_EQ(audio.frames, 480);
    EXPECT_EQ(audio.duration_us, 10'000);
    // Interleaved: frames * channels floats.
    EXPECT_EQ(audio.samples.size(),
              static_cast<size_t>(audio.frames) * audio.channels);
}

TEST_F(ClipPlayerTest, PullAudioEmptyInIdle)
{
    // No clip loaded — pullAudio should return invalid.
    auto audio = player.pullAudio(10'000);
    EXPECT_FALSE(audio.valid);
    EXPECT_EQ(audio.frames, 0);
}

TEST_F(ClipPlayerTest, AudioPtsDrivesVideoClock)
{
    ASSERT_TRUE(player.load(DANCEHAP_TEST_ASSET));

    // tick(0) decodes frame 0 (pts=0 is always due even at clock=0).
    player.tick(0.0f);
    ASSERT_EQ(player.getFrameCount(), 1);
    // next_video_pts is now 33333µs, clock is 0.

    // Advance audio clock by 40000µs (> one frame duration).
    player.advanceAudioClock(40'000);
    EXPECT_EQ(player.getMasterClockUs(), 40'000);

    // tick(0) should now decode frame 1 because the audio clock pushed
    // master_clock past next_video_pts.
    player.tick(0.0f);
    EXPECT_EQ(player.getFrameCount(), 2);
}

// ===========================================================================
// 1.5.c — Audio decode (AAC→PCM float)
// ===========================================================================

TEST_F(ClipPlayerTest, PullAudioProducesNonSilentSamples)
{
    ASSERT_TRUE(player.load(DANCEHAP_TEST_ASSET));

    auto audio = player.pullAudio(10'000);  // 10ms

    ASSERT_TRUE(audio.valid);
    ASSERT_FALSE(audio.samples.empty());

    // In stub mode the audio decoder produces a sinusoid based on pts.
    // At least one sample must be non-zero (non-silent).
    bool has_nonzero = false;
    for (float s : audio.samples) {
        if (s != 0.0f) {
            has_nonzero = true;
            break;
        }
    }
    EXPECT_TRUE(has_nonzero) << "pullAudio returned silence — decoder not integrated";
}

TEST_F(ClipPlayerTest, PullAudioConsecutiveChunksHaveDifferentContent)
{
    ASSERT_TRUE(player.load(DANCEHAP_TEST_ASSET));

    auto a1 = player.pullAudio(10'000);
    auto a2 = player.pullAudio(10'000);

    ASSERT_TRUE(a1.valid);
    ASSERT_TRUE(a2.valid);

    // Two consecutive 10ms chunks should have different sample content
    // because the sine phase advances with pts.
    bool different = false;
    size_t n = std::min(a1.samples.size(), a2.samples.size());
    for (size_t i = 0; i < n; ++i) {
        if (a1.samples[i] != a2.samples[i]) {
            different = true;
            break;
        }
    }
    EXPECT_TRUE(different) << "Consecutive audio chunks are identical — pts not used";
}

TEST_F(ClipPlayerTest, PullAudioAtEOFWithLoopWrapsAround)
{
    player.setLoop(true);
    ASSERT_TRUE(player.load(DANCEHAP_TEST_ASSET));

    // Pull 600 chunks of 10ms = 6 seconds total. Clip is 5 seconds.
    // After 500 pulls, audio should wrap around.
    int64_t total_us = 0;
    for (int i = 0; i < 600; ++i) {
        auto audio = player.pullAudio(10'000);
        ASSERT_TRUE(audio.valid) << "pull failed at iteration " << i;
        total_us += audio.duration_us;
    }

    // Total pulled > clip duration → audio wrapped at least once.
    EXPECT_GT(total_us, DURATION_US);
}

// ===========================================================================
// 1.4.4 — Loop + EOF
// ===========================================================================

TEST_F(ClipPlayerTest, NoLoopEndsAtEOF)
{
    player.setLoop(false);
    ASSERT_TRUE(player.load(DANCEHAP_TEST_ASSET));

    // Tick through the entire clip (150 frames at ~3 frames per 100ms tick).
    for (int i = 0; i < 500 &&
         player.getState() == dancehap::PlayerState::Playing; ++i) {
        player.tick(0.1f);
    }

    EXPECT_EQ(player.getState(), dancehap::PlayerState::Ended);
}

TEST_F(ClipPlayerTest, LoopReopenDemuxerAndResetsClock)
{
    player.setLoop(true);
    ASSERT_TRUE(player.load(DANCEHAP_TEST_ASSET));

    // Tick until at least one loop completes.
    for (int i = 0; i < 600 &&
         player.getLoopCount() < 1; ++i) {
        player.tick(0.1f);
    }

    EXPECT_GE(player.getLoopCount(), 1);
    EXPECT_EQ(player.getState(), dancehap::PlayerState::Playing);
}

TEST_F(ClipPlayerTest, MultipleLoopsNoCrash)
{
    player.setLoop(true);
    ASSERT_TRUE(player.load(DANCEHAP_TEST_ASSET));

    // Tick aggressively — should loop multiple times without crash.
    for (int i = 0; i < 2000; ++i) {
        player.tick(0.1f);
    }

    EXPECT_GE(player.getLoopCount(), 3);
    EXPECT_EQ(player.getState(), dancehap::PlayerState::Playing);
}

// ===========================================================================
// Integration
// ===========================================================================

TEST_F(ClipPlayerTest, GetTextureNullInStubMode)
{
    ASSERT_TRUE(player.load(DANCEHAP_TEST_ASSET));
    player.tick(0.1f);

    // Stub mode: no GPU → texture is nullptr.
    EXPECT_EQ(player.getTexture(), nullptr);

    // But dimensions are reported from video info.
    EXPECT_EQ(player.getVideoWidth(), WIDTH);
    EXPECT_EQ(player.getVideoHeight(), HEIGHT);
}

TEST_F(ClipPlayerTest, EndedPlayerPlayRestartsFromBeginning)
{
    player.setLoop(false);
    ASSERT_TRUE(player.load(DANCEHAP_TEST_ASSET));

    // Tick to EOF.
    for (int i = 0; i < 500 &&
         player.getState() == dancehap::PlayerState::Playing; ++i) {
        player.tick(0.1f);
    }
    ASSERT_EQ(player.getState(), dancehap::PlayerState::Ended);

    // play() on Ended should restart.
    player.play();
    EXPECT_EQ(player.getState(), dancehap::PlayerState::Playing);
    EXPECT_EQ(player.getFrameCount(), 0);  // fresh start
    EXPECT_EQ(player.getMasterClockUs(), 0);
}
