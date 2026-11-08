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
#include <stdatomic.h>

#include "config_components.h"

#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/channel_layout.h"
#include "libavutil/common.h"
#include "libavutil/intfloat.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/thread.h"
#include "libavutil/threadmessage.h"

#include "avformat.h"
#include "internal.h"
#include "mux.h"
#include "network.h"
#include "url.h"

#define OMT_HEADER_SIZE 16
#define OMT_VIDEO_HEADER_SIZE 32
#define OMT_AUDIO_HEADER_SIZE 24
#define OMT_TIME_BASE 10000000
#define OMT_MAX_FRAME_SIZE (256 * 1024 * 1024)
#define OMT_MAX_RECEIVERS 64
#define OMT_MAX_QUEUED_FRAMES 8
#define OMT_CLIENT_TIMEOUT (5 * AV_TIME_BASE)
#define OMT_MAX_DISCOVERY_MESSAGE (64 * 1024)

enum OMTChannel {
    OMT_CHANNEL_VIDEO,
    OMT_CHANNEL_AUDIO,
    OMT_CHANNEL_COUNT,
};

typedef struct OMTMuxContext {
    const AVClass *class;
    URLContext *listener;
    struct OMTReceiver *receivers;
    AVMutex receiver_lock;
    pthread_t accept_thread;
    atomic_int exiting;
    int receiver_count;
    int lock_initialized;
    int thread_started;
    char *discovery_server;
    char *source_name;
    URLContext *discovery_tcp;
    pthread_t discovery_thread;
    atomic_int discovery_exiting;
    int discovery_thread_started;
    int listener_port;
    int stream_index[OMT_CHANNEL_COUNT];
} OMTMuxContext;

typedef struct OMTReceiver {
    URLContext *tcp;
    AVThreadMessageQueue *queue;
    pthread_t thread;
    int channel;
    int thread_started;
    atomic_int failed;
    atomic_int finished;
    atomic_int error;
    struct OMTReceiver *next;
} OMTReceiver;

