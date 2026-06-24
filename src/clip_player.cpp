// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// clip_player.cpp — Playback engine for HAP video clips.
//
// Phase 1.4 (PLAN-PHASE-1.4.md): state machine + FPS pacing + audio master
// clock (ADR-007) + loop/EOF. OBS-independent — all obs_* calls remain in
// hap_clip_source.cpp.
//
// Build modes: works identically in stub and real modes. The underlying
// HapDemuxer and HapDecoder handle the stub/real split internally.

#include "clip_player.hpp"
#include "audio_decoder.hpp"
#include "hap_decoder.hpp"
#include "hap_demuxer.hpp"
#include "obs_compat.hpp"  // blog()

#include <algorithm>
#include <cmath>

namespace dancehap {

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct ClipPlayer::Impl {
    // Owned resources
    std::unique_ptr<HapDemuxer>  demuxer;
    std::unique_ptr<HapDecoder>  decoder;
    std::unique_ptr<AudioDecoder> audio_decoder;

    // Configuration
    bool    loop              = true;

    // State
    PlayerState state         = PlayerState::Idle;
    std::string last_error;
    std::string loaded_path;    // for loop reopen

    // Timing — all in microseconds
    int64_t master_clock_us   = 0;  // current playback position
    int64_t next_video_pts_us = 0;  // PTS of next frame to decode

    // Counters
    int frame_count           = 0;  // frames decoded this playthrough
    int loop_count            = 0;  // loops completed since load

    // Audio cursor (tracks how much audio has been pulled, in microseconds)
    int64_t audio_read_us     = 0;

    // Audio sample buffer: accumulate decoded samples from multiple packets
    // to satisfy pullAudio requests that may span several packets.
    std::vector<float> audio_buffer;

    // Max frames to decode per tick to avoid catch-up storms.
    static constexpr int MAX_DECODE_PER_TICK = 5;

    // --- Helpers ---

    int64_t frameDurationUs() const
    {
        if (!demuxer || !demuxer->hasVideo()) return 33333;  // default 30 fps
        const auto &vi = demuxer->getVideoInfo();
        if (vi.fps_num <= 0 || vi.fps_den <= 0) return 33333;
        return (int64_t)vi.fps_den * 1'000'000LL / vi.fps_num;
    }

    /// Decode video frames whose PTS has become due relative to master clock.
    /// Handles loop (reopen) and EOF (Ended state) when the demuxer is
    /// exhausted.
    void decodeDueFrames()
    {
        if (state != PlayerState::Playing) return;
        if (!demuxer || !decoder) return;

        const int64_t frame_dur = frameDurationUs();
        int decoded_this_tick = 0;

        while (master_clock_us >= next_video_pts_us) {
            if (decoded_this_tick >= MAX_DECODE_PER_TICK) {
                // Too far behind — resync: skip the cursor forward.
                // This drops frames to catch up rather than decoding
                // the entire remaining clip in one tick.
                next_video_pts_us = master_clock_us + frame_dur;
                break;
            }

            DemuxPacket pkt = demuxer->readNextVideoPacket();
            if (!pkt.valid) {
                handleVideoEOF();
                return;
            }

            decoder->decode(pkt);
            ++frame_count;
            next_video_pts_us = pkt.pts_us + frame_dur;
            ++decoded_this_tick;
        }
    }

    /// Called when the demuxer video stream is exhausted.
    void handleVideoEOF()
    {
        if (loop) {
            ++loop_count;
            // Reopen the demuxer to restart from the beginning.
            if (demuxer->reopen(loaded_path)) {
                decoder->setVideoInfo(demuxer->getVideoInfo());
                if (audio_decoder && demuxer->hasAudio()) {
                    audio_decoder->init(demuxer->getAudioInfo());
                }
                master_clock_us   = 0;
                next_video_pts_us = 0;
                frame_count       = 0;
                audio_read_us     = 0;
                audio_buffer.clear();
                blog(LOG_INFO, "[DanceHAP] ClipPlayer loop #%d (reopen '%s')",
                     loop_count, loaded_path.c_str());
                // Remain in Playing — next tick resumes decoding.
            } else {
                blog(LOG_WARNING, "[DanceHAP] ClipPlayer loop reopen failed: %s",
                     demuxer->getLastError().c_str());
                state = PlayerState::Error;
            }
        } else {
            blog(LOG_INFO, "[DanceHAP] ClipPlayer reached EOF (loop=false) — Ended");
            state = PlayerState::Ended;
        }
    }
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

ClipPlayer::ClipPlayer()
    : pimpl_(std::make_unique<Impl>())
{
}

ClipPlayer::~ClipPlayer() = default;

ClipPlayer::ClipPlayer(ClipPlayer &&) noexcept = default;
ClipPlayer &ClipPlayer::operator=(ClipPlayer &&) noexcept = default;

bool ClipPlayer::load(const std::string &path)
{
    pimpl_->state = PlayerState::Loading;
    pimpl_->last_error.clear();

    // (Re)create the demuxer + decoder fresh.
    pimpl_->demuxer = std::make_unique<HapDemuxer>();
    pimpl_->decoder = std::make_unique<HapDecoder>();
    pimpl_->audio_decoder = std::make_unique<AudioDecoder>();

    if (!pimpl_->demuxer->open(path)) {
        pimpl_->last_error = pimpl_->demuxer->getLastError();
        pimpl_->state = PlayerState::Error;
        blog(LOG_WARNING, "[DanceHAP] ClipPlayer::load('%s') failed: %s",
             path.c_str(), pimpl_->last_error.c_str());
        return false;
    }

    // Configure decoder with video info from the demuxer.
    pimpl_->decoder->setVideoInfo(pimpl_->demuxer->getVideoInfo());

    // Initialize audio decoder if audio stream exists.
    if (pimpl_->demuxer->hasAudio()) {
        if (!pimpl_->audio_decoder->init(pimpl_->demuxer->getAudioInfo())) {
            blog(LOG_WARNING, "[DanceHAP] ClipPlayer::load — audio decoder init failed");
        }
    }

    // Reset timing + counters.
    pimpl_->loaded_path     = path;
    pimpl_->master_clock_us = 0;
    pimpl_->next_video_pts_us = 0;
    pimpl_->frame_count     = 0;
    pimpl_->loop_count      = 0;
    pimpl_->audio_read_us   = 0;
    pimpl_->audio_buffer.clear();
    pimpl_->state           = PlayerState::Playing;

    blog(LOG_INFO, "[DanceHAP] ClipPlayer loaded '%s' — %s %dx%d %d/%d fps "
         "(audio=%s, duration=%.3fs)",
         path.c_str(),
         hap_variant_to_string(pimpl_->demuxer->getVideoInfo().variant),
         pimpl_->demuxer->getVideoInfo().width,
         pimpl_->demuxer->getVideoInfo().height,
         pimpl_->demuxer->getVideoInfo().fps_num,
         pimpl_->demuxer->getVideoInfo().fps_den,
         pimpl_->demuxer->hasAudio() ? "yes" : "no",
         pimpl_->demuxer->getDuration());

    return true;
}

void ClipPlayer::play()
{
    if (pimpl_->state == PlayerState::Ended) {
        // Restart from beginning.
        if (!pimpl_->loaded_path.empty()) {
            load(pimpl_->loaded_path);
        }
        return;
    }
    if (pimpl_->state == PlayerState::Idle || pimpl_->state == PlayerState::Error) {
        // Nothing to play — load first.
        return;
    }
    // Already Playing or Loading — no-op.
}

void ClipPlayer::stop()
{
    pimpl_->demuxer.reset();
    pimpl_->decoder.reset();
    pimpl_->audio_decoder.reset();
    pimpl_->loaded_path.clear();
    pimpl_->last_error.clear();
    pimpl_->master_clock_us   = 0;
    pimpl_->next_video_pts_us = 0;
    pimpl_->frame_count       = 0;
    pimpl_->loop_count        = 0;
    pimpl_->audio_read_us     = 0;
    pimpl_->audio_buffer.clear();
    pimpl_->state             = PlayerState::Idle;
}

// ---------------------------------------------------------------------------
// Playback loop
// ---------------------------------------------------------------------------

void ClipPlayer::tick(float dt_seconds)
{
    if (pimpl_->state != PlayerState::Playing) return;

    // Advance master clock by wall-clock delta.
    if (dt_seconds > 0.0f) {
        pimpl_->master_clock_us +=
            static_cast<int64_t>(dt_seconds * 1'000'000.0);
    }

    pimpl_->decodeDueFrames();
}

void ClipPlayer::advanceAudioClock(int64_t audio_us)
{
    if (pimpl_->state != PlayerState::Playing) return;
    if (audio_us <= 0) return;

    // In audio-master mode, the audio output is the timing reference.
    // Advance the master clock by the audio played.
    pimpl_->master_clock_us += audio_us;
}

// ---------------------------------------------------------------------------
// Audio output
// ---------------------------------------------------------------------------

AudioOutput ClipPlayer::pullAudio(int64_t max_duration_us)
{
    AudioOutput out;

    if (pimpl_->state != PlayerState::Playing) return out;
    if (!pimpl_->demuxer || !pimpl_->demuxer->hasAudio()) return out;
    if (!pimpl_->audio_decoder || !pimpl_->audio_decoder->isReady()) return out;
    if (max_duration_us <= 0) return out;

    const auto &ai = pimpl_->demuxer->getAudioInfo();
    out.channels    = ai.channels;
    out.sample_rate = ai.sample_rate;
    out.pts_us      = pimpl_->audio_read_us;

    // Determine how much audio to produce.
    int64_t remaining = ai.duration_us - pimpl_->audio_read_us;

    if (remaining <= 0) {
        // Audio EOF.
        if (pimpl_->loop) {
            // Wrap around.
            pimpl_->audio_read_us = 0;
            out.pts_us = 0;
            remaining = ai.duration_us;
            // Reset audio decoder state for the new loop.
            pimpl_->audio_decoder->init(ai);
            pimpl_->audio_buffer.clear();
        } else {
            // No more audio — return silence.
            return out;
        }
    }

    int64_t produce_us = std::min(max_duration_us, remaining);

    // Convert microseconds to audio frames at the sample rate.
    int frames_needed = static_cast<int>(
        produce_us * ai.sample_rate / 1'000'000LL);
    if (frames_needed <= 0) return out;

    // Recompute exact duration from frame count (avoids drift).
    out.frames      = frames_needed;
    out.duration_us = static_cast<int64_t>(frames_needed) * 1'000'000LL / ai.sample_rate;

    // Decode audio packets from the demuxer to fill the request.
    // We accumulate decoded samples in audio_buffer until we have enough.
    const int channels = ai.channels;
    const size_t samples_needed = static_cast<size_t>(frames_needed) * channels;

    // Consume packets until we have enough samples in the buffer.
    while (pimpl_->audio_buffer.size() < samples_needed) {
        DemuxPacket pkt = pimpl_->demuxer->readNextAudioPacket();
        if (!pkt.valid) {
            // EOF: no more audio packets.
            break;
        }
        auto decoded = pimpl_->audio_decoder->decode(pkt);
        pimpl_->audio_buffer.insert(
            pimpl_->audio_buffer.end(), decoded.begin(), decoded.end());
    }

    // Fill out.samples from the buffer.
    if (pimpl_->audio_buffer.size() >= samples_needed) {
        out.samples.assign(
            pimpl_->audio_buffer.begin(),
            pimpl_->audio_buffer.begin() + samples_needed);
        pimpl_->audio_buffer.erase(
            pimpl_->audio_buffer.begin(),
            pimpl_->audio_buffer.begin() + samples_needed);
    } else if (!pimpl_->audio_buffer.empty()) {
        // Not enough decoded samples — use what we have, pad with silence.
        out.samples = std::move(pimpl_->audio_buffer);
        out.samples.resize(samples_needed, 0.0f);
        pimpl_->audio_buffer.clear();
    } else {
        // No samples decoded at all — fall back to silence.
        out.samples.assign(samples_needed, 0.0f);
    }

    out.valid = true;
    pimpl_->audio_read_us += out.duration_us;
    return out;
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void ClipPlayer::setLoop(bool loop)
{
    pimpl_->loop = loop;
}

bool ClipPlayer::getLoop() const
{
    return pimpl_->loop;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

PlayerState ClipPlayer::getState() const { return pimpl_->state; }

int64_t ClipPlayer::getMasterClockUs() const { return pimpl_->master_clock_us; }

int64_t ClipPlayer::getNextVideoPtsUs() const { return pimpl_->next_video_pts_us; }

int ClipPlayer::getFrameCount() const { return pimpl_->frame_count; }

int ClipPlayer::getLoopCount() const { return pimpl_->loop_count; }

bool ClipPlayer::hasVideo() const
{
    return pimpl_->demuxer && pimpl_->demuxer->hasVideo();
}

bool ClipPlayer::hasAudio() const
{
    return pimpl_->demuxer && pimpl_->demuxer->hasAudio();
}

const VideoInfo &ClipPlayer::getVideoInfo() const
{
    static const VideoInfo s_empty{};
    if (pimpl_->demuxer) return pimpl_->demuxer->getVideoInfo();
    return s_empty;
}

const AudioInfo &ClipPlayer::getAudioInfo() const
{
    static const AudioInfo s_empty{};
    if (pimpl_->demuxer) return pimpl_->demuxer->getAudioInfo();
    return s_empty;
}

gs_texture_t *ClipPlayer::getTexture() const
{
    return pimpl_->decoder ? pimpl_->decoder->getTexture() : nullptr;
}

void ClipPlayer::uploadToGpu()
{
    if (pimpl_->decoder) pimpl_->decoder->uploadToGpu();
}

int ClipPlayer::getVideoWidth() const
{
    return pimpl_->decoder ? pimpl_->decoder->getWidth() : 0;
}

int ClipPlayer::getVideoHeight() const
{
    return pimpl_->decoder ? pimpl_->decoder->getHeight() : 0;
}

const std::string &ClipPlayer::getLastError() const
{
    return pimpl_->last_error;
}

} // namespace dancehap
