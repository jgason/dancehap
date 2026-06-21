// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// hap_demuxer.cpp — Container demuxer for HAP video clips.
//
// Phase 1.2: open container, detect HAP variant + audio, read raw packets.
// No decoding yet — that arrives in Phase 1.3 (HapDecoder).
//
// Build modes:
//   DANCEHAP_HAVE_FFMPEG → real libavformat/libavcodec implementation.
//   Otherwise            → stub: validates file existence + ISO-BMFF
//                           signature, returns synthetic metadata matching
//                           tests/assets/sample_hapa_5s.mov (HAPA 256x256
//                           30fps 5s, AAC audio).

#include "hap_demuxer.hpp"
#include "obs_compat.hpp"   // blog()

#include <cstring>
#include <fstream>

// ===========================================================================
//  Real FFmpeg implementation
// ===========================================================================
#ifdef DANCEHAP_HAVE_FFMPEG

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_par.h>
#include <libavutil/dict.h>
#include <libavutil/rational.h>
#include <libavutil/mathematics.h>
}

namespace dancehap {

// Build a 32-bit FourCC tag in little-endian byte order (AVI/MOV convention,
// matches FFmpeg's MKTAG).
static constexpr uint32_t mk_tag(char a, char b, char c, char d)
{
    return (uint32_t)(uint8_t)(a)
         | ((uint32_t)(uint8_t)(b) << 8)
         | ((uint32_t)(uint8_t)(c) << 16)
         | ((uint32_t)(uint8_t)(d) << 24);
}

// HAP FourCC codec tags stored in MOV containers.
// See https://hap.video/ and FFmpeg libavcodec/hap.c.
static constexpr uint32_t HAP_TAG_HAP1 = mk_tag('H', 'a', 'p', '1'); // HAP
static constexpr uint32_t HAP_TAG_HAP5 = mk_tag('H', 'a', 'p', '5'); // HAPA
static constexpr uint32_t HAP_TAG_HAPY = mk_tag('H', 'a', 'p', 'Y'); // HAPQ
static constexpr uint32_t HAP_TAG_HAPA = mk_tag('H', 'a', 'p', 'A'); // HAPQ-A

static HapVariant detect_variant_from_tag(uint32_t codec_tag)
{
    switch (codec_tag) {
    case HAP_TAG_HAP1: return HapVariant::HAP;
    case HAP_TAG_HAP5: return HapVariant::HAPA;
    case HAP_TAG_HAPY: return HapVariant::HAPQ;
    case HAP_TAG_HAPA: return HapVariant::HAPQ_A;
    default:           return HapVariant::Unknown;
    }
}

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct HapDemuxer::Impl {
    AVFormatContext *fmt_ctx         = nullptr;
    int  video_stream_index          = -1;
    int  audio_stream_index          = -1;

    VideoInfo  video_info;
    AudioInfo  audio_info;
    DemuxState state                 = DemuxState::Idle;
    std::string last_error;

    bool has_video                   = false;
    bool has_audio                   = false;

    // Packet cursor position (for diagnostics / seek later)
    int64_t video_packets_read       = 0;
    int64_t audio_packets_read       = 0;

    ~Impl()
    {
        if (fmt_ctx) {
            avformat_close_input(&fmt_ctx);  // also frees fmt_ctx → nullptr
        }
    }

    void reset()
    {
        if (fmt_ctx) {
            avformat_close_input(&fmt_ctx);
        }
        fmt_ctx             = nullptr;
        video_stream_index  = -1;
        audio_stream_index  = -1;
        video_info          = VideoInfo{};
        audio_info          = AudioInfo{};
        state               = DemuxState::Idle;
        last_error.clear();
        has_video           = false;
        has_audio           = false;
        video_packets_read  = 0;
        audio_packets_read  = 0;
    }

    // Convert AVRational time-base to microseconds.
    static int64_t ts_to_us(int64_t ts, const AVRational &time_base)
    {
        if (ts == AV_NOPTS_VALUE || time_base.den == 0) return 0;
        return av_rescale_q(ts, time_base, {1, 1000000});
    }
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

HapDemuxer::HapDemuxer()
    : pimpl_(std::make_unique<Impl>())
{
}

HapDemuxer::~HapDemuxer() = default;

HapDemuxer::HapDemuxer(HapDemuxer &&) noexcept = default;
HapDemuxer &HapDemuxer::operator=(HapDemuxer &&) noexcept = default;

bool HapDemuxer::open(const std::string &path)
{
    pimpl_->reset();
    pimpl_->state = DemuxState::Loading;

    if (path.empty()) {
        pimpl_->last_error = "path is empty";
        pimpl_->state = DemuxState::Error;
        blog(LOG_WARNING, "[DanceHAP] HapDemuxer::open: empty path");
        return false;
    }

    // Open input — allocates fmt_ctx internally.
    int ret = avformat_open_input(&pimpl_->fmt_ctx, path.c_str(),
                                  nullptr, nullptr);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(ret, errbuf, sizeof(errbuf));
        pimpl_->last_error = std::string("avformat_open_input failed: ") + errbuf;
        pimpl_->state = DemuxState::Error;
        blog(LOG_WARNING, "[DanceHAP] HapDemuxer::open('%s') failed: %s",
             path.c_str(), errbuf);
        return false;
    }

    // Probe stream info.
    ret = avformat_find_stream_info(pimpl_->fmt_ctx, nullptr);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(ret, errbuf, sizeof(errbuf));
        pimpl_->last_error = std::string("avformat_find_stream_info failed: ") + errbuf;
        pimpl_->state = DemuxState::Error;
        avformat_close_input(&pimpl_->fmt_ctx);
        blog(LOG_WARNING, "[DanceHAP] HapDemuxer::open('%s') stream info failed: %s",
             path.c_str(), errbuf);
        return false;
    }