#define OFFSET(x) offsetof(OMTMuxContext, x)
#define E AV_OPT_FLAG_ENCODING_PARAM
static const AVOption omt_mux_options[] = {
        { "discovery_server", "OMT discovery server URL", OFFSET(discovery_server),
            AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, E },
        { "source_name", "source name advertised to OMT discovery", OFFSET(source_name),
            AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, E },
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

static void omt_xml_escape(AVBPrint *bp, const char *text)
{
    for (const uint8_t *p = (const uint8_t *)text; *p; p++) {
        switch (*p) {
        case '&':  av_bprintf(bp, "&amp;");  break;
        case '<':  av_bprintf(bp, "&lt;");   break;
        case '>':  av_bprintf(bp, "&gt;");   break;
        case '"': av_bprintf(bp, "&quot;"); break;
        case '\'': av_bprintf(bp, "&apos;"); break;
        default:
            if (*p < 0x20 && *p != '\t' && *p != '\n' && *p != '\r')
                av_bprintf(bp, " ");
            else
                av_bprint_append_data(bp, (const char *)p, 1);
        }
    }
}

static void *omt_discovery_thread(void *arg)
{
    OMTMuxContext *s = arg;
    uint8_t header[OMT_HEADER_SIZE], buffer[1024];

    while (!atomic_load(&s->discovery_exiting)) {
        int ret = ffurl_read_complete(s->discovery_tcp, header, sizeof(header));
        if (ret == AVERROR(ETIMEDOUT) || ret == AVERROR(EAGAIN))
            continue;
        if (ret < 0 || ret != sizeof(header) || header[0] != 1 || header[1] != 1)
            break;

        uint32_t size = AV_RL32(header + 12);
        if (size > OMT_MAX_DISCOVERY_MESSAGE)
            break;
        while (size) {
            int chunk = FFMIN(size, sizeof(buffer));
            ret = ffurl_read_complete(s->discovery_tcp, buffer, chunk);
            if (ret < 0 || ret != chunk)
                return NULL;
            size -= chunk;
        }
    }
    return NULL;
}

static int omt_open_discovery(AVFormatContext *ctx, OMTMuxContext *s)
{
    static const char subscribe[] = "<OMTSubscribe Metadata=\"true\" />";
    char protocol[16], hostname[256], path[1024], authorization[256];
    char tcp_url[1024], machine_name[256];
    AVBPrint xml;
    AVDictionary *options = NULL;
    uint8_t header[OMT_HEADER_SIZE] = { 1, 1 };
    char *xml_data = NULL;
    size_t xml_size;
    int port = -1, ret;

    if (!s->discovery_server)
        return 0;
    if (!s->source_name || !s->source_name[0]) {
        av_log(ctx, AV_LOG_ERROR,
               "source_name is required when discovery_server is configured\n");
        return AVERROR(EINVAL);
    }

    av_url_split(protocol, sizeof(protocol), authorization, sizeof(authorization),
                 hostname, sizeof(hostname), &port, path, sizeof(path), s->discovery_server);
    if (strcmp(protocol, "omt") || !hostname[0] || port < 1 || port > 65535) {
        av_log(ctx, AV_LOG_ERROR,
               "Expected a discovery_server URL of the form omt://host:port\n");
        return AVERROR(EINVAL);
    }
    ret = ff_url_join(tcp_url, sizeof(tcp_url), "tcp", NULL, hostname, port, NULL);
    if (ret < 0 || ret >= sizeof(tcp_url))
        return AVERROR(EINVAL);
    if ((ret = av_dict_set_int(&options, "rw_timeout", 500000, 0)) < 0)
        goto end;
    ret = ffurl_open_whitelist(&s->discovery_tcp, tcp_url, AVIO_FLAG_READ_WRITE,
                               &ctx->interrupt_callback, &options,
                               ctx->protocol_whitelist, ctx->protocol_blacklist, NULL);
    if (ret < 0)
        goto end;

    AV_WL32(header + 12, sizeof(subscribe));
    ret = omt_write_all(s->discovery_tcp, header, sizeof(header));
    if (ret >= 0)
        ret = omt_write_all(s->discovery_tcp, (const uint8_t *)subscribe, sizeof(subscribe));
    if (ret < 0)
        goto end;

    if (gethostname(machine_name, sizeof(machine_name) - 1) < 0)
        av_strlcpy(machine_name, "localhost", sizeof(machine_name));
    machine_name[sizeof(machine_name) - 1] = 0;
    av_bprint_init(&xml, 128, OMT_MAX_DISCOVERY_MESSAGE - 1);
    av_bprintf(&xml, "<OMTAddress><Name>");
    omt_xml_escape(&xml, machine_name);
    av_bprintf(&xml, " (");
    omt_xml_escape(&xml, s->source_name);
    av_bprintf(&xml,
               ")</Name><Port>%d</Port><Addresses><IPAddress>0.0.0.0</IPAddress>"
               "</Addresses></OMTAddress>", s->listener_port);
    if (!av_bprint_is_complete(&xml)) {
        av_bprint_finalize(&xml, NULL);
        ret = AVERROR(EINVAL);
        goto end;
    }
    if ((ret = av_bprint_finalize(&xml, &xml_data)) < 0)
        goto end;
    xml_size = strlen(xml_data) + 1;
    if (xml_size > UINT32_MAX || xml_size > OMT_MAX_DISCOVERY_MESSAGE) {
        ret = AVERROR(EINVAL);
        goto end;
    }
    AV_WL32(header + 12, xml_size);
    ret = omt_write_all(s->discovery_tcp, header, sizeof(header));
    if (ret >= 0)
        ret = omt_write_all(s->discovery_tcp, (const uint8_t *)xml_data, xml_size);
    if (ret < 0)
        goto end;

    atomic_init(&s->discovery_exiting, 0);
    ret = pthread_create(&s->discovery_thread, NULL, omt_discovery_thread, s);
    if (ret) {
        ret = AVERROR(ret);
        goto end;
    }
    s->discovery_thread_started = 1;

end:
    av_free(xml_data);
    av_dict_free(&options);
    if (ret < 0)
        ffurl_closep(&s->discovery_tcp);
    return ret;
}

static int omt_make_frame_packet(AVPacket **pkt, int type, int64_t pts,
                                 const uint8_t *extended, int extended_size,
                                 const uint8_t *data, int data_size)
{
    uint64_t frame_size = (uint64_t)extended_size + data_size;
    int ret;

    if (frame_size > OMT_MAX_FRAME_SIZE ||
        frame_size + OMT_HEADER_SIZE > INT_MAX)
        return AVERROR(EINVAL);
    *pkt = av_packet_alloc();
    if (!*pkt)
        return AVERROR(ENOMEM);
    ret = av_new_packet(*pkt, OMT_HEADER_SIZE + frame_size);
    if (ret < 0) {
        av_packet_free(pkt);
        return ret;
    }
    memset((*pkt)->data, 0, OMT_HEADER_SIZE);
    (*pkt)->data[0] = 1;
    (*pkt)->data[1] = type;
    AV_WL64((*pkt)->data + 2, pts);
    AV_WL16((*pkt)->data + 10, 0);
    AV_WL32((*pkt)->data + 12, frame_size);
    if (extended_size)
        memcpy((*pkt)->data + OMT_HEADER_SIZE, extended, extended_size);
    if (data_size)
        memcpy((*pkt)->data + OMT_HEADER_SIZE + extended_size, data, data_size);
    return 0;
}

static void omt_free_packet_message(void *msg)
{
    AVPacket **pkt = msg;
    av_packet_free(pkt);
}

static void omt_receiver_fail(OMTReceiver *receiver, int error)
{
    if (atomic_exchange(&receiver->failed, 1))
        return;
    atomic_store(&receiver->error, error);
    av_thread_message_queue_set_err_send(receiver->queue, error);
    av_thread_message_queue_set_err_recv(receiver->queue, AVERROR_EXIT);
    av_thread_message_flush(receiver->queue);
}

static void *omt_receiver_thread(void *arg)
{
    OMTReceiver *receiver = arg;
    AVPacket *pkt = NULL;
    int ret;

    while ((ret = av_thread_message_queue_recv(receiver->queue, &pkt, 0)) >= 0) {
        ret = omt_write_all(receiver->tcp, pkt->data, pkt->size);
        av_packet_free(&pkt);
        if (ret < 0) {
            omt_receiver_fail(receiver, ret);
            break;
        }
    }
    atomic_store(&receiver->finished, 1);
    return NULL;
}

static void omt_receiver_stop(OMTReceiver *receiver)
{
    if (receiver->queue) {
        av_thread_message_queue_set_err_send(receiver->queue, AVERROR_EOF);
        av_thread_message_flush(receiver->queue);
        av_thread_message_queue_set_err_recv(receiver->queue, AVERROR_EOF);
    }
    if (receiver->thread_started)
        pthread_join(receiver->thread, NULL);
    ffurl_closep(&receiver->tcp);
    av_thread_message_queue_free(&receiver->queue);
    av_free(receiver);
}

static void omt_reap_receivers(AVFormatContext *ctx, OMTMuxContext *s)
{
    for (;;) {
        OMTReceiver *finished = NULL;

        ff_mutex_lock(&s->receiver_lock);
        OMTReceiver **next = &s->receivers;
        while (*next) {
            if (atomic_load(&(*next)->finished)) {
                finished = *next;
                *next = finished->next;
                s->receiver_count--;
                break;
            }
            next = &(*next)->next;
        }
        ff_mutex_unlock(&s->receiver_lock);
        if (!finished)
            break;
        if (atomic_load(&finished->failed))
            av_log(ctx, AV_LOG_WARNING, "Removing OMT receiver: %s\n",
                   av_err2str(atomic_load(&finished->error)));
        omt_receiver_stop(finished);
    }
}

static int omt_queue_frame(AVFormatContext *ctx, OMTMuxContext *s, int channel,
                           int type, int64_t pts, const uint8_t *extended,
                           int extended_size, const uint8_t *data, int data_size)
{
    AVPacket *pkt = NULL;
    int ret;

    ret = omt_make_frame_packet(&pkt, type, pts, extended, extended_size,
                                data, data_size);
    if (ret < 0)
        return ret;
    ret = 0;
    omt_reap_receivers(ctx, s);
    ff_mutex_lock(&s->receiver_lock);
    for (OMTReceiver *receiver = s->receivers; receiver; receiver = receiver->next) {
        AVPacket *queued_pkt;

        if (receiver->channel != channel || atomic_load(&receiver->failed) ||
            atomic_load(&receiver->finished))
            continue;
        queued_pkt = av_packet_clone(pkt);
        if (!queued_pkt) {
            ret = AVERROR(ENOMEM);
            break;
        }
        ret = av_thread_message_queue_send(receiver->queue, &queued_pkt,
                                           AV_THREAD_MESSAGE_NONBLOCK);
        if (ret < 0) {
            av_packet_free(&queued_pkt);
            omt_receiver_fail(receiver, ret);
            ret = 0;
        }
    }
    ff_mutex_unlock(&s->receiver_lock);
    av_packet_free(&pkt);
    return ret;
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
                      "?listen=2&listen_timeout=10&timeout=5000000");
    if (ret < 0 || ret >= sizeof(tcp_url))
        return AVERROR(EINVAL);
    s->listener_port = port;
    return ffurl_open_whitelist(&s->listener, tcp_url, AVIO_FLAG_READ_WRITE,
                                &ctx->interrupt_callback, NULL,
                                ctx->protocol_whitelist, ctx->protocol_blacklist, NULL);
}

