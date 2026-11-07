/*
 * Open Media Transport muxer
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <string.h>

#include "config_components.h"

#include "libavutil/channel_layout.h"
#include "libavutil/common.h"
#include "libavutil/intfloat.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avformat.h"
#include "internal.h"
#include "mux.h"
#include "url.h"

#define OMT_HEADER_SIZE 16
#define OMT_VIDEO_HEADER_SIZE 32
#define OMT_AUDIO_HEADER_SIZE 24
#define OMT_TIME_BASE 10000000
#define OMT_MAX_FRAME_SIZE (256 * 1024 * 1024)

enum OMTChannel {
    OMT_CHANNEL_VIDEO,
    OMT_CHANNEL_AUDIO,
    OMT_CHANNEL_COUNT,
};

typedef struct OMTMuxContext {
    const AVClass *class;
    URLContext *listener;
    URLContext *receiver[OMT_CHANNEL_COUNT];
    int stream_index[OMT_CHANNEL_COUNT];
    int listen_timeout;
} OMTMuxContext;

#define OFFSET(x) offsetof(OMTMuxContext, x)
#define E AV_OPT_FLAG_ENCODING_PARAM
static const AVOption omt_mux_options[] = {
    { "listen_timeout", "receiver connection timeout in milliseconds", OFFSET(listen_timeout),
      AV_OPT_TYPE_INT, { .i64 = 30000 }, 1, INT_MAX, E },
    { NULL }
};

static const AVClass omt_mux_class = {
    .class_name = "omt muxer",
    .item_name  = av_default_item_name,
    .option     = omt_mux_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static int omt_write_all(URLContext *h, const uint8_t *buf, int size)
{
    int written = 0;

    while (written < size) {
        int ret = ffurl_write(h, buf + written, size - written);
        if (ret <= 0)
            return ret < 0 ? ret : AVERROR(EIO);
        written += ret;
    }
    return 0;
}

static int omt_write_frame(URLContext *h, int type, int64_t pts,
                           const uint8_t *extended, int extended_size,
                           const uint8_t *data, int data_size)
{
    uint8_t header[OMT_HEADER_SIZE] = { 1 };
    uint64_t frame_size = (uint64_t)extended_size + data_size;
    int ret;

    if (frame_size > OMT_MAX_FRAME_SIZE || frame_size > INT_MAX)
        return AVERROR(EINVAL);
    header[1] = type;
    AV_WL64(header + 2, pts);
    AV_WL16(header + 10, 0);
    AV_WL32(header + 12, frame_size);
    ret = omt_write_all(h, header, sizeof(header));
    if (ret < 0)
        return ret;
    if (extended_size && (ret = omt_write_all(h, extended, extended_size)) < 0)
        return ret;
    return omt_write_all(h, data, data_size);
}

static int omt_read_subscription(URLContext *client, int *channel)
{
    static const char *video_sub = "<OMTSubscribe Video=\"true\" />";
    static const char *audio_sub = "<OMTSubscribe Audio=\"true\" />";
    uint8_t header[OMT_HEADER_SIZE];

    for (int i = 0; i < 8; i++) {
        uint32_t data_size;
        char xml[4097];
        int ret = ffurl_read_complete(client, header, sizeof(header));
        if (ret < 0)
            return ret;
        if (ret != sizeof(header) || header[0] != 1 || header[1] != 1 ||
            AV_RL16(header + 10))
            return AVERROR_INVALIDDATA;
        data_size = AV_RL32(header + 12);
        if (!data_size || data_size >= sizeof(xml))
            return AVERROR_INVALIDDATA;
        ret = ffurl_read_complete(client, (uint8_t *)xml, data_size);
        if (ret < 0)
            return ret;
        if (ret != data_size)
            return AVERROR_EOF;
        xml[data_size] = 0;
        if (!strcmp(xml, video_sub)) {
            *channel = OMT_CHANNEL_VIDEO;
            return 0;
        }
        if (!strcmp(xml, audio_sub)) {
            *channel = OMT_CHANNEL_AUDIO;
            return 0;
        }
    }
    return AVERROR_INVALIDDATA;
}

static int omt_open_listener(AVFormatContext *ctx)
{
    OMTMuxContext *s = ctx->priv_data;
    char protocol[16], hostname[256], path[1024], authorization[256];
    char tcp_url[1024];
    int port = -1, ret;

    av_url_split(protocol, sizeof(protocol), authorization, sizeof(authorization),
                 hostname, sizeof(hostname), &port, path, sizeof(path), ctx->url);
    if (strcmp(protocol, "omt") || port < 1 || port > 65535) {
        av_log(ctx, AV_LOG_ERROR, "Expected an OMT output URL of the form omt://host:port\n");
        return AVERROR(EINVAL);
    }
    ret = ff_url_join(tcp_url, sizeof(tcp_url), "tcp", NULL, hostname, port,
                      "?listen=2&listen_timeout=%d", s->listen_timeout);
    if (ret < 0 || ret >= sizeof(tcp_url))
        return AVERROR(EINVAL);
    return ffurl_open_whitelist(&s->listener, tcp_url, AVIO_FLAG_READ_WRITE,
                                &ctx->interrupt_callback, NULL,
                                ctx->protocol_whitelist, ctx->protocol_blacklist, NULL);
}

static int omt_accept_receivers(AVFormatContext *ctx)
{
    OMTMuxContext *s = ctx->priv_data;
    int required = 0;

    for (int i = 0; i < OMT_CHANNEL_COUNT; i++)
        required += s->stream_index[i] >= 0;

    for (int accepted = 0; accepted < required; ) {
        URLContext *client = NULL;
        int channel, ret = ffurl_accept(s->listener, &client);
        if (ret < 0) {
            if (accepted && ret == AVERROR(ETIMEDOUT))
                break;
            return ret;
        }
        ret = omt_read_subscription(client, &channel);
        if (ret < 0) {
            ffurl_closep(&client);
            return ret;
        }
        if (s->stream_index[channel] < 0 || s->receiver[channel]) {
            ffurl_closep(&client);
            continue;
        }
        s->receiver[channel] = client;
        accepted++;
    }
    return 0;
}

static int omt_write_header(AVFormatContext *ctx)
{
    OMTMuxContext *s = ctx->priv_data;
    int ret;

    for (int i = 0; i < OMT_CHANNEL_COUNT; i++)
        s->stream_index[i] = -1;

    for (unsigned i = 0; i < ctx->nb_streams; i++) {
        AVCodecParameters *par = ctx->streams[i]->codecpar;
        int channel;

        if (par->codec_type == AVMEDIA_TYPE_VIDEO)
            channel = OMT_CHANNEL_VIDEO;
        else if (par->codec_type == AVMEDIA_TYPE_AUDIO)
            channel = OMT_CHANNEL_AUDIO;
        else
            return AVERROR(ENOSYS);
        if (s->stream_index[channel] >= 0) {
            av_log(ctx, AV_LOG_ERROR, "OMT supports at most one stream per media channel\n");
            return AVERROR(ENOSYS);
        }
        s->stream_index[channel] = i;
    }

    if ((ret = omt_open_listener(ctx)) < 0 ||
        (ret = omt_accept_receivers(ctx)) < 0) {
        for (int i = 0; i < OMT_CHANNEL_COUNT; i++)
            ffurl_closep(&s->receiver[i]);
        ffurl_closep(&s->listener);
        return ret;
    }
    if (!s->receiver[OMT_CHANNEL_VIDEO] && !s->receiver[OMT_CHANNEL_AUDIO]) {
        ffurl_closep(&s->listener);
        return AVERROR(ETIMEDOUT);
    }
    return 0;
}

static int omt_video_header(AVFormatContext *ctx, AVStream *st,
                            uint8_t header[OMT_VIDEO_HEADER_SIZE])
{
    AVCodecParameters *par = st->codecpar;
    enum AVPixelFormat pix_fmt = par->format;
    AVRational frame_rate = av_guess_frame_rate(ctx, st, NULL);
    AVRational sar = st->sample_aspect_ratio.num > 0 ? st->sample_aspect_ratio :
                     (AVRational){ 1, 1 };
    uint32_t codec;
    uint32_t flags = 0;
    int width = par->width, height = par->height;
    float aspect;

    if (par->codec_id == AV_CODEC_ID_VMIX) {
        if (par->bits_per_raw_sample > 8) {
            av_log(ctx, AV_LOG_ERROR, "High-bit-depth VMX1 output is not supported yet\n");
            return AVERROR_PATCHWELCOME;
        }
        codec = MKTAG('V', 'M', 'X', '1');
    } else if (par->codec_id == AV_CODEC_ID_RAWVIDEO) {
        switch (pix_fmt) {
        case AV_PIX_FMT_UYVY422: codec = MKTAG('U', 'Y', 'V', 'Y'); break;
        case AV_PIX_FMT_YUYV422: codec = MKTAG('Y', 'U', 'Y', '2'); break;
        case AV_PIX_FMT_BGRA:
            codec = MKTAG('B', 'G', 'R', 'A');
            flags |= 2;
            break;
        case AV_PIX_FMT_NV12: codec = MKTAG('N', 'V', '1', '2'); break;
        case AV_PIX_FMT_YUV420P: codec = MKTAG('Y', 'V', '1', '2'); break;
        case AV_PIX_FMT_P216LE:
            codec = MKTAG('P', '2', '1', '6');
            flags |= 16;
            break;
        default:
            av_log(ctx, AV_LOG_ERROR, "Unsupported OMT raw video pixel format %s\n",
                   av_get_pix_fmt_name(pix_fmt));
            return AVERROR_PATCHWELCOME;
        }
    } else {
        av_log(ctx, AV_LOG_ERROR, "Unsupported OMT video codec %s\n",
               avcodec_get_name(par->codec_id));
        return AVERROR_PATCHWELCOME;
    }
    if (width <= 0 || height <= 0 || frame_rate.num <= 0 || frame_rate.den <= 0)
        return AVERROR_INVALIDDATA;
    memset(header, 0, OMT_VIDEO_HEADER_SIZE);
    AV_WL32(header, codec);
    AV_WL32(header + 4, width);
    AV_WL32(header + 8, height);
    AV_WL32(header + 12, frame_rate.num);
    AV_WL32(header + 16, frame_rate.den);
    aspect = (float)width * sar.num / ((float)height * sar.den);
    AV_WL32(header + 20, av_float2int(aspect));
    AV_WL32(header + 24, flags);
    if (par->color_space == AVCOL_SPC_SMPTE170M)
        AV_WL32(header + 28, 601);
    else if (par->color_space == AVCOL_SPC_BT709)
        AV_WL32(header + 28, 709);
    return 0;
}

static int omt_write_packet(AVFormatContext *ctx, AVPacket *pkt)
{
    OMTMuxContext *s = ctx->priv_data;
    AVStream *st = ctx->streams[pkt->stream_index];
    AVCodecParameters *par = st->codecpar;
    int channel = par->codec_type == AVMEDIA_TYPE_VIDEO ? OMT_CHANNEL_VIDEO : OMT_CHANNEL_AUDIO;
    URLContext *client = s->receiver[channel];
    uint8_t extended[OMT_VIDEO_HEADER_SIZE];
    int64_t pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
    int ret;

    if (!client)
        return 0;
    pts = pts == AV_NOPTS_VALUE ? 0 : av_rescale_q(pts, st->time_base,
                                                   (AVRational){ 1, OMT_TIME_BASE });
    if (channel == OMT_CHANNEL_VIDEO) {
        if ((ret = omt_video_header(ctx, st, extended)) < 0)
            return ret;
        return omt_write_frame(client, 2, pts, extended, sizeof(extended),
                               pkt->data, pkt->size);
    } else {
        int channels = par->ch_layout.nb_channels;
        int samples;
        size_t expected, planar_size;
        uint8_t header[OMT_AUDIO_HEADER_SIZE] = { 0 };
        uint8_t *planar;

        if (par->codec_id != AV_CODEC_ID_PCM_F32LE || channels <= 0 || channels > 32 ||
            av_size_mult((size_t)channels, 4, &expected) < 0 ||
            pkt->size <= 0 || pkt->size % expected)
            return AVERROR_PATCHWELCOME;
        samples = pkt->size / expected;
        if (av_size_mult((size_t)samples, (size_t)channels * 4, &planar_size) < 0 ||
            planar_size > OMT_MAX_FRAME_SIZE)
            return AVERROR(EINVAL);
        planar = av_malloc(planar_size);
        if (!planar)
            return AVERROR(ENOMEM);
        for (int channel_index = 0; channel_index < channels; channel_index++)
            for (int sample = 0; sample < samples; sample++)
                memcpy(planar + ((size_t)channel_index * samples + sample) * 4,
                       pkt->data + ((size_t)sample * channels + channel_index) * 4, 4);
        AV_WL32(header, MKTAG('F', 'P', 'A', '1'));
        AV_WL32(header + 4, par->sample_rate);
        AV_WL32(header + 8, samples);
        AV_WL32(header + 12, channels);
        AV_WL32(header + 16, channels == 32 ? UINT32_MAX : (1U << channels) - 1);
        ret = omt_write_frame(client, 4, pts, header, sizeof(header), planar, planar_size);
        av_free(planar);
        return ret;
    }
}

static void omt_close(AVFormatContext *ctx)
{
    OMTMuxContext *s = ctx->priv_data;
    for (int i = 0; i < OMT_CHANNEL_COUNT; i++)
        ffurl_closep(&s->receiver[i]);
    ffurl_closep(&s->listener);
}

static int omt_write_trailer(AVFormatContext *ctx)
{
    omt_close(ctx);
    return 0;
}

static void omt_deinit(AVFormatContext *ctx)
{
    omt_close(ctx);
}

const FFOutputFormat ff_omt_muxer = {
    .p.name           = "omt",
    .p.long_name      = NULL_IF_CONFIG_SMALL("Open Media Transport"),
    .p.audio_codec    = AV_CODEC_ID_PCM_F32LE,
    .p.video_codec    = AV_CODEC_ID_RAWVIDEO,
    .p.flags           = AVFMT_NOFILE,
    .priv_data_size   = sizeof(OMTMuxContext),
    .p.priv_class     = &omt_mux_class,
    .write_header    = omt_write_header,
    .write_packet    = omt_write_packet,
    .write_trailer   = omt_write_trailer,
    .deinit           = omt_deinit,
};