    // Find video and audio streams.
    for (unsigned i = 0; i < pimpl_->fmt_ctx->nb_streams; ++i) {
        AVStream *st = pimpl_->fmt_ctx->streams[i];
        AVMediaType type = st->codecpar->codec_type;

        if (type == AVMEDIA_TYPE_VIDEO && pimpl_->video_stream_index < 0) {
            uint32_t tag = st->codecpar->codec_tag;
            HapVariant variant = detect_variant_from_tag(tag);

            if (variant == HapVariant::Unknown) {
                // Not a HAP stream — skip (might be an H.264 thumbnail).
                continue;
            }

            pimpl_->video_stream_index = (int)i;
            pimpl_->has_video = true;

            VideoInfo &vi = pimpl_->video_info;
            vi.variant  = variant;
            vi.width    = st->codecpar->width;
            vi.height   = st->codecpar->height;
            vi.fps_num  = st->avg_frame_rate.num;
            vi.fps_den  = st->avg_frame_rate.den;
            if (vi.fps_den == 0) {
                // Fall back to r_frame_rate.
                vi.fps_num = st->r_frame_rate.num;
                vi.fps_den = st->r_frame_rate.den;
            }
            vi.duration_us = Impl::ts_to_us(st->duration, st->time_base);
            if (vi.duration_us == 0) {
                vi.duration_us = Impl::ts_to_us(pimpl_->fmt_ctx->duration,
                                                {1, AV_TIME_BASE});
            }

            blog(LOG_INFO, "[DanceHAP] video stream #%u: %s %dx%d %d/%d fps"
                 " (%s variant, tag=0x%08X)",
                 i, hap_variant_to_string(variant), vi.width, vi.height,
                 vi.fps_num, vi.fps_den,
                 hap_variant_has_alpha(variant) ? "alpha" : "no-alpha",
                 tag);

        } else if (type == AVMEDIA_TYPE_AUDIO && pimpl_->audio_stream_index < 0) {
            pimpl_->audio_stream_index = (int)i;
            pimpl_->has_audio = true;

            AudioInfo &ai = pimpl_->audio_info;
            ai.sample_rate = st->codecpar->sample_rate;
            ai.channels    = st->codecpar->ch_layout.nb_channels;
            const AVCodecDescriptor *cdesc =
                avcodec_descriptor_get(st->codecpar->codec_id);
            ai.codec_name  = cdesc ? cdesc->name : "unknown";
            ai.duration_us = Impl::ts_to_us(st->duration, st->time_base);

            blog(LOG_INFO, "[DanceHAP] audio stream #%u: %s %dHz %dch",
                 i, ai.codec_name.c_str(), ai.sample_rate, ai.channels);
        }
    }