static void *omt_accept_thread(void *arg)
{
    AVFormatContext *ctx = arg;
    OMTMuxContext *s = ctx->priv_data;

    while (!atomic_load(&s->exiting)) {
        URLContext *client = NULL;
        int channel, ret = ffurl_accept(s->listener, &client);
        if (ret < 0) {
            if (ret == AVERROR(ETIMEDOUT))
                continue;
            if (!atomic_load(&s->exiting))
                av_log(ctx, AV_LOG_WARNING, "OMT receiver accept failed: %s\n",
                       av_err2str(ret));
            break;
        }
        av_opt_set_int(client, "rw_timeout", OMT_CLIENT_TIMEOUT,
                       AV_OPT_SEARCH_CHILDREN);
        ret = omt_read_subscription(client, &channel);
        if (ret < 0) {
            ffurl_closep(&client);
            continue;
        }
        if (s->stream_index[channel] < 0) {
            ffurl_closep(&client);
            continue;
        }
        OMTReceiver *receiver = av_mallocz(sizeof(*receiver));
        if (!receiver) {
            ffurl_closep(&client);
            continue;
        }
        receiver->tcp = client;
        receiver->channel = channel;
        ret = av_thread_message_queue_alloc(&receiver->queue,
                                            OMT_MAX_QUEUED_FRAMES,
                                            sizeof(AVPacket *));
        if (ret < 0) {
            ffurl_closep(&receiver->tcp);
            av_free(receiver);
            continue;
        }
        av_thread_message_queue_set_free_func(receiver->queue,
                                              omt_free_packet_message);
        atomic_init(&receiver->failed, 0);
        atomic_init(&receiver->finished, 0);
        atomic_init(&receiver->error, 0);
        ret = pthread_create(&receiver->thread, NULL, omt_receiver_thread, receiver);
        if (ret) {
            omt_receiver_stop(receiver);
            continue;
        }
        receiver->thread_started = 1;
        ff_mutex_lock(&s->receiver_lock);
        if (s->receiver_count >= OMT_MAX_RECEIVERS) {
            ff_mutex_unlock(&s->receiver_lock);
            omt_receiver_stop(receiver);
            continue;
        }
        receiver->next = s->receivers;
        s->receivers = receiver;
        s->receiver_count++;
        ff_mutex_unlock(&s->receiver_lock);
    }
    return NULL;
}

