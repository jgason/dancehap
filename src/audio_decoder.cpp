// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// audio_decoder.cpp — AAC→PCM float audio decoder.
//
// Phase 1.5.c: decodes audio packets from HapDemuxer into interleaved
// float PCM for OBS output.
//
// Build modes:
//   DANCEHAP_HAVE_FFMPEG → real libavcodec implementation.
//   Otherwise            → stub: generates a sinusoid based on PTS.

#include "audio_decoder.hpp"
#include "obs_compat.hpp"  // blog()

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ===========================================================================
//  Real FFmpeg implementation
// ===========================================================================
#ifdef DANCEHAP_HAVE_FFMPEG

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace dancehap {

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct AudioDecoder::Impl {
    const AVCodec    *codec        = nullptr;
    AVCodecContext    *codec_ctx   = nullptr;
    AVFrame           *frame       = nullptr;
    SwrContext        *swr         = nullptr;

    int sample_rate   = 0;
    int channels      = 0;
    bool ready        = false;

    ~Impl()
    {
        if (swr) swr_free(&swr);
        if (frame) av_frame_free(&frame);
        if (codec_ctx) avcodec_free_context(&codec_ctx);
    }

    void reset()
    {
        if (swr) swr_free(&swr);
        if (frame) av_frame_free(&frame);
        if (codec_ctx) avcodec_free_context(&codec_ctx);
        codec       = nullptr;
        swr         = nullptr;
        sample_rate = 0;
        channels    = 0;
        ready       = false;
    }
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

AudioDecoder::AudioDecoder()
    : pimpl_(std::make_unique<Impl>())
{
}

AudioDecoder::~AudioDecoder() = default;

AudioDecoder::AudioDecoder(AudioDecoder &&) noexcept = default;
AudioDecoder &AudioDecoder::operator=(AudioDecoder &&) noexcept = default;

bool AudioDecoder::init(const AudioInfo &info)
{
    pimpl_->reset();

    if (info.sample_rate <= 0 || info.channels <= 0) {
        blog(LOG_WARNING, "[DanceHAP] AudioDecoder::init — invalid params");
        return false;
    }

    // Find decoder by name (e.g. "aac") or fall back to generic.
    pimpl_->codec = avcodec_find_decoder_by_name(info.codec_name.c_str());
    if (!pimpl_->codec) {
        blog(LOG_WARNING, "[DanceHAP] AudioDecoder: codec '%s' not found",
             info.codec_name.c_str());
        return false;
    }

    pimpl_->codec_ctx = avcodec_alloc_context3(pimpl_->codec);
    if (!pimpl_->codec_ctx) {
        blog(LOG_WARNING, "[DanceHAP] AudioDecoder: could not alloc context");
        return false;
    }

    pimpl_->codec_ctx->sample_rate    = info.sample_rate;
    av_channel_layout_default(&pimpl_->codec_ctx->ch_layout, info.channels);
    pimpl_->codec_ctx->request_sample_fmt = AV_SAMPLE_FMT_FLT;

    if (avcodec_open2(pimpl_->codec_ctx, pimpl_->codec, nullptr) < 0) {
        blog(LOG_WARNING, "[DanceHAP] AudioDecoder: avcodec_open2 failed");
        avcodec_free_context(&pimpl_->codec_ctx);
        return false;
    }

    pimpl_->frame = av_frame_alloc();
    if (!pimpl_->frame) {
        blog(LOG_WARNING, "[DanceHAP] AudioDecoder: av_frame_alloc failed");
        avcodec_free_context(&pimpl_->codec_ctx);
        return false;
    }

    pimpl_->sample_rate = info.sample_rate;
    pimpl_->channels    = info.channels;
    pimpl_->ready       = true;

    blog(LOG_INFO, "[DanceHAP] AudioDecoder initialized: %s %dHz %dch",
         info.codec_name.c_str(), info.sample_rate, info.channels);
    return true;
}

bool AudioDecoder::isReady() const
{
    return pimpl_->ready;
}

// ---------------------------------------------------------------------------
// Decode
// ---------------------------------------------------------------------------

std::vector<float> AudioDecoder::decode(const DemuxPacket &packet)
{
    std::vector<float> result;

    if (!pimpl_->ready || !packet.valid)
        return result;

    // Build AVPacket from DemuxPacket data.
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return result;

    pkt->data = const_cast<uint8_t *>(packet.data.data());
    pkt->size = static_cast<int>(packet.data.size());
    pkt->pts  = packet.pts_us;
    pkt->dts  = packet.dts_us;

    int ret = avcodec_send_packet(pimpl_->codec_ctx, pkt);
    av_packet_free(&pkt);

    if (ret < 0) {
        blog(LOG_WARNING, "[DanceHAP] AudioDecoder: avcodec_send_packet failed: %d", ret);
        return result;
    }

    // Collect all frames produced by this packet.
    int channels = pimpl_->channels;

    while (true) {
        ret = avcodec_receive_frame(pimpl_->codec_ctx, pimpl_->frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0) {
            blog(LOG_WARNING, "[DanceHAP] AudioDecoder: avcodec_receive_frame failed: %d", ret);
            break;
        }

        int nb_samples = pimpl_->frame->nb_samples;
        if (nb_samples <= 0) continue;

        AVSampleFormat fmt = static_cast<AVSampleFormat>(pimpl_->frame->format);
        int frame_channels = pimpl_->frame->ch_layout.nb_channels;
        if (frame_channels <= 0) frame_channels = channels;

        if (fmt == AV_SAMPLE_FMT_FLT) {
            // Packed float — already interleaved.
            const float *data = reinterpret_cast<const float *>(pimpl_->frame->data[0]);
            size_t count = static_cast<size_t>(nb_samples) * frame_channels;
            result.insert(result.end(), data, data + count);
        } else if (fmt == AV_SAMPLE_FMT_FLTP) {
            // Planar float — interleave.
            size_t before = result.size();
            result.resize(before + static_cast<size_t>(nb_samples) * frame_channels);
            for (int s = 0; s < nb_samples; ++s) {
                for (int ch = 0; ch < frame_channels; ++ch) {
                    const float *plane = reinterpret_cast<const float *>(
                        pimpl_->frame->data[ch]);
                    result[before + s * frame_channels + ch] = plane[s];
                }
            }
        } else {
            // Other formats: use SwrContext to convert to FLT packed.
            if (!pimpl_->swr) {
                AVChannelLayout out_layout;
                av_channel_layout_default(&out_layout, frame_channels);
                ret = swr_alloc_set_opts2(&pimpl_->swr,
                    &out_layout, AV_SAMPLE_FMT_FLT, pimpl_->sample_rate,
                    &pimpl_->frame->ch_layout, fmt, pimpl_->frame->sample_rate,
                    0, nullptr);
                av_channel_layout_uninit(&out_layout);
                if (ret < 0 || !pimpl_->swr) {
                    blog(LOG_WARNING, "[DanceHAP] AudioDecoder: swr setup failed");
                    av_frame_unref(pimpl_->frame);
                    continue;
                }
                swr_init(pimpl_->swr);
            }

            int dst_nb_samples = swr_get_out_samples(pimpl_->swr, nb_samples);
            std::vector<uint8_t> dst_buf(
                static_cast<size_t>(dst_nb_samples) * frame_channels * sizeof(float));
            uint8_t *dst_ptrs[1] = { dst_buf.data() };
            int dst_linesize = 0;
            int converted = swr_convert(pimpl_->swr,
                dst_ptrs, dst_nb_samples,
                const_cast<const uint8_t **>(pimpl_->frame->data),
                nb_samples);
            if (converted > 0) {
                const float *fdata = reinterpret_cast<const float *>(dst_buf.data());
                result.insert(result.end(), fdata, fdata + static_cast<size_t>(converted) * frame_channels);
            }
            (void)dst_linesize;
        }

        av_frame_unref(pimpl_->frame);
    }

    return result;
}

} // namespace dancehap

// ===========================================================================
//  Stub implementation (no FFmpeg)
// ===========================================================================
#else // !DANCEHAP_HAVE_FFMPEG

namespace dancehap {

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct AudioDecoder::Impl {
    int  sample_rate = 0;
    int  channels    = 0;
    bool ready       = false;

    // Phase accumulator for continuous sinusoid across packets.
    double phase = 0.0;
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

AudioDecoder::AudioDecoder()
    : pimpl_(std::make_unique<Impl>())
{
}

AudioDecoder::~AudioDecoder() = default;

AudioDecoder::AudioDecoder(AudioDecoder &&) noexcept = default;
AudioDecoder &AudioDecoder::operator=(AudioDecoder &&) noexcept = default;

bool AudioDecoder::init(const AudioInfo &info)
{
    pimpl_->sample_rate = info.sample_rate;
    pimpl_->channels    = info.channels;
    pimpl_->phase       = 0.0;
    pimpl_->ready       = (info.sample_rate > 0 && info.channels > 0);
    return pimpl_->ready;
}

bool AudioDecoder::isReady() const
{
    return pimpl_->ready;
}

// ---------------------------------------------------------------------------
// Decode (stub: sinusoid based on PTS)
// ---------------------------------------------------------------------------

std::vector<float> AudioDecoder::decode(const DemuxPacket &packet)
{
    std::vector<float> result;

    if (!pimpl_->ready || !packet.valid)
        return result;

    // Determine how many samples to produce for this packet.
    // Stub audio packets are spaced 1000µs apart (see hap_demuxer.cpp stub).
    // We produce samples for the duration implied by the packet PTS.
    // For a 10ms pullAudio request at 48kHz → 480 frames.
    // The stub decoder is called per-packet, so we produce a reasonable
    // number of samples based on a 1ms packet spacing (48 samples at 48kHz).
    // However, pullAudio() may request more or fewer — the ClipPlayer will
    // accumulate samples from multiple decode() calls.

    // Use a fixed duration for each stub packet: 1000µs = 1ms.
    // At 48kHz, that's 48 samples per channel.
    const int64_t packet_duration_us = 1000;
    int frames = static_cast<int>(
        packet_duration_us * pimpl_->sample_rate / 1'000'000LL);
    if (frames <= 0) frames = 1;

    int channels = pimpl_->channels;
    result.reserve(static_cast<size_t>(frames) * channels);

    // Generate a 440Hz sinusoid with continuous phase across packets.
    const double freq     = 440.0;
    const double sample_dur = 1.0 / static_cast<double>(pimpl_->sample_rate);
    const double amplitude  = 0.2;  // -14 dBFS, audible but not clipping

    for (int i = 0; i < frames; ++i) {
        double sample = amplitude * std::sin(2.0 * M_PI * freq * pimpl_->phase);
        for (int ch = 0; ch < channels; ++ch) {
            result.push_back(static_cast<float>(sample));
        }
        pimpl_->phase += sample_dur;
    }

    return result;
}

} // namespace dancehap

#endif // DANCEHAP_HAVE_FFMPEG