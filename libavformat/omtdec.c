/*
 * Open Media Transport demuxer
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
#include <stdlib.h>

#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/channel_layout.h"
#include "libavutil/imgutils.h"
#include "libavutil/common.h"
#include "libavutil/dict.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/intfloat.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "libavcodec/packet.h"

#include "avformat.h"
#include "demux.h"
#include "internal.h"
#include "omtdns.h"
#include "url.h"

#define OMT_HEADER_SIZE 16
#define OMT_VIDEO_HEADER_SIZE 32
#define OMT_AUDIO_HEADER_SIZE 24
#define OMT_TIME_BASE 10000000
#define OMT_MAX_FRAME_SIZE (256 * 1024 * 1024)
#define OMT_MAX_DISCOVERY_MESSAGE (64 * 1024)

enum OMTChannel {
    OMT_CHANNEL_VIDEO,
    OMT_CHANNEL_AUDIO,
};

typedef struct OMTContext {
    const AVClass *class;
    URLContext *tcp;
    AVStream *stream;
    AVStream *metadata_stream;
    int channel;
    uint32_t codec_tag;
    int width;
    int height;
    int64_t rw_timeout;
    char *source_name;
    char *discovery_server;
} OMTContext;

#define OFFSET(x) offsetof(OMTContext, x)
#define D AV_OPT_FLAG_DECODING_PARAM
static const AVOption omt_options[] = {
    { "channel", "media channel to receive", OFFSET(channel), AV_OPT_TYPE_INT,
      { .i64 = OMT_CHANNEL_VIDEO }, OMT_CHANNEL_VIDEO, OMT_CHANNEL_AUDIO, D, .unit = "channel" },
    { "video", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = OMT_CHANNEL_VIDEO }, 0, 0, D, .unit = "channel" },
    { "audio", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = OMT_CHANNEL_AUDIO }, 0, 0, D, .unit = "channel" },
    { "rw_timeout", "maximum network read/write wait in microseconds",
      OFFSET(rw_timeout), AV_OPT_TYPE_INT64,
      { .i64 = 0 }, 0, INT64_MAX, D },
        { "source_name", "full OMT source name to resolve through discovery", OFFSET(source_name),
            AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, D },
        { "discovery_server", "OMT discovery server URL", OFFSET(discovery_server),
            AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, D },
    { NULL }
};

static const AVClass omt_class = {
    .class_name = "omt",
    .item_name  = av_default_item_name,
    .option     = omt_options,
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

static int omt_send_metadata(OMTContext *s, const char *xml)
{
    uint8_t header[OMT_HEADER_SIZE] = { 1, 1 };
    int size = strlen(xml) + 1;
    int ret;

    AV_WL32(header + 12, size);
    ret = omt_write_all(s->tcp, header, sizeof(header));
    if (ret < 0)
        return ret;
    return omt_write_all(s->tcp, (const uint8_t *)xml, size);
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

static int omt_xml_element(const char *xml, const char *name,
                           char *value, size_t value_size)
{
    char start_tag[32], end_tag[32];
    const char *start, *end;
    size_t length;

    if (av_strlcpy(start_tag, "<", sizeof(start_tag)) >= sizeof(start_tag) ||
        av_strlcat(start_tag, name, sizeof(start_tag)) >= sizeof(start_tag) ||
        av_strlcat(start_tag, ">", sizeof(start_tag)) >= sizeof(start_tag) ||
        av_strlcpy(end_tag, "</", sizeof(end_tag)) >= sizeof(end_tag) ||
        av_strlcat(end_tag, name, sizeof(end_tag)) >= sizeof(end_tag) ||
        av_strlcat(end_tag, ">", sizeof(end_tag)) >= sizeof(end_tag))
        return AVERROR(EINVAL);
    start = strstr(xml, start_tag);
    if (!start)
        return AVERROR_INVALIDDATA;
    start += strlen(start_tag);
    end = strstr(start, end_tag);
    if (!end)
        return AVERROR_INVALIDDATA;
    length = end - start;
    if (length >= value_size)
        return AVERROR(ENOSPC);
    memcpy(value, start, length);
    value[length] = 0;
    return 0;
}

static int omt_parse_address(const char *xml, const char *source_name,
                             char *host, size_t host_size, int *port)
{
    AVBPrint expected;
    char *expected_name = NULL;
    char name[256], port_text[16];
    char *end;
    long parsed_port;
    int ret;

    if ((ret = omt_xml_element(xml, "Name", name, sizeof(name))) < 0 ||
        (ret = omt_xml_element(xml, "Port", port_text, sizeof(port_text))) < 0)
        return ret;
    av_bprint_init(&expected, 64, sizeof(name) - 1);
    omt_xml_escape(&expected, source_name);
    if (!av_bprint_is_complete(&expected)) {
        av_bprint_finalize(&expected, NULL);
        return AVERROR(ENOSPC);
    }
    if ((ret = av_bprint_finalize(&expected, &expected_name)) < 0)
        return ret;
    if (strcmp(name, expected_name)) {
        av_free(expected_name);
        return AVERROR(EAGAIN);
    }
    av_free(expected_name);

    parsed_port = strtol(port_text, &end, 10);
    if (end == port_text || *end || parsed_port < 1 || parsed_port > 65535)
        return AVERROR_INVALIDDATA;
    ret = omt_xml_element(xml, "IPAddress", host, host_size);
    if (ret < 0)
        ret = omt_xml_element(xml, "Address", host, host_size);
    if (ret < 0 || !host[0] || !strcmp(host, "0.0.0.0") || !strcmp(host, "::"))
        return AVERROR_INVALIDDATA;
    *port = parsed_port;
    return 0;
}

static int omt_resolve_source(AVFormatContext *ctx, OMTContext *s,
                              const char *server_url, char *source_host,
                              size_t source_host_size, int *source_port)
{
    static const char subscribe[] = "<OMTSubscribe Metadata=\"true\" />";
    char protocol[16], hostname[256], path[1024], authorization[256];
    char tcp_url[1024], *xml = NULL;
    URLContext *tcp = NULL;
    AVDictionary *options = NULL;
    uint8_t header[OMT_HEADER_SIZE] = { 1, 1 };
    int port = -1, ret;

    av_url_split(protocol, sizeof(protocol), authorization, sizeof(authorization),
                 hostname, sizeof(hostname), &port, path, sizeof(path), server_url);
    if (strcmp(protocol, "omt") || !hostname[0] || port < 1 || port > 65535)
        return AVERROR(EINVAL);
    ret = ff_url_join(tcp_url, sizeof(tcp_url), "tcp", NULL, hostname, port, NULL);
    if (ret < 0 || ret >= sizeof(tcp_url))
        return AVERROR(EINVAL);
    ret = av_dict_set_int(&options, "rw_timeout",
                          s->rw_timeout ? s->rw_timeout : 5 * AV_TIME_BASE, 0);
    if (ret < 0)
        goto end;
    ret = ffurl_open_whitelist(&tcp, tcp_url, AVIO_FLAG_READ_WRITE,
                               &ctx->interrupt_callback, &options,
                               ctx->protocol_whitelist, ctx->protocol_blacklist, NULL);
    if (ret < 0)
        goto end;

    AV_WL32(header + 12, sizeof(subscribe));
    ret = omt_write_all(tcp, header, sizeof(header));
    if (ret >= 0)
        ret = omt_write_all(tcp, (const uint8_t *)subscribe, sizeof(subscribe));
    if (ret < 0)
        goto end;

    xml = av_malloc(OMT_MAX_DISCOVERY_MESSAGE + 1);
    if (!xml) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    for (;;) {
        ret = ffurl_read_complete(tcp, header, sizeof(header));
        if (ret < 0)
            goto end;
        if (ret != sizeof(header) || header[0] != 1 || header[1] != 1) {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }
        uint32_t size = AV_RL32(header + 12);
        if (!size || size > OMT_MAX_DISCOVERY_MESSAGE) {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }
        ret = ffurl_read_complete(tcp, (uint8_t *)xml, size);
        if (ret < 0 || ret != size) {
            ret = ret < 0 ? ret : AVERROR_EOF;
            goto end;
        }
        if (xml[size - 1] == 0)
            size--;
        xml[size] = 0;
        ret = omt_parse_address(xml, s->source_name, source_host,
                                source_host_size, source_port);
        if (ret == AVERROR(EAGAIN))
            continue;
        if (ret < 0)
            goto end;
        break;
    }
    ret = 0;

end:
    av_free(xml);
    av_dict_free(&options);
    ffurl_closep(&tcp);
    return ret;
}

static int omt_open(AVFormatContext *ctx)
{
    OMTContext *s = ctx->priv_data;
    char protocol[16], hostname[256], path[1024], authorization[256];
    char tcp_url[1024];
    AVDictionary *options = NULL;
    int port = -1, ret;

    if (s->source_name) {
        if (s->discovery_server) {
            ret = omt_resolve_source(ctx, s, s->discovery_server,
                                     hostname, sizeof(hostname), &port);
        }
        if (!s->discovery_server || (ret < 0 && ret != AVERROR_EXIT)) {
            if (s->discovery_server)
                av_log(ctx, AV_LOG_VERBOSE,
                       "Discovery Server lookup failed; trying DNS-SD\n");
            ret = ff_omt_dns_sd_resolve(s->source_name, hostname, sizeof(hostname),
                                        &port, &ctx->interrupt_callback,
                                        s->rw_timeout ? s->rw_timeout : 5 * AV_TIME_BASE,
                                        ctx);
        }
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Could not resolve OMT source '%s': %s\n",
                   s->source_name, av_err2str(ret));
            return ret;
        }
    } else {
        if (s->discovery_server) {
            av_log(ctx, AV_LOG_ERROR, "source_name is required with discovery_server\n");
            return AVERROR(EINVAL);
        }
        av_url_split(protocol, sizeof(protocol), authorization, sizeof(authorization),
                     hostname, sizeof(hostname), &port, path, sizeof(path), ctx->url);
        if (strcmp(protocol, "omt") || !hostname[0] || port < 1 || port > 65535) {
            av_log(ctx, AV_LOG_ERROR, "Expected an OMT URL of the form omt://host:port\n");
            return AVERROR(EINVAL);
        }
    }
    ret = ff_url_join(tcp_url, sizeof(tcp_url), "tcp", NULL, hostname, port, NULL);
    if (ret < 0 || ret >= sizeof(tcp_url))
        return AVERROR(EINVAL);
    ret = av_dict_set_int(&options, "rw_timeout", s->rw_timeout, 0);
    if (ret < 0) {
        av_dict_free(&options);
        return ret;
    }
    ret = ffurl_open_whitelist(&s->tcp, tcp_url, AVIO_FLAG_READ_WRITE,
                               &ctx->interrupt_callback, &options,
                               ctx->protocol_whitelist, ctx->protocol_blacklist, NULL);
    av_dict_free(&options);
    if (ret < 0)
        return ret;

    ret = omt_send_metadata(s, "<OMTSubscribe Metadata=\"true\" />");
    if (ret >= 0)
        ret = omt_send_metadata(s, s->channel == OMT_CHANNEL_VIDEO ?
                                "<OMTSubscribe Video=\"true\" />" :
                                "<OMTSubscribe Audio=\"true\" />");
    if (ret >= 0 && s->channel == OMT_CHANNEL_VIDEO)
        ret = omt_send_metadata(s, "<OMTSettings Quality=\"Default\" />");
    if (ret < 0)
        ffurl_closep(&s->tcp);
    return ret;
}

static int omt_skip(URLContext *h, uint32_t size)
{
    uint8_t buf[4096];

    while (size) {
        int chunk = FFMIN(size, sizeof(buf));
        int ret = ffurl_read_complete(h, buf, chunk);
        if (ret < 0)
            return ret;
        if (ret != chunk)
            return AVERROR_EOF;
        size -= chunk;
    }
    return 0;
}

static int omt_read_packet_metadata(OMTContext *s, AVPacket *pkt, uint16_t size)
{
    AVDictionary *metadata = NULL;
    AVDictionaryEntry *entry;
    uint8_t *xml, *packed, *side_data;
    size_t packed_size;
    int ret;

    if (!size)
        return 0;
    xml = av_malloc(size);
    if (!xml)
        return AVERROR(ENOMEM);
    ret = ffurl_read_complete(s->tcp, xml, size);
    if (ret < 0 || ret != size) {
        av_free(xml);
        return ret < 0 ? ret : AVERROR_EOF;
    }
    if (xml[size - 1] || memchr(xml, 0, size - 1)) {
        av_free(xml);
        return AVERROR_INVALIDDATA;
    }
    ret = av_dict_set(&metadata, "omt.xml", (const char *)xml, 0);
    av_free(xml);
    if (ret < 0)
        goto end;
    entry = av_dict_get(metadata, "omt.xml", NULL, 0);
    if (!entry || !entry->value) {
        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    packed = av_packet_pack_dictionary(metadata, &packed_size);
    if (!packed) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    side_data = av_packet_new_side_data(pkt, AV_PKT_DATA_STRINGS_METADATA, packed_size);
    if (!side_data) {
        av_free(packed);
        ret = AVERROR(ENOMEM);
        goto end;
    }
    memcpy(side_data, packed, packed_size);
    av_free(packed);
    ret = 0;

end:
    av_dict_free(&metadata);
    return ret;
}

static enum AVPixelFormat omt_pixel_format(uint32_t codec)
{
    switch (codec) {
    case MKTAG('U', 'Y', 'V', 'Y'): return AV_PIX_FMT_UYVY422;
    case MKTAG('Y', 'U', 'Y', '2'): return AV_PIX_FMT_YUYV422;
    case MKTAG('B', 'G', 'R', 'A'): return AV_PIX_FMT_BGRA;
    case MKTAG('B', 'G', 'R', 'X'): return AV_PIX_FMT_BGR0;
    case MKTAG('N', 'V', '1', '2'): return AV_PIX_FMT_NV12;
    case MKTAG('Y', 'V', '1', '2'): return AV_PIX_FMT_YUV420P;
    case MKTAG('P', '2', '1', '6'): return AV_PIX_FMT_P216LE;
    case MKTAG('U', 'Y', 'V', 'A'): return AV_PIX_FMT_YUVA422P;
    case MKTAG('P', 'A', '1', '6'): return AV_PIX_FMT_YUVA422P10LE;
    default:                         return AV_PIX_FMT_NONE;
    }
}

static void omt_unpack_uyva(uint8_t *dst, const uint8_t *src, int width, int height)
{
    size_t luma_size = (size_t)width * height;
    size_t chroma_size = luma_size / 2;
    uint8_t *y_plane = dst;
    uint8_t *u_plane = y_plane + luma_size;
    uint8_t *v_plane = u_plane + chroma_size;
    uint8_t *a_plane = v_plane + chroma_size;
    const uint8_t *alpha = src + luma_size * 2;

    for (int y = 0; y < height; y++) {
        const uint8_t *src_row = src + (size_t)y * width * 2;
        uint8_t *y_row = y_plane + (size_t)y * width;
        uint8_t *u_row = u_plane + (size_t)y * (width / 2);
        uint8_t *v_row = v_plane + (size_t)y * (width / 2);

        for (int x = 0; x < width; x += 2) {
            u_row[x / 2] = src_row[x * 2];
            y_row[x] = src_row[x * 2 + 1];
            v_row[x / 2] = src_row[x * 2 + 2];
            y_row[x + 1] = src_row[x * 2 + 3];
        }
        memcpy(a_plane + (size_t)y * width, alpha + (size_t)y * width, width);
    }
}

static void omt_unpack_pa16(uint8_t *dst, const uint8_t *src, int width, int height)
{
    size_t luma_size = (size_t)width * height;
    uint8_t *y_plane = dst;
    uint8_t *u_plane = y_plane + luma_size * 2;
    uint8_t *v_plane = u_plane + luma_size;
    uint8_t *a_plane = v_plane + luma_size;
    const uint8_t *uv_plane = src + luma_size * 2;
    const uint8_t *alpha = uv_plane + luma_size * 2;

    for (int y = 0; y < height; y++) {
        const uint8_t *y_src = src + (size_t)y * width * 2;
        const uint8_t *uv_src = uv_plane + (size_t)y * width * 2;
        const uint8_t *a_src = alpha + (size_t)y * width * 2;
        uint8_t *y_dst = y_plane + (size_t)y * width * 2;
        uint8_t *u_dst = u_plane + (size_t)y * width;
        uint8_t *v_dst = v_plane + (size_t)y * width;
        uint8_t *a_dst = a_plane + (size_t)y * width * 2;

        for (int x = 0; x < width; x++) {
            AV_WL16(y_dst + x * 2, AV_RL16(y_src + x * 2) >> 6);
            AV_WL16(a_dst + x * 2, AV_RL16(a_src + x * 2) >> 6);
        }
        for (int x = 0; x < width; x += 2) {
            AV_WL16(u_dst + x, AV_RL16(uv_src + x * 2) >> 6);
            AV_WL16(v_dst + x, AV_RL16(uv_src + x * 2 + 2) >> 6);
        }
    }
}

static void omt_unpack_yv12(uint8_t *dst, const uint8_t *src, int width, int height)
{
    size_t luma_size = (size_t)width * height;
    size_t chroma_size = luma_size / 4;

    memcpy(dst, src, luma_size);
    memcpy(dst + luma_size, src + luma_size + chroma_size, chroma_size);
    memcpy(dst + luma_size + chroma_size, src + luma_size, chroma_size);
}

static int omt_add_video_stream(AVFormatContext *ctx, const uint8_t *header)
{
    OMTContext *s = ctx->priv_data;
    uint32_t codec = AV_RL32(header);
    uint32_t flags = AV_RL32(header + 24);
    int width = AV_RL32(header + 4);
    int height = AV_RL32(header + 8);
    AVStream *st;

    if (width <= 0 || height <= 0 || width > 32768 || height > 32768 ||
        ((codec == MKTAG('U', 'Y', 'V', 'A') || codec == MKTAG('P', 'A', '1', '6')) &&
         (width & 1)) ||
        (codec == MKTAG('Y', 'V', '1', '2') && ((width | height) & 1)))
        return AVERROR_INVALIDDATA;
    if (codec == MKTAG('V', 'M', 'X', '1')) {
        if (flags & ~16U) {
            av_log(ctx, AV_LOG_ERROR,
                   "OMT VMX1 interlaced, alpha, and preview streams are not supported yet\n");
            return AVERROR_PATCHWELCOME;
        }
        st = avformat_new_stream(ctx, NULL);
        if (!st)
            return AVERROR(ENOMEM);
        st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        st->codecpar->codec_id = AV_CODEC_ID_VMIX;
        st->codecpar->codec_tag = codec;
        st->codecpar->format = flags & 16 ? AV_PIX_FMT_YUV422P10LE : AV_PIX_FMT_YUV422P;
        st->codecpar->bits_per_raw_sample = flags & 16 ? 10 : 8;
    } else {
        enum AVPixelFormat pix_fmt = omt_pixel_format(codec);
        int expected_size;

        if (flags & (1 | 4 | 8) ||
            ((flags & 2) && codec != MKTAG('B', 'G', 'R', 'A') &&
             codec != MKTAG('U', 'Y', 'V', 'A') &&
             codec != MKTAG('P', 'A', '1', '6')) ||
            ((flags & 16) && codec != MKTAG('P', '2', '1', '6') &&
             codec != MKTAG('P', 'A', '1', '6')) ||
            (codec == MKTAG('U', 'Y', 'V', 'A') && !(flags & 2)) ||
            (codec == MKTAG('P', 'A', '1', '6') &&
             (flags & (2 | 16)) != (2 | 16))) {
            av_log(ctx, AV_LOG_ERROR, "Unsupported OMT raw video flags 0x%x\n", flags);
            return AVERROR_PATCHWELCOME;
        }
        if (pix_fmt == AV_PIX_FMT_NONE) {
            av_log(ctx, AV_LOG_ERROR, "Unsupported OMT video codec tag 0x%08x\n", codec);
            return AVERROR_PATCHWELCOME;
        }
        expected_size = av_image_get_buffer_size(pix_fmt, width, height, 1);
        if (expected_size < 0)
            return expected_size;
        st = avformat_new_stream(ctx, NULL);
        if (!st)
            return AVERROR(ENOMEM);
        st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        st->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
        if (codec == MKTAG('B', 'G', 'R', 'X') ||
            codec == MKTAG('U', 'Y', 'V', 'A') ||
            codec == MKTAG('P', 'A', '1', '6'))
            st->codecpar->codec_tag = 0;
        else
            st->codecpar->codec_tag = codec;
        st->codecpar->format = pix_fmt;
        if (pix_fmt == AV_PIX_FMT_P216LE || pix_fmt == AV_PIX_FMT_YUVA422P10LE)
            st->codecpar->bits_per_raw_sample = 10;
    }
    st->codecpar->width = width;
    st->codecpar->height = height;
    int frame_rate_num = AV_RL32(header + 12);
    int frame_rate_den = AV_RL32(header + 16);
    float aspect = av_int2float(AV_RL32(header + 20));
    if (frame_rate_num <= 0 || frame_rate_den <= 0 ||
        !(aspect > 0.0f) || aspect > 1000.0f)
        return AVERROR_INVALIDDATA;
    st->avg_frame_rate = (AVRational){ frame_rate_num, frame_rate_den };
    st->sample_aspect_ratio = av_d2q((double)aspect * height / width, 10000);
    if (AV_RL32(header + 28) == 601)
        st->codecpar->color_space = AVCOL_SPC_SMPTE170M;
    else if (AV_RL32(header + 28) == 709)
        st->codecpar->color_space = AVCOL_SPC_BT709;
    avpriv_set_pts_info(st, 64, 1, OMT_TIME_BASE);
    s->stream = st;
    s->codec_tag = codec;
    s->width = width;
    s->height = height;
    return 0;
}

static int omt_add_audio_stream(AVFormatContext *ctx, const uint8_t *header)
{
    OMTContext *s = ctx->priv_data;
    uint32_t codec = AV_RL32(header);
    int sample_rate = AV_RL32(header + 4);
    int channels = AV_RL32(header + 12);
    AVStream *st;

    if (codec != MKTAG('F', 'P', 'A', '1') || sample_rate <= 0 ||
        sample_rate > 192000 || channels <= 0 || channels > 32) {
        av_log(ctx, AV_LOG_ERROR, "Unsupported OMT audio stream parameters\n");
        return AVERROR_PATCHWELCOME;
    }
    st = avformat_new_stream(ctx, NULL);
    if (!st)
        return AVERROR(ENOMEM);
    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id = AV_CODEC_ID_PCM_F32LE;
    st->codecpar->codec_tag = codec;
    st->codecpar->sample_rate = sample_rate;
    st->codecpar->bits_per_coded_sample = 32;
    st->codecpar->block_align = channels * 4;
    st->codecpar->bit_rate = (int64_t)sample_rate * channels * 32;
    st->codecpar->ch_layout.nb_channels = channels;
    avpriv_set_pts_info(st, 64, 1, OMT_TIME_BASE);
    s->stream = st;
    s->codec_tag = codec;
    return 0;
}

static int omt_add_metadata_stream(AVFormatContext *ctx)
{
    OMTContext *s = ctx->priv_data;
    AVStream *st;

    if (s->metadata_stream)
        return 0;
    st = avformat_new_stream(ctx, NULL);
    if (!st)
        return AVERROR(ENOMEM);
    st->codecpar->codec_type = AVMEDIA_TYPE_DATA;
    st->codecpar->codec_id = AV_CODEC_ID_BIN_DATA;
    avpriv_set_pts_info(st, 64, 1, OMT_TIME_BASE);
    s->metadata_stream = st;
    return 0;
}

static int omt_read_packet(AVFormatContext *ctx, AVPacket *pkt)
{
    OMTContext *s = ctx->priv_data;
    uint8_t header[OMT_HEADER_SIZE], extended[OMT_VIDEO_HEADER_SIZE];
    uint32_t data_length, media_length, extension_size;
    uint16_t metadata_length;
    int64_t timestamp;
    int ret;

    for (;;) {
        ret = ffurl_read_complete(s->tcp, header, sizeof(header));
        if (ret < 0)
            return ret;
        if (ret != sizeof(header))
            return AVERROR_EOF;
        if (header[0] != 1)
            return AVERROR_INVALIDDATA;

        data_length = AV_RL32(header + 12);
        metadata_length = AV_RL16(header + 10);
        timestamp = AV_RL64(header + 2);
        if (data_length > OMT_MAX_FRAME_SIZE || data_length < metadata_length)
            return AVERROR_INVALIDDATA;
        if (header[1] == 1) {
            ret = omt_add_metadata_stream(ctx);
            if (ret < 0)
                return ret;
            ret = av_new_packet(pkt, data_length);
            if (ret < 0)
                return ret;
            ret = ffurl_read_complete(s->tcp, pkt->data, data_length);
            if (ret < 0 || ret != data_length) {
                av_packet_unref(pkt);
                return ret < 0 ? ret : AVERROR_EOF;
            }
            pkt->stream_index = s->metadata_stream->index;
            pkt->pts = pkt->dts = timestamp;
            return 0;
        }
        if (header[1] != (s->channel == OMT_CHANNEL_VIDEO ? 2 : 4)) {
            ret = omt_skip(s->tcp, data_length);
            if (ret < 0)
                return ret;
            continue;
        }

        extension_size = s->channel == OMT_CHANNEL_VIDEO ?
                         OMT_VIDEO_HEADER_SIZE : OMT_AUDIO_HEADER_SIZE;
        if (data_length < extension_size + metadata_length)
            return AVERROR_INVALIDDATA;
        ret = ffurl_read_complete(s->tcp, extended, extension_size);
        if (ret < 0)
            return ret;
        if (ret != extension_size)
            return AVERROR_EOF;

        if (!s->stream) {
            ret = s->channel == OMT_CHANNEL_VIDEO ? omt_add_video_stream(ctx, extended) :
                                                    omt_add_audio_stream(ctx, extended);
            if (ret < 0)
                return ret;
        }

        media_length = data_length - extension_size - metadata_length;
        if (s->channel == OMT_CHANNEL_VIDEO) {
            if (AV_RL32(extended) != s->codec_tag ||
                AV_RL32(extended + 4) != s->width ||
                AV_RL32(extended + 8) != s->height)
                return AVERROR_INVALIDDATA;
            if (s->codec_tag != MKTAG('V', 'M', 'X', '1')) {
                int expected = av_image_get_buffer_size(s->stream->codecpar->format,
                                                        s->width, s->height, 1);
                if (expected < 0 || media_length != expected)
                    return AVERROR_INVALIDDATA;
            }
            if (s->codec_tag == MKTAG('U', 'Y', 'V', 'A') ||
                s->codec_tag == MKTAG('P', 'A', '1', '6') ||
                s->codec_tag == MKTAG('Y', 'V', '1', '2')) {
                uint8_t *wire = av_malloc(media_length);
                if (!wire)
                    return AVERROR(ENOMEM);
                ret = ffurl_read_complete(s->tcp, wire, media_length);
                if (ret < 0 || ret != media_length) {
                    av_free(wire);
                    return ret < 0 ? ret : AVERROR_EOF;
                }
                ret = av_new_packet(pkt, media_length);
                if (ret >= 0) {
                    if (s->codec_tag == MKTAG('U', 'Y', 'V', 'A'))
                        omt_unpack_uyva(pkt->data, wire, s->width, s->height);
                    else if (s->codec_tag == MKTAG('P', 'A', '1', '6'))
                        omt_unpack_pa16(pkt->data, wire, s->width, s->height);
                    else
                        omt_unpack_yv12(pkt->data, wire, s->width, s->height);
                }
                av_free(wire);
                if (ret < 0)
                    return ret;
            } else {
                ret = av_new_packet(pkt, media_length);
                if (ret < 0)
                    return ret;
                ret = ffurl_read_complete(s->tcp, pkt->data, media_length);
                if (ret < 0 || ret != media_length) {
                    av_packet_unref(pkt);
                    return ret < 0 ? ret : AVERROR_EOF;
                }
            }
            pkt->duration = av_rescale_q(1,
                                         (AVRational){ s->stream->avg_frame_rate.den,
                                                       s->stream->avg_frame_rate.num },
                                         s->stream->time_base);
        } else {
            int samples = AV_RL32(extended + 8);
            int channels = s->stream->codecpar->ch_layout.nb_channels;
            uint32_t active = AV_RL32(extended + 16);
            uint32_t valid_mask = channels == 32 ? UINT32_MAX : (1U << channels) - 1;
            int active_channels = av_popcount(active);
            size_t expected_input, output_size;
            uint8_t *planar;

            if (AV_RL32(extended) != s->codec_tag ||
                AV_RL32(extended + 4) != s->stream->codecpar->sample_rate ||
                AV_RL32(extended + 12) != channels || samples <= 0 ||
                (active & ~valid_mask) ||
                av_size_mult((size_t)samples, (size_t)active_channels * 4, &expected_input) < 0 ||
                av_size_mult((size_t)samples, (size_t)channels * 4, &output_size) < 0 ||
                expected_input != media_length || output_size > OMT_MAX_FRAME_SIZE)
                return AVERROR_INVALIDDATA;
            planar = av_malloc(expected_input ? expected_input : 1);
            if (!planar)
                return AVERROR(ENOMEM);
            ret = ffurl_read_complete(s->tcp, planar, expected_input);
            if (ret < 0 || ret != expected_input) {
                av_free(planar);
                return ret < 0 ? ret : AVERROR_EOF;
            }
            ret = av_new_packet(pkt, output_size);
            if (ret < 0) {
                av_free(planar);
                return ret;
            }
            memset(pkt->data, 0, output_size);
            for (int channel = 0, plane = 0; channel < channels; channel++) {
                if (active & (1U << channel)) {
                    for (int sample = 0; sample < samples; sample++)
                        memcpy(pkt->data + ((size_t)sample * channels + channel) * 4,
                               planar + ((size_t)plane * samples + sample) * 4, 4);
                    plane++;
                }
            }
            av_free(planar);
            pkt->duration = av_rescale_q(samples,
                                         (AVRational){ 1, s->stream->codecpar->sample_rate },
                                         (AVRational){ 1, OMT_TIME_BASE });
        }

        ret = omt_read_packet_metadata(s, pkt, metadata_length);
        if (ret < 0) {
            av_packet_unref(pkt);
            return ret;
        }
        pkt->stream_index = s->stream->index;
        pkt->pts = pkt->dts = timestamp;
        if (s->channel == OMT_CHANNEL_VIDEO)
            pkt->flags |= AV_PKT_FLAG_KEY;
        return 0;
    }
}

static int omt_read_header(AVFormatContext *ctx)
{
    ctx->ctx_flags |= AVFMTCTX_NOHEADER;
    return omt_open(ctx);
}

static int omt_read_close(AVFormatContext *ctx)
{
    OMTContext *s = ctx->priv_data;
    return ffurl_closep(&s->tcp);
}

const FFInputFormat ff_omt_demuxer = {
    .p.name         = "omt",
    .p.long_name    = NULL_IF_CONFIG_SMALL("Open Media Transport"),
    .p.flags        = AVFMT_NOFILE,
    .p.priv_class   = &omt_class,
    .priv_data_size = sizeof(OMTContext),
    .read_header    = omt_read_header,
    .read_packet    = omt_read_packet,
    .read_close     = omt_read_close,
};