static void omt_close(AVFormatContext *ctx);

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

    if ((ret = omt_open_listener(ctx)) < 0)
        return ret;
    if ((ret = omt_open_discovery(ctx, s)) < 0) {
        ffurl_closep(&s->listener);
        return ret;
    }
    ret = ff_mutex_init(&s->receiver_lock, NULL);
    if (ret) {
        if (s->discovery_thread_started) {
            atomic_store(&s->discovery_exiting, 1);
            pthread_join(s->discovery_thread, NULL);
            s->discovery_thread_started = 0;
        }
        ffurl_closep(&s->discovery_tcp);
        ffurl_closep(&s->listener);
        return AVERROR(ret);
    }
    s->lock_initialized = 1;
    atomic_init(&s->exiting, 0);
    ret = pthread_create(&s->accept_thread, NULL, omt_accept_thread, ctx);
    if (ret) {
        omt_close(ctx);
        return AVERROR(ret);
    }
    s->thread_started = 1;
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
        if (par->bits_per_raw_sample > 10 ||
            (par->bits_per_raw_sample > 8 && par->bits_per_raw_sample != 10)) {
            av_log(ctx, AV_LOG_ERROR, "Unsupported VMX1 bit depth %d\n",
                   par->bits_per_raw_sample);
            return AVERROR_PATCHWELCOME;
        }
        codec = MKTAG('V', 'M', 'X', '1');
        if (par->bits_per_raw_sample == 10)
            flags |= 16;
    } else if (par->codec_id == AV_CODEC_ID_RAWVIDEO) {
        switch (pix_fmt) {
        case AV_PIX_FMT_UYVY422: codec = MKTAG('U', 'Y', 'V', 'Y'); break;
        case AV_PIX_FMT_YUYV422: codec = MKTAG('Y', 'U', 'Y', '2'); break;
        case AV_PIX_FMT_BGRA:
            codec = MKTAG('B', 'G', 'R', 'A');
            flags |= 2;
            break;
        case AV_PIX_FMT_BGR0:
            codec = MKTAG('B', 'G', 'R', 'X');
            break;
        case AV_PIX_FMT_YUVA422P:
            codec = MKTAG('U', 'Y', 'V', 'A');
            flags |= 2;
            break;
        case AV_PIX_FMT_YUVA422P10LE:
            codec = MKTAG('P', 'A', '1', '6');
            flags |= 2 | 16;
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
    uint8_t extended[OMT_VIDEO_HEADER_SIZE];
    uint8_t *converted_media = NULL;
    const uint8_t *media = pkt->data;
    int media_size = pkt->size;
    int64_t pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
    int ret;

    pts = pts == AV_NOPTS_VALUE ? 0 : av_rescale_q(pts, st->time_base,
                                                   (AVRational){ 1, OMT_TIME_BASE });
    if (channel == OMT_CHANNEL_VIDEO) {
        if ((ret = omt_video_header(ctx, st, extended)) < 0)
            return ret;
        if (par->format == AV_PIX_FMT_YUV420P) {
            uint8_t *planes[4];
            int linesizes[4];
            int expected = av_image_get_buffer_size(par->format, par->width,
                                                    par->height, 1);
            size_t luma_size = (size_t)par->width * par->height;
            size_t chroma_size = luma_size / 4;

            if (expected < 0 || expected > OMT_MAX_FRAME_SIZE - OMT_VIDEO_HEADER_SIZE ||
                pkt->size != expected || ((par->width | par->height) & 1))
                return AVERROR_INVALIDDATA;
            ret = av_image_fill_arrays(planes, linesizes, pkt->data,
                                       par->format, par->width, par->height, 1);
            if (ret < 0)
                return ret;
            converted_media = av_malloc(pkt->size);
            if (!converted_media)
                return AVERROR(ENOMEM);
            memcpy(converted_media, planes[0], luma_size);
            for (int y = 0; y < par->height / 2; y++) {
                memcpy(converted_media + luma_size + (size_t)y * (par->width / 2),
                       planes[2] + (ptrdiff_t)y * linesizes[2], par->width / 2);
                memcpy(converted_media + luma_size + chroma_size +
                       (size_t)y * (par->width / 2),
                       planes[1] + (ptrdiff_t)y * linesizes[1], par->width / 2);
            }
            media = converted_media;
            media_size = pkt->size;
        } else if (par->format == AV_PIX_FMT_YUVA422P ||
            par->format == AV_PIX_FMT_YUVA422P10LE) {
            uint8_t *planes[4];
            int linesizes[4];
            int ten_bit = par->format == AV_PIX_FMT_YUVA422P10LE;
            int expected = av_image_get_buffer_size(par->format, par->width,
                                                    par->height, 1);
            size_t luma_size = (size_t)par->width * par->height;

            if (expected < 0 || expected > OMT_MAX_FRAME_SIZE - OMT_VIDEO_HEADER_SIZE ||
                pkt->size != expected || (par->width & 1))
                return AVERROR_INVALIDDATA;
            ret = av_image_fill_arrays(planes, linesizes, pkt->data,
                                       par->format, par->width, par->height, 1);
            if (ret < 0)
                return ret;
            converted_media = av_malloc(pkt->size);
            if (!converted_media)
                return AVERROR(ENOMEM);
            if (ten_bit) {
                uint8_t *uv_dst = converted_media + luma_size * 2;
                uint8_t *alpha = uv_dst + luma_size * 2;
                for (int y = 0; y < par->height; y++) {
                    const uint8_t *y_row = planes[0] + (ptrdiff_t)y * linesizes[0];
                    const uint8_t *u_row = planes[1] + (ptrdiff_t)y * linesizes[1];
                    const uint8_t *v_row = planes[2] + (ptrdiff_t)y * linesizes[2];
                    const uint8_t *a_row = planes[3] + (ptrdiff_t)y * linesizes[3];
                    uint8_t *y_dst = converted_media + (size_t)y * par->width * 2;
                    uint8_t *uv_row = uv_dst + (size_t)y * par->width * 2;
                    uint8_t *a_dst = alpha + (size_t)y * par->width * 2;

                    for (int x = 0; x < par->width; x++) {
                        AV_WL16(y_dst + x * 2, (AV_RL16(y_row + x * 2) & 0x3ff) << 6);
                        AV_WL16(a_dst + x * 2, (AV_RL16(a_row + x * 2) & 0x3ff) << 6);
                    }
                    for (int x = 0; x < par->width; x += 2) {
                        AV_WL16(uv_row + x * 2, (AV_RL16(u_row + x) & 0x3ff) << 6);
                        AV_WL16(uv_row + x * 2 + 2, (AV_RL16(v_row + x) & 0x3ff) << 6);
                    }
                }
            } else {
                uint8_t *alpha = converted_media + luma_size * 2;
                for (int y = 0; y < par->height; y++) {
                    const uint8_t *y_row = planes[0] + (ptrdiff_t)y * linesizes[0];
                    const uint8_t *u_row = planes[1] + (ptrdiff_t)y * linesizes[1];
                    const uint8_t *v_row = planes[2] + (ptrdiff_t)y * linesizes[2];
                    uint8_t *dst_row = converted_media + (size_t)y * par->width * 2;

                    for (int x = 0; x < par->width; x += 2) {
                        dst_row[x * 2] = u_row[x / 2];
                        dst_row[x * 2 + 1] = y_row[x];
                        dst_row[x * 2 + 2] = v_row[x / 2];
                        dst_row[x * 2 + 3] = y_row[x + 1];
                    }
                    memcpy(alpha + (size_t)y * par->width,
                           planes[3] + (ptrdiff_t)y * linesizes[3], par->width);
                }
            }
            media = converted_media;
            media_size = pkt->size;
        }
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
        ret = omt_queue_frame(ctx, s, channel, 4, pts, header, sizeof(header),
                              planar, planar_size);
        av_free(planar);
        return ret;
    }

    ret = omt_queue_frame(ctx, s, channel, 2, pts, extended, sizeof(extended),
                          media, media_size);
    av_free(converted_media);
    return ret;
}

static void omt_close(AVFormatContext *ctx)
{
    OMTMuxContext *s = ctx->priv_data;
    if (s->discovery_thread_started) {
        atomic_store(&s->discovery_exiting, 1);
        pthread_join(s->discovery_thread, NULL);
        s->discovery_thread_started = 0;
    }
    ffurl_closep(&s->discovery_tcp);
    if (s->thread_started) {
        atomic_store(&s->exiting, 1);
        pthread_join(s->accept_thread, NULL);
        s->thread_started = 0;
    }
    if (s->lock_initialized) {
        ff_mutex_lock(&s->receiver_lock);
        OMTReceiver *receiver = s->receivers;
        s->receivers = NULL;
        s->receiver_count = 0;
        ff_mutex_unlock(&s->receiver_lock);
        while (receiver) {
            OMTReceiver *next = receiver->next;
            omt_receiver_stop(receiver);
            receiver = next;
        }
        ff_mutex_destroy(&s->receiver_lock);
        s->lock_initialized = 0;
    }
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