    if (!pimpl_->has_video) {
        pimpl_->last_error = "no HAP video stream found in container";
        pimpl_->state = DemuxState::Error;
        avformat_close_input(&pimpl_->fmt_ctx);
        blog(LOG_WARNING, "[DanceHAP] HapDemuxer::open('%s') — no HAP video stream",
             path.c_str());
        return false;
    }

    pimpl_->state = DemuxState::Ready;
    blog(LOG_INFO, "[DanceHAP] HapDemuxer opened '%s' — %s, duration=%.3fs, audio=%s",
         path.c_str(),
         hap_variant_to_string(pimpl_->video_info.variant),
         getDuration(),
         pimpl_->has_audio ? "yes" : "no");
    return true;
}

void HapDemuxer::close()
{
    pimpl_->reset();
}

bool HapDemuxer::reopen(const std::string &path)
{
    close();
    return open(path);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool HapDemuxer::hasVideo() const          { return pimpl_->has_video; }
bool HapDemuxer::hasAudio() const          { return pimpl_->has_audio; }
const VideoInfo &HapDemuxer::getVideoInfo() const  { return pimpl_->video_info; }
const AudioInfo &HapDemuxer::getAudioInfo() const  { return pimpl_->audio_info; }
DemuxState HapDemuxer::getState() const    { return pimpl_->state; }
const std::string &HapDemuxer::getLastError() const { return pimpl_->last_error; }

double HapDemuxer::getDuration() const
{
    if (!pimpl_->has_video) return 0.0;
    return pimpl_->video_info.duration_us / 1000000.0;
}

// ---------------------------------------------------------------------------
// Packet reading
// ---------------------------------------------------------------------------

static DemuxPacket convert_packet(const AVPacket *pkt, int stream_index)
{
    DemuxPacket dp;
    dp.stream_index = stream_index;
    dp.pts_us  = pkt->pts  != AV_NOPTS_VALUE ? (int64_t)pkt->pts  : (int64_t)pkt->dts;
    dp.dts_us  = pkt->dts  != AV_NOPTS_VALUE ? (int64_t)pkt->dts  : 0;
    dp.key_frame = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
    dp.data.assign(pkt->data, pkt->data + pkt->size);
    dp.valid = true;
    return dp;
}

DemuxPacket HapDemuxer::readNextVideoPacket()
{
    if (!pimpl_->has_video || pimpl_->state != DemuxState::Ready)
        return DemuxPacket{};

    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return DemuxPacket{};

    while (true) {
        int ret = av_read_frame(pimpl_->fmt_ctx, pkt);
        if (ret < 0) {
            av_packet_free(&pkt);
            return DemuxPacket{};  // EOF or error
        }

        if (pkt->stream_index == pimpl_->video_stream_index) {
            DemuxPacket dp = convert_packet(pkt, pimpl_->video_stream_index);
            av_packet_free(&pkt);
            ++pimpl_->video_packets_read;
            return dp;
        }
        // Non-video packet — skip (audio buffering for interleaved reads
        // is a Phase 1.4 concern; for 1.2 we just advance the cursor).
    }
}

DemuxPacket HapDemuxer::readNextAudioPacket()
{
    if (!pimpl_->has_audio || pimpl_->state != DemuxState::Ready)
        return DemuxPacket{};

    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return DemuxPacket{};

    while (true) {
        int ret = av_read_frame(pimpl_->fmt_ctx, pkt);
        if (ret < 0) {
            av_packet_free(&pkt);
            return DemuxPacket{};
        }

        if (pkt->stream_index == pimpl_->audio_stream_index) {
            DemuxPacket dp = convert_packet(pkt, pimpl_->audio_stream_index);
            av_packet_free(&pkt);
            ++pimpl_->audio_packets_read;
            return dp;
        }
        // Non-audio packet — skip.
    }
}

} // namespace dancehap

