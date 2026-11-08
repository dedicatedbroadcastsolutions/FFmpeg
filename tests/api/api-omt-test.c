/*
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

#include <stdio.h>
#include <string.h>

#include "libavutil/intfloat.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/random_seed.h"
#include "libavutil/thread.h"
#include "libavutil/dict.h"
#include "libavutil/time.h"

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavformat/url.h"

typedef struct OMTTestCase {
    uint32_t codec;
    uint32_t flags;
    enum AVPixelFormat pixel_format;
    const uint8_t *wire_data;
    int wire_size;
    const uint8_t *expected_data;
    int expected_size;
} OMTTestCase;

typedef struct OMTTestServer {
    URLContext *listener;
    const OMTTestCase *test;
    int result;
} OMTTestServer;

static int write_all(URLContext *url, const uint8_t *data, int size)
{
    int written = 0;

    while (written < size) {
        int ret = ffurl_write(url, data + written, size - written);
        if (ret <= 0)
            return ret < 0 ? ret : AVERROR(EIO);
        written += ret;
    }
    return 0;
}

static void *omt_test_server(void *arg)
{
    OMTTestServer *server = arg;
    URLContext *client = NULL;
    uint8_t header[16], extended[32], message[128];
    int ret;

    ret = ffurl_accept(server->listener, &client);
    if (ret < 0)
        goto end;
    for (int i = 0; i < 3; i++) {
        ret = ffurl_read_complete(client, header, sizeof(header));
        if (ret < 0 || ret != sizeof(header) || header[0] != 1 || header[1] != 1) {
            ret = ret < 0 ? ret : AVERROR_INVALIDDATA;
            goto end;
        }
        uint32_t size = AV_RL32(header + 12);
        if (!size || size > sizeof(message)) {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }
        ret = ffurl_read_complete(client, message, size);
        if (ret < 0 || ret != size) {
            ret = ret < 0 ? ret : AVERROR_EOF;
            goto end;
        }
    }

    memset(extended, 0, sizeof(extended));
    AV_WL32(extended, server->test->codec);
    AV_WL32(extended + 4, 2);
    AV_WL32(extended + 8, 2);
    AV_WL32(extended + 12, 30);
    AV_WL32(extended + 16, 1);
    AV_WL32(extended + 20, av_float2int(1.0f));
    AV_WL32(extended + 24, server->test->flags);
    AV_WL32(extended + 28, 709);

    memset(header, 0, sizeof(header));
    header[0] = 1;
    header[1] = 2;
    AV_WL64(header + 2, 123);
    AV_WL32(header + 12, sizeof(extended) + server->test->wire_size);
    ret = write_all(client, header, sizeof(header));
    if (ret >= 0)
        ret = write_all(client, extended, sizeof(extended));
    if (ret >= 0)
        ret = write_all(client, server->test->wire_data, server->test->wire_size);

end:
    ffurl_closep(&client);
    server->result = ret;
    return NULL;
}

static int omt_test_one(const OMTTestCase *test)
{
    OMTTestServer server = { .test = test };
    AVFormatContext *format = NULL;
    AVPacket *packet = NULL;
    pthread_t thread;
    char listen_url[128], input_url[128];
    int port = -1, ret = AVERROR(EADDRINUSE);

    for (int i = 0; i < 128; i++) {
        port = 49152 + (av_get_random_seed() + i * 7919) % (65536 - 49152);
        snprintf(listen_url, sizeof(listen_url),
                 "tcp://127.0.0.1:%d?listen=2&listen_timeout=5000&timeout=5000000",
                 port);
        ret = ffurl_open_whitelist(&server.listener, listen_url,
                                   AVIO_FLAG_READ_WRITE, NULL, NULL,
                                   NULL, NULL, NULL);
        if (ret >= 0)
            break;
    }
    if (ret < 0)
        return ret;
    ret = pthread_create(&thread, NULL, omt_test_server, &server);
    if (ret) {
        ffurl_closep(&server.listener);
        return AVERROR(ret);
    }

    snprintf(input_url, sizeof(input_url), "omt://127.0.0.1:%d", port);
    ret = avformat_open_input(&format, input_url, av_find_input_format("omt"), NULL);
    if (ret < 0)
        goto end;
    packet = av_packet_alloc();
    if (!packet) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    ret = av_read_frame(format, packet);
    if (ret < 0)
        goto end;
    if (format->nb_streams != 1 ||
        format->streams[0]->codecpar->format != test->pixel_format ||
        packet->size != test->expected_size ||
        memcmp(packet->data, test->expected_data, test->expected_size))
        ret = AVERROR_INVALIDDATA;

end:
    av_packet_free(&packet);
    avformat_close_input(&format);
    pthread_join(thread, NULL);
    ffurl_closep(&server.listener);
    if (ret >= 0 && server.result < 0)
        ret = server.result;
    return ret;
}

static int omt_test_mux_one(const OMTTestCase *test)
{
    AVFormatContext *format = NULL;
    AVPacket *packet = NULL;
    URLContext *client = NULL;
    AVDictionary *options = NULL;
    AVStream *stream;
    char output_url[128], tcp_url[128];
    uint8_t header[16], extended[32], wire_data[128];
    uint32_t size;
    int port = -1, ret = AVERROR(EADDRINUSE), header_written = 0;
    const char *stage = "allocate output";

    for (int i = 0; i < 128; i++) {
        port = 49152 + (av_get_random_seed() + i * 6151) % (65536 - 49152);
        snprintf(output_url, sizeof(output_url), "omt://127.0.0.1:%d", port);
        ret = avformat_alloc_output_context2(&format,
                                             av_guess_format("omt", NULL, NULL),
                                             NULL, output_url);
        if (ret < 0)
            goto end;
        stage = "create stream";
        stream = avformat_new_stream(format, NULL);
        if (!stream) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        stream->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
        stream->codecpar->format = test->pixel_format;
        stream->codecpar->width = 2;
        stream->codecpar->height = 2;
        stream->codecpar->bits_per_raw_sample =
            test->pixel_format == AV_PIX_FMT_YUVA422P10LE ? 10 : 8;
        stream->time_base = (AVRational){ 1, 30 };
        stream->r_frame_rate = (AVRational){ 30, 1 };
        stream->avg_frame_rate = (AVRational){ 30, 1 };
        stream->sample_aspect_ratio = (AVRational){ 1, 1 };
        stage = "write output header";
        ret = avformat_write_header(format, NULL);
        if (ret >= 0) {
            header_written = 1;
            break;
        }
        avformat_free_context(format);
        format = NULL;
        if (ret != AVERROR(EADDRINUSE))
            goto end;
    }
    if (!header_written)
        goto end;

    snprintf(tcp_url, sizeof(tcp_url), "tcp://127.0.0.1:%d", port);
    stage = "set receiver timeout";
    ret = av_dict_set_int(&options, "rw_timeout", 2000000, 0);
    if (ret < 0)
        goto end;
    ret = ffurl_open_whitelist(&client, tcp_url, AVIO_FLAG_READ_WRITE, NULL,
                               &options, NULL, NULL, NULL);
    av_dict_free(&options);
    if (ret < 0)
        goto end;
    stage = "send subscription";
    static const char subscribe[] = "<OMTSubscribe Video=\"true\" />";
    memset(header, 0, sizeof(header));
    header[0] = 1;
    header[1] = 1;
    AV_WL32(header + 12, sizeof(subscribe) - 1);
    ret = write_all(client, header, sizeof(header));
    if (ret >= 0)
        ret = write_all(client, (const uint8_t *)subscribe, sizeof(subscribe) - 1);
    if (ret < 0)
        goto end;
    av_usleep(100000);

    packet = av_packet_alloc();
    stage = "allocate test packet";
    if (!packet) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    ret = av_new_packet(packet, test->expected_size);
    if (ret < 0)
        goto end;
    memcpy(packet->data, test->expected_data, test->expected_size);
    packet->stream_index = 0;
    stage = "write test packet";
    packet->pts = packet->dts = 123;
    ret = av_write_frame(format, packet);
    if (ret < 0)
        goto end;

    stage = "read OMT frame header";
    ret = ffurl_read_complete(client, header, sizeof(header));
    if (ret < 0 || ret != sizeof(header)) {
        ret = ret < 0 ? ret : AVERROR_EOF;
        goto end;
    }
    size = AV_RL32(header + 12);
    if (header[0] != 1 || header[1] != 2 || AV_RL64(header + 2) != 41000000 ||
        AV_RL16(header + 10) || size != sizeof(extended) + test->wire_size) {
        av_log(NULL, AV_LOG_ERROR,
               "OMT mux frame header differs: version=%u type=%u pts=%"PRId64" size=%u\n",
               header[0], header[1], AV_RL64(header + 2), size);
        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    stage = "read OMT video extension";
    ret = ffurl_read_complete(client, extended, sizeof(extended));
    if (ret < 0 || ret != sizeof(extended)) {
        ret = ret < 0 ? ret : AVERROR_EOF;
        goto end;
    }
    if (AV_RL32(extended) != test->codec || AV_RL32(extended + 4) != 2 ||
        AV_RL32(extended + 8) != 2 || AV_RL32(extended + 24) != test->flags) {
        av_log(NULL, AV_LOG_ERROR,
               "OMT mux video header differs: codec=%08x flags=%u\n",
               AV_RL32(extended), AV_RL32(extended + 24));
        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    stage = "read OMT media payload";
    ret = ffurl_read_complete(client, wire_data, test->wire_size);
    if (ret < 0 || ret != test->wire_size) {
        ret = ret < 0 ? ret : AVERROR_EOF;
        goto end;
    }
    if (memcmp(wire_data, test->wire_data, test->wire_size)) {
        av_log(NULL, AV_LOG_ERROR, "OMT mux payload differs for codec %08x\n",
               test->codec);
        ret = AVERROR_INVALIDDATA;
    } else
        ret = 0;

end:
    if (ret < 0)
        av_log(NULL, AV_LOG_ERROR, "OMT mux test failed during %s: %s\n",
               stage, av_err2str(ret));
    av_dict_free(&options);
    av_packet_free(&packet);
    ffurl_closep(&client);
    if (format) {
        if (header_written) {
            int trailer_ret = av_write_trailer(format);
            if (ret >= 0 && trailer_ret < 0)
                ret = trailer_ret;
        }
        avformat_free_context(format);
    }
    return ret;
}

int main(void)
{
    static const uint8_t uyvy[] = {
        128, 16, 128, 64, 129, 32, 129, 80,
    };
    static const uint8_t yuy2[] = {
        16, 128, 64, 128, 32, 129, 80, 129,
    };
    static const uint8_t bgra[] = {
        1, 2, 3, 4, 5, 6, 7, 8,
        9, 10, 11, 12, 13, 14, 15, 16,
    };
    static const uint8_t bgrx[] = {
        32, 33, 34, 35, 36, 37, 38, 39,
        40, 41, 42, 43, 44, 45, 46, 47,
    };
    static const uint8_t nv12[] = {
        16, 32, 64, 80, 128, 129,
    };
    static const uint8_t p216[] = {
        0x00, 0x10, 0x40, 0x10, 0x80, 0x10, 0xc0, 0x10,
        0x00, 0x14, 0x80, 0x16, 0x40, 0x14, 0xc0, 0x16,
    };
    static const uint8_t uyva_wire[] = {
        128, 16, 128, 64, 129, 32, 129, 80, 0, 64, 128, 255,
    };
    static const uint8_t uyva_planar[] = {
        16, 64, 32, 80, 128, 129, 128, 129, 0, 64, 128, 255,
    };
    static const uint8_t yv12_wire[] = {
        16, 32, 64, 80, 200, 100,
    };
    static const uint8_t yv12_planar[] = {
        16, 32, 64, 80, 100, 200,
    };
    static const uint8_t pa16_wire[] = {
        0x00, 0x10, 0x40, 0x10, 0x80, 0x10, 0xc0, 0x10,
        0x00, 0x14, 0x80, 0x16, 0x40, 0x14, 0xc0, 0x16,
        0x00, 0x00, 0x00, 0x40, 0x00, 0x80, 0xc0, 0xff,
    };
    static const uint8_t pa16_planar[] = {
        64, 0, 65, 0, 66, 0, 67, 0,
        80, 0, 81, 0, 90, 0, 91, 0,
        0, 0, 0, 1, 0, 2, 255, 3,
    };
    static const OMTTestCase tests[] = {
                { MKTAG('U', 'Y', 'V', 'Y'), 0, AV_PIX_FMT_UYVY422,
                    uyvy, sizeof(uyvy), uyvy, sizeof(uyvy) },
                { MKTAG('Y', 'U', 'Y', '2'), 0, AV_PIX_FMT_YUYV422,
                    yuy2, sizeof(yuy2), yuy2, sizeof(yuy2) },
                { MKTAG('B', 'G', 'R', 'A'), 2, AV_PIX_FMT_BGRA,
                    bgra, sizeof(bgra), bgra, sizeof(bgra) },
        { MKTAG('B', 'G', 'R', 'X'), 0, AV_PIX_FMT_BGR0,
          bgrx, sizeof(bgrx), bgrx, sizeof(bgrx) },
                { MKTAG('N', 'V', '1', '2'), 0, AV_PIX_FMT_NV12,
                    nv12, sizeof(nv12), nv12, sizeof(nv12) },
                { MKTAG('P', '2', '1', '6'), 16, AV_PIX_FMT_P216LE,
                    p216, sizeof(p216), p216, sizeof(p216) },
        { MKTAG('Y', 'V', '1', '2'), 0, AV_PIX_FMT_YUV420P,
            yv12_wire, sizeof(yv12_wire), yv12_planar, sizeof(yv12_planar) },
        { MKTAG('U', 'Y', 'V', 'A'), 2, AV_PIX_FMT_YUVA422P,
          uyva_wire, sizeof(uyva_wire), uyva_planar, sizeof(uyva_planar) },
        { MKTAG('P', 'A', '1', '6'), 18, AV_PIX_FMT_YUVA422P10LE,
          pa16_wire, sizeof(pa16_wire), pa16_planar, sizeof(pa16_planar) },
    };
    int ret = avformat_network_init();

    if (ret < 0)
        return 1;
    for (unsigned i = 0; i < FF_ARRAY_ELEMS(tests); i++) {
        ret = omt_test_one(&tests[i]);
        if (ret >= 0)
            ret = omt_test_mux_one(&tests[i]);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "OMT API test %u failed: %s\n",
                   i, av_err2str(ret));
            break;
        }
    }
    avformat_network_deinit();
    return ret < 0;
}