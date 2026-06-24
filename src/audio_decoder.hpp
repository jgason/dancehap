// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// audio_decoder.hpp — AAC→PCM float audio decoder.
//
// Phase 1.5.c: decodes audio packets (AAC or any codec) from HapDemuxer
// into interleaved float PCM suitable for OBS audio output.
//
// Build modes:
//   DANCEHAP_HAVE_FFMPEG defined → real implementation using libavcodec.
//   Undefined (stub mode) → synthetic sinusoid generator that produces
//     non-silent samples based on the packet PTS, allowing tests to
//     validate the audio path without a real AAC decoder.
//
// The decoder is initialized with AudioInfo from the demuxer and accepts
// DemuxPacket objects. Each decode() call returns a vector of interleaved
// float samples (channels interleaved).

#pragma once

#include "hap_demuxer.hpp"  // DemuxPacket, AudioInfo

#include <cstdint>
#include <vector>

namespace dancehap {

class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();

    AudioDecoder(const AudioDecoder &) = delete;
    AudioDecoder &operator=(const AudioDecoder &) = delete;

    AudioDecoder(AudioDecoder &&) noexcept;
    AudioDecoder &operator=(AudioDecoder &&) noexcept;

    /// Initialize the decoder for the given audio stream info.
    /// Must be called before decode(). Returns false on failure.
    bool init(const AudioInfo &info);

    /// Decode a single audio packet into interleaved float PCM.
    /// Returns an empty vector on error or end-of-stream.
    /// The number of samples is always a multiple of channels.
    std::vector<float> decode(const DemuxPacket &packet);

    /// Whether the decoder is ready to accept packets.
    bool isReady() const;

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace dancehap