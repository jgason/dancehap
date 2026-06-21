// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// clip_player.hpp — Playback engine for HAP video clips.
//
// Phase 1.4 deliverable (PLAN-PHASE-1.4.md):
//   Orchestrates HapDemuxer (Phase 1.2) + HapDecoder (Phase 1.3) into a
//   stateful playback engine with FPS-paced video decoding, A/V sync via
//   audio master clock (ADR-007), and seamless looping.
//
// Design principles:
//   • OBS-INDEPENDENT — no obs_* calls. All OBS callbacks stay in
//     hap_clip_source.cpp. This class is fully unit-testable in stub mode.
//   • pimpl idiom — hides HapDemuxer/HapDecoder ownership, clean ABI.
//   • Single master clock — driven by wall-clock dt (video master) or by
//     audio pushed (audio master, per ADR-007). Video frames are decoded
//     when their PTS falls due relative to the master clock.
//
// State machine:
//   Idle → Loading → Playing    (load succeeded)
//                   ↘ Error      (load failed)
//   Playing → Ended              (EOF, loop=false)
//   Playing → Playing            (EOF, loop=true → reopen)
//   * → Idle                     (stop)
//   Error → Idle                 (stop or new load attempt)

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "hap_demuxer.hpp"  // VideoInfo, AudioInfo, DemuxState, HapVariant
#include "obs_compat.hpp"   // gs_texture_t

namespace dancehap {

// ---------------------------------------------------------------------------
// Player state machine
// ---------------------------------------------------------------------------

enum class PlayerState {
    Idle,       // No clip loaded
    Loading,    // Demuxer open in progress (transient)
    Playing,    // Active playback
    Ended,      // Clip finished (loop=false), last frame held
    Error,      // Terminal error (bad file, decode failure)
};

inline const char *player_state_to_string(PlayerState s)
{
    switch (s) {
    case PlayerState::Idle:    return "Idle";
    case PlayerState::Loading: return "Loading";
    case PlayerState::Playing: return "Playing";
    case PlayerState::Ended:   return "Ended";
    case PlayerState::Error:   return "Error";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// AudioOutput — PCM audio pulled from the player for OBS output.
//
// In stub mode (no FFmpeg), the demuxer returns zero-filled audio packets
// that cannot be decoded to real PCM. pullAudio() therefore returns SILENCE
// (zero-filled float samples) with correct timing metadata. The timing
// (duration_us, pts_us, channels, sample_rate) is what drives the audio
// master clock — the actual sample content is irrelevant for sync.
//
// In real mode (DANCEHAP_HAVE_FFMPEG), the demuxer audio packets would be
// decoded to real PCM. Audio decoding integration is a Phase 1.5+ concern;
// for Phase 1.4 the timing correctness is validated in stub mode.
// ---------------------------------------------------------------------------

struct AudioOutput {
    std::vector<float> samples;  // Interleaved float PCM (silence in stub)
    int     frames      = 0;     // Audio frames (= samples per channel)
    int     channels    = 0;
    int     sample_rate = 0;
    int64_t pts_us      = 0;     // Presentation timestamp of first frame
    int64_t duration_us = 0;     // Duration of this chunk
    bool    valid       = false;
};

// ---------------------------------------------------------------------------
// ClipPlayer — non-copyable, movable.
//
// Typical usage:
//   ClipPlayer player;
//   player.setLoop(true);
//   if (player.load("clip.mov")) {
//       while (player.getState() == PlayerState::Playing) {
//           player.tick(0.016f);  // ~60 Hz
//           // For audio master clock:
//           //   auto audio = player.pullAudio(10000); // 10ms
//           //   player.advanceAudioClock(audio.duration_us);
//           //   // push audio.samples to OBS...
//           // Render: player.getTexture()
//       }
//   }
// ---------------------------------------------------------------------------

class ClipPlayer {
public:
    ClipPlayer();
    ~ClipPlayer();

    ClipPlayer(const ClipPlayer &) = delete;
    ClipPlayer &operator=(const ClipPlayer &) = delete;

    ClipPlayer(ClipPlayer &&) noexcept;
    ClipPlayer &operator=(ClipPlayer &&) noexcept;

    // --- Lifecycle --------------------------------------------------------

    /// Open a clip file and prepare for playback.
    /// Opens the demuxer, creates the decoder, decodes frame 0 (pre-roll),
    /// and transitions to Playing on success.
    /// On failure: state → Error, last_error set, returns false.
    /// Calling load() while already loaded replaces the current clip.
    bool load(const std::string &path);

    /// Explicitly begin/resume playback. After a successful load() the
    /// player is already Playing. play() on an Ended player restarts from
    /// the beginning. play() on Error/Idle is a no-op.
    void play();

    /// Stop playback and release all resources. State → Idle.
    void stop();

    // --- Playback loop ----------------------------------------------------

    /// Advance the master clock and decode any due video frames.
    /// dt_seconds is the wall-clock time elapsed since the last tick
    /// (typically 1/60 s from OBS video_tick).
    /// In audio-master mode, the clock is primarily driven by
    /// advanceAudioClock(); dt_seconds is still consumed to advance the
    /// clock so the player progresses even without audio.
    void tick(float dt_seconds);

    /// Advance the master clock by a known amount of audio playback time.
    /// Used in audio-master mode (ADR-007): after pushing audio to OBS,
    /// call this with the duration of audio pushed. This drives video
    /// decoding to stay in sync with the audio output.
    void advanceAudioClock(int64_t audio_us);

    // --- Audio output -----------------------------------------------------

    /// Pull up to max_duration_us of audio for output.
    /// Returns silence in stub mode with correct timing metadata.
    /// At EOF with loop=true, the audio cursor wraps around.
    /// At EOF with loop=false, returns valid=false (silence).
    AudioOutput pullAudio(int64_t max_duration_us);

    // --- Configuration ----------------------------------------------------

    void setLoop(bool loop);
    bool getLoop() const;

    // --- Queries ----------------------------------------------------------

    PlayerState getState() const;
    int64_t     getMasterClockUs() const;
    int64_t     getNextVideoPtsUs() const;
    int         getFrameCount() const;    // Frames decoded this playthrough
    int         getLoopCount() const;     // Loops completed since load
    bool        hasVideo() const;
    bool        hasAudio() const;
    const VideoInfo &getVideoInfo() const;
    const AudioInfo &getAudioInfo() const;

    /// Current decoded texture (nullptr in stub mode or before first frame).
    gs_texture_t *getTexture() const;
    int          getVideoWidth() const;
    int          getVideoHeight() const;

    const std::string &getLastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace dancehap