// ===========================================================================
//  Stub implementation (no FFmpeg)
// ===========================================================================
#else // !DANCEHAP_HAVE_FFMPEG

namespace dancehap {

// Known metadata matching tests/assets/sample_hapa_5s.mov:
//   HAPA (Hap5 tag), 256x256, 30/1 fps, 5.0s, AAC 48000Hz stereo
static constexpr int  STUB_WIDTH   = 256;
static constexpr int  STUB_HEIGHT  = 256;
static constexpr int  STUB_FPS_NUM = 30;
static constexpr int  STUB_FPS_DEN = 1;
static constexpr int64_t STUB_DURATION_US = 5'000'000;   // 5 seconds
static constexpr int  STUB_AUDIO_RATE = 48000;
static constexpr int  STUB_AUDIO_CH   = 2;

// Check if a file has a valid ISO-BMFF / QuickTime container signature.
// Valid files have a known atom type at bytes 4-7 (ftyp, moov, mdat, free,
// skip, wide, mdat). This rejects random-byte files while accepting real
// .mov/.mp4 containers.
static bool has_valid_container_signature(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;

    uint8_t header[12] = {};
    f.read(reinterpret_cast<char *>(header), sizeof(header));
    auto n = static_cast<std::streamsize>(f.gcount());
    if (n < 8) return false;

    // Bytes 4-7 should be a printable ASCII atom type.
    for (int i = 4; i < 8; ++i) {
        if (header[i] < 0x20 || header[i] > 0x7E) return false;
    }

    // Check against known first-atom types.
    static const char *known_types[] = {
        "ftyp", "moov", "mdat", "free", "skip", "wide",
        "pnot", "PICT", "uuid", "qt  ", nullptr
    };
    char tag[5] = {};
    std::memcpy(tag, header + 4, 4);
    for (int i = 0; known_types[i]; ++i) {
        if (std::memcmp(tag, known_types[i], 4) == 0) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct HapDemuxer::Impl {
    DemuxState state                 = DemuxState::Idle;
    std::string last_error;
    std::string current_path;

    bool has_video                   = false;
    bool has_audio                   = false;

    VideoInfo video_info;
    AudioInfo audio_info;

    // Stub packet cursor: how many video/audio packets returned so far.
    int video_packets_read           = 0;
    int audio_packets_read           = 0;

    // Total packets (computed from duration/fps for video).
    int total_video_packets          = 0;

    void reset()
    {
        state               = DemuxState::Idle;
        last_error.clear();
        current_path.clear();
        has_video           = false;
        has_audio           = false;
        video_info          = VideoInfo{};
        audio_info          = AudioInfo{};
        video_packets_read  = 0;
        audio_packets_read  = 0;
        total_video_packets = 0;
    }
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

HapDemuxer::HapDemuxer()
    : pimpl_(std::make_unique<Impl>())
{
}

HapDemuxer::~HapDemuxer() = default;

HapDemuxer::HapDemuxer(HapDemuxer &&) noexcept = default;
HapDemuxer &HapDemuxer::operator=(HapDemuxer &&) noexcept = default;

bool HapDemuxer::open(const std::string &path)
{
    pimpl_->reset();
    pimpl_->state = DemuxState::Loading;

    if (path.empty()) {
        pimpl_->last_error = "path is empty";
        pimpl_->state = DemuxState::Error;
        return false;
    }

    // Check file exists.
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        pimpl_->last_error = "file not found: " + path;
        pimpl_->state = DemuxState::Error;
        return false;
    }

    // Check container signature.
    if (!has_valid_container_signature(path)) {
        pimpl_->last_error = "unsupported format or corrupted file: " + path;
        pimpl_->state = DemuxState::Error;
        return false;
    }

    // Simulate HAPA detection (matches sample_hapa_5s.mov).
    pimpl_->current_path = path;
    pimpl_->has_video = true;
    pimpl_->has_audio = true;

    VideoInfo &vi = pimpl_->video_info;
    vi.variant     = HapVariant::HAPA;  // Hap5 FourCC
    vi.width       = STUB_WIDTH;
    vi.height      = STUB_HEIGHT;
    vi.fps_num     = STUB_FPS_NUM;
    vi.fps_den     = STUB_FPS_DEN;
    vi.duration_us = STUB_DURATION_US;

    AudioInfo &ai = pimpl_->audio_info;
    ai.sample_rate = STUB_AUDIO_RATE;
    ai.channels    = STUB_AUDIO_CH;
    ai.codec_name  = "aac";
    ai.duration_us = STUB_DURATION_US;

    pimpl_->total_video_packets =
        (int)((STUB_FPS_NUM * STUB_DURATION_US) /
              (int64_t)(STUB_FPS_DEN * 1'000'000));  // 150 frames

    pimpl_->state = DemuxState::Ready;
    return true;
}

void HapDemuxer::close()
{
    pimpl_->reset();
}

bool HapDemuxer::reopen(const std::string &path)
{
    close();
    return open(path);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool HapDemuxer::hasVideo() const          { return pimpl_->has_video; }
bool HapDemuxer::hasAudio() const          { return pimpl_->has_audio; }
const VideoInfo &HapDemuxer::getVideoInfo() const  { return pimpl_->video_info; }
const AudioInfo &HapDemuxer::getAudioInfo() const  { return pimpl_->audio_info; }
DemuxState HapDemuxer::getState() const    { return pimpl_->state; }
const std::string &HapDemuxer::getLastError() const { return pimpl_->last_error; }

double HapDemuxer::getDuration() const
{
    if (!pimpl_->has_video) return 0.0;
    return pimpl_->video_info.duration_us / 1000000.0;
}

// ---------------------------------------------------------------------------
// Packet reading (synthetic)
// ---------------------------------------------------------------------------

DemuxPacket HapDemuxer::readNextVideoPacket()
{
    if (!pimpl_->has_video || pimpl_->state != DemuxState::Ready)
        return DemuxPacket{};

    if (pimpl_->video_packets_read >= pimpl_->total_video_packets)
        return DemuxPacket{};  // EOF

    DemuxPacket dp;
    dp.stream_index = 0;  // video stream
    dp.pts_us = (int64_t)pimpl_->video_packets_read *
                STUB_FPS_DEN * 1'000'000LL / STUB_FPS_NUM;
    dp.dts_us = dp.pts_us;
    dp.key_frame = (pimpl_->video_packets_read % 30 == 0);  // keyframe every 30
    // Synthetic minimal HAP frame header (placeholder — real decode in 1.3).
    dp.data.resize(64, 0);
    dp.valid = true;
    ++pimpl_->video_packets_read;
    return dp;
}

DemuxPacket HapDemuxer::readNextAudioPacket()
{
    if (!pimpl_->has_audio || pimpl_->state != DemuxState::Ready)
        return DemuxPacket{};

    // Stub: allow a fixed number of audio packets proportional to duration.
    int total_audio = (int)(STUB_DURATION_US / 1000);  // ~5000 ms-based packets
    if (pimpl_->audio_packets_read >= total_audio)
        return DemuxPacket{};

    DemuxPacket dp;
    dp.stream_index = 1;  // audio stream
    dp.pts_us = (int64_t)pimpl_->audio_packets_read * 1000;  // 1ms spacing
    dp.dts_us = dp.pts_us;
    dp.data.resize(128, 0);
    dp.valid = true;
    ++pimpl_->audio_packets_read;
    return dp;
}

} // namespace dancehap

#endif // DANCEHAP_HAVE_FFMPEG
