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

#include "libavutil/base64.h"
#include "libavutil/dict.h"
#include "libavutil/intfloat.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/random_seed.h"
#include "libavutil/thread.h"
#include "libavutil/time.h"

#include "libavcodec/avcodec.h"
#include "libavcodec/packet.h"
#include "libavformat/avformat.h"
#include "libavformat/omtdns.h"
#include "libavformat/url.h"

typedef struct OMTTestCase {
    uint32_t codec;
    uint32_t flags;
    enum AVPixelFormat pixel_format;
    const uint8_t *wire_data;
    int wire_size;
    const uint8_t *expected_data;
    int expected_size;
    const char *metadata;
    const char *product_name;
    const char *manufacturer;
    const char *version;
    const char *connection_metadata;
    const char *input_metadata;
} OMTTestCase;

typedef struct OMTTestServer {
    URLContext *listener;
    const OMTTestCase *test;
    int result;
} OMTTestServer;

typedef struct OMTAudioTestServer {
    URLContext *listener;
    const uint8_t *wire_data;
    int wire_size;
    int result;
} OMTAudioTestServer;

typedef struct OMTTimeoutTestServer {
    URLContext *listener;
    int result;
} OMTTimeoutTestServer;

typedef struct OMTDiscoveryTestServer {
    URLContext *listener;
    char xml[512];
    int result;
    int disconnected;
} OMTDiscoveryTestServer;

typedef struct OMTDiscoveryResolverTestServer {
    URLContext *listener;
    int media_port;
    int result;
} OMTDiscoveryResolverTestServer;

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

static int read_omt_metadata(URLContext *client, const char *expected)
{
    uint8_t header[16], data[512];
    uint32_t size;
    int ret;

    ret = ffurl_read_complete(client, header, sizeof(header));
    if (ret < 0 || ret != sizeof(header))
        return ret < 0 ? ret : AVERROR_EOF;
    size = AV_RL32(header + 12);
    if (size > sizeof(data))
        return AVERROR_INVALIDDATA;
    ret = ffurl_read_complete(client, data, size);
    if (ret < 0 || ret != size)
        return ret < 0 ? ret : AVERROR_EOF;
    if (header[0] != 1 || header[1] != 1 || AV_RL16(header + 10) ||
        size != strlen(expected) + 1 || data[size - 1] ||
        strcmp((const char *)data, expected)) {
        av_log(NULL, AV_LOG_ERROR,
               "OMT metadata mismatch: v=%u type=%u header-metadata=%u size=%u payload=%s\n",
               header[0], header[1], AV_RL16(header + 10), size, data);
        return AVERROR_INVALIDDATA;
    }
    return 0;
}

static int set_omt_packet_metadata(AVPacket *packet, const char *xml)
{
    AVDictionary *metadata = NULL;
    uint8_t *packed, *side_data;
    size_t packed_size;
    int ret;

    if ((ret = av_dict_set(&metadata, "omt.xml", xml, 0)) < 0)
        goto end;
    packed = av_packet_pack_dictionary(metadata, &packed_size);
    if (!packed) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    side_data = av_packet_new_side_data(packet, AV_PKT_DATA_STRINGS_METADATA,
                                        packed_size);
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

static void *omt_test_server(void *arg)
{
    OMTTestServer *server = arg;
    URLContext *client = NULL;
    uint8_t header[16], extended[32], message[128];
    uint16_t metadata_size = server->test->metadata ? strlen(server->test->metadata) + 1 : 0;
    int width = server->test->codec == MKTAG('V', 'M', 'X', '1') ? 32 : 2;
    int height = width;
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
    AV_WL32(extended + 4, width);
    AV_WL32(extended + 8, height);
    AV_WL32(extended + 12, 30);
    AV_WL32(extended + 16, 1);
    AV_WL32(extended + 20, av_float2int(1.0f));
    AV_WL32(extended + 24, server->test->flags);
    AV_WL32(extended + 28, 709);

    if (server->test->input_metadata) {
        memset(header, 0, sizeof(header));
        header[0] = 1;
        header[1] = 1;
        AV_WL32(header + 12, strlen(server->test->input_metadata) + 1);
        ret = write_all(client, header, sizeof(header));
        if (ret >= 0)
            ret = write_all(client, (const uint8_t *)server->test->input_metadata,
                            strlen(server->test->input_metadata) + 1);
        if (ret < 0)
            goto end;
    }

    memset(header, 0, sizeof(header));
    header[0] = 1;
    header[1] = 2;
    AV_WL64(header + 2, 123);
    AV_WL16(header + 10, metadata_size);
    AV_WL32(header + 12, sizeof(extended) + server->test->wire_size + metadata_size);
    ret = write_all(client, header, sizeof(header));
    if (ret >= 0)
        ret = write_all(client, extended, sizeof(extended));
    if (ret >= 0)
        ret = write_all(client, server->test->wire_data, server->test->wire_size);
    if (ret >= 0 && metadata_size)
        ret = write_all(client, (const uint8_t *)server->test->metadata, metadata_size);

end:
    ffurl_closep(&client);
    server->result = ret;
    return NULL;
}

static void *omt_audio_test_server(void *arg)
{
    OMTAudioTestServer *server = arg;
    URLContext *client = NULL;
    uint8_t header[16], extended[24], message[128];
    int ret;

    ret = ffurl_accept(server->listener, &client);
    if (ret < 0)
        goto end;
    for (int i = 0; i < 2; i++) {
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
    AV_WL32(extended, MKTAG('F', 'P', 'A', '1'));
    AV_WL32(extended + 4, 48000);
    AV_WL32(extended + 8, 2);
    AV_WL32(extended + 12, 2);
    AV_WL32(extended + 16, 3);
    memset(header, 0, sizeof(header));
    header[0] = 1;
    header[1] = 4;
    AV_WL64(header + 2, 123);
    AV_WL32(header + 12, sizeof(extended) + server->wire_size);
    ret = write_all(client, header, sizeof(header));
    if (ret >= 0)
        ret = write_all(client, extended, sizeof(extended));
    if (ret >= 0)
        ret = write_all(client, server->wire_data, server->wire_size);

end:
    ffurl_closep(&client);
    server->result = ret;
    return NULL;
}

static void *omt_timeout_test_server(void *arg)
{
    OMTTimeoutTestServer *server = arg;
    URLContext *client = NULL;
    uint8_t header[16], message[128], byte;
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
    ret = ffurl_read(client, &byte, 1);

end:
    ffurl_closep(&client);
    server->result = ret;
    return NULL;
}

static void *omt_discovery_test_server(void *arg)
{
    static const char subscribe[] = "<OMTSubscribe Metadata=\"true\" />";
    OMTDiscoveryTestServer *server = arg;
    URLContext *client = NULL;
    uint8_t header[16], message[512], byte;
    uint32_t size;
    int ret;

    ret = ffurl_accept(server->listener, &client);
    if (ret < 0)
        goto end;
    for (int i = 0; i < 2; i++) {
        ret = ffurl_read_complete(client, header, sizeof(header));
        if (ret < 0 || ret != sizeof(header) || header[0] != 1 || header[1] != 1 ||
            AV_RL16(header + 10)) {
            ret = ret < 0 ? ret : AVERROR_INVALIDDATA;
            goto end;
        }
        size = AV_RL32(header + 12);
        if (size < 2 || size > sizeof(message)) {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }
        ret = ffurl_read_complete(client, message, size);
        if (ret < 0 || ret != size) {
            ret = ret < 0 ? ret : AVERROR_EOF;
            goto end;
        }
        if (message[size - 1]) {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }
        if (!i) {
            if (size != sizeof(subscribe) || memcmp(message, subscribe, sizeof(subscribe))) {
                ret = AVERROR_INVALIDDATA;
                goto end;
            }
        } else {
            memcpy(server->xml, message, size);
        }
    }
    ret = ffurl_read(client, &byte, 1);
    server->disconnected = ret <= 0;

end:
    ffurl_closep(&client);
    server->result = ret;
    return NULL;
}

static int omt_test_discovery_register(void)
{
    OMTDiscoveryTestServer server = { 0 };
    AVFormatContext *format = NULL;
    AVDictionary *options = NULL;
    AVStream *stream;
    pthread_t thread;
    char listener_url[128], output_url[128], discovery_url[128];
    int discovery_port = -1, media_port = -1;
    int ret = AVERROR(EADDRINUSE), thread_started = 0, header_written = 0;

    for (int i = 0; i < 128; i++) {
        discovery_port = 49152 + (av_get_random_seed() + i * 6151) % (65536 - 49152);
        snprintf(listener_url, sizeof(listener_url),
                 "tcp://127.0.0.1:%d?listen=2&listen_timeout=5000&timeout=5000000",
                 discovery_port);
        ret = ffurl_open_whitelist(&server.listener, listener_url, AVIO_FLAG_READ_WRITE,
                                   NULL, NULL, NULL, NULL, NULL);
        if (ret >= 0)
            break;
    }
    if (ret < 0)
        goto end;
    ret = pthread_create(&thread, NULL, omt_discovery_test_server, &server);
    if (ret) {
        ret = AVERROR(ret);
        goto end;
    }
    thread_started = 1;

    for (int i = 0; i < 128; i++) {
        media_port = 49152 + (av_get_random_seed() + i * 4421) % (65536 - 49152);
        snprintf(output_url, sizeof(output_url), "omt://127.0.0.1:%d", media_port);
        ret = avformat_alloc_output_context2(&format,
                                             av_guess_format("omt", NULL, NULL),
                                             NULL, output_url);
        if (ret < 0)
            goto end;
        stream = avformat_new_stream(format, NULL);
        if (!stream) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        stream->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
        stream->codecpar->format = AV_PIX_FMT_UYVY422;
        stream->codecpar->width = 2;
        stream->codecpar->height = 2;
        stream->time_base = (AVRational){ 1, 30 };
        stream->r_frame_rate = stream->avg_frame_rate = (AVRational){ 30, 1 };
        stream->sample_aspect_ratio = (AVRational){ 1, 1 };
        snprintf(discovery_url, sizeof(discovery_url), "omt://127.0.0.1:%d", discovery_port);
        ret = av_dict_set(&options, "discovery_server", discovery_url, 0);
        if (ret >= 0)
            ret = av_dict_set(&options, "source_name", "Test & Source", 0);
        if (ret < 0)
            goto end;
        ret = avformat_write_header(format, &options);
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
    ret = av_write_trailer(format);
    header_written = 0;
    if (ret < 0)
        goto end;
    avformat_free_context(format);
    format = NULL;
    pthread_join(thread, NULL);
    thread_started = 0;
    if ((server.result < 0 && server.result != AVERROR_EOF) || !server.disconnected ||
        !strstr(server.xml, "<OMTAddress>") ||
        !strstr(server.xml, "Test &amp; Source") ||
        !strstr(server.xml, "<Port>") ||
        !strstr(server.xml, "<IPAddress>0.0.0.0</IPAddress>")) {
        av_log(NULL, AV_LOG_ERROR,
               "OMT discovery response result=%d disconnected=%d xml=%s\n",
               server.result, server.disconnected, server.xml);
        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    char port_text[32];
    snprintf(port_text, sizeof(port_text), "<Port>%d</Port>", media_port);
    ret = strstr(server.xml, port_text) ? 0 : AVERROR_INVALIDDATA;

end:
    av_dict_free(&options);
    if (format) {
        if (header_written)
            av_write_trailer(format);
        avformat_free_context(format);
    }
    ffurl_closep(&server.listener);
    if (thread_started)
        pthread_join(thread, NULL);
    if (ret < 0)
        av_log(NULL, AV_LOG_ERROR, "OMT discovery registration test failed: %s\n",
               av_err2str(ret));
    return ret;
}

static void *omt_discovery_resolver_test_server(void *arg)
{
    static const char subscribe[] = "<OMTSubscribe Metadata=\"true\" />";
    OMTDiscoveryResolverTestServer *server = arg;
    URLContext *client = NULL;
    uint8_t header[16];
    char xml[256];
    int ret;

    ret = ffurl_accept(server->listener, &client);
    if (ret < 0)
        goto end;
    ret = ffurl_read_complete(client, header, sizeof(header));
    if (ret < 0 || ret != sizeof(header) || header[0] != 1 || header[1] != 1 ||
        AV_RL16(header + 10) || AV_RL32(header + 12) != sizeof(subscribe)) {
        ret = ret < 0 ? ret : AVERROR_INVALIDDATA;
        goto end;
    }
    char message[sizeof(subscribe)];
    ret = ffurl_read_complete(client, (uint8_t *)message, sizeof(message));
    if (ret < 0 || ret != sizeof(message) || memcmp(message, subscribe, sizeof(subscribe))) {
        ret = ret < 0 ? ret : AVERROR_INVALIDDATA;
        goto end;
    }
    snprintf(xml, sizeof(xml),
             "<OMTAddress><Name>Test Host (Camera &amp; One)</Name><Port>%d</Port>"
             "<Addresses><IPAddress>127.0.0.1</IPAddress></Addresses></OMTAddress>",
             server->media_port);
    memset(header, 0, sizeof(header));
    header[0] = 1;
    header[1] = 1;
    AV_WL32(header + 12, strlen(xml) + 1);
    ret = write_all(client, header, sizeof(header));
    if (ret >= 0)
        ret = write_all(client, (const uint8_t *)xml, strlen(xml) + 1);

end:
    ffurl_closep(&client);
    server->result = ret;
    return NULL;
}

static int omt_test_discovery_resolve(void)
{
    static const uint8_t wire_data[] = { 128, 16, 128, 64, 129, 32, 129, 80 };
    OMTTestCase test = {
        MKTAG('U', 'Y', 'V', 'Y'), 0, AV_PIX_FMT_UYVY422,
        wire_data, sizeof(wire_data), wire_data, sizeof(wire_data),
    };
    OMTTestServer media_server = { .test = &test };
    OMTDiscoveryResolverTestServer registry_server = { 0 };
    AVFormatContext *format = NULL;
    AVPacket *packet = NULL;
    AVDictionary *options = NULL;
    pthread_t media_thread, registry_thread;
    int media_thread_started = 0, registry_thread_started = 0;
    int registry_port = -1, media_port = -1, ret = AVERROR(EADDRINUSE);
    char listener_url[128], registry_url[128];

    for (int i = 0; i < 128; i++) {
        media_port = 49152 + (av_get_random_seed() + i * 6151) % (65536 - 49152);
        snprintf(listener_url, sizeof(listener_url),
                 "tcp://127.0.0.1:%d?listen=2&listen_timeout=5000&timeout=5000000",
                 media_port);
        ret = ffurl_open_whitelist(&media_server.listener, listener_url,
                                   AVIO_FLAG_READ_WRITE, NULL, NULL, NULL, NULL, NULL);
        if (ret >= 0)
            break;
    }
    if (ret < 0)
        goto end;
    registry_server.media_port = media_port;
    for (int i = 0; i < 128; i++) {
        registry_port = 49152 + (av_get_random_seed() + i * 4421) % (65536 - 49152);
        if (registry_port == media_port)
            continue;
        snprintf(listener_url, sizeof(listener_url),
                 "tcp://127.0.0.1:%d?listen=2&listen_timeout=5000&timeout=5000000",
                 registry_port);
        ret = ffurl_open_whitelist(&registry_server.listener, listener_url,
                                   AVIO_FLAG_READ_WRITE, NULL, NULL, NULL, NULL, NULL);
        if (ret >= 0)
            break;
    }
    if (ret < 0)
        goto end;
    ret = pthread_create(&media_thread, NULL, omt_test_server, &media_server);
    if (ret) {
        ret = AVERROR(ret);
        goto end;
    }
    media_thread_started = 1;
    ret = pthread_create(&registry_thread, NULL, omt_discovery_resolver_test_server,
                         &registry_server);
    if (ret) {
        ret = AVERROR(ret);
        goto end;
    }
    registry_thread_started = 1;
    snprintf(registry_url, sizeof(registry_url), "omt://127.0.0.1:%d", registry_port);
    ret = av_dict_set(&options, "source_name", "Test Host (Camera & One)", 0);
    if (ret >= 0)
        ret = av_dict_set(&options, "discovery_server", registry_url, 0);
    if (ret >= 0)
        ret = av_dict_set_int(&options, "rw_timeout", 2000000, 0);
    if (ret < 0)
        goto end;
    ret = avformat_open_input(&format, registry_url, av_find_input_format("omt"), &options);
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
    if (packet->size != sizeof(wire_data) || memcmp(packet->data, wire_data, sizeof(wire_data)))
        ret = AVERROR_INVALIDDATA;

end:
    av_dict_free(&options);
    av_packet_free(&packet);
    avformat_close_input(&format);
    ffurl_closep(&registry_server.listener);
    ffurl_closep(&media_server.listener);
    if (registry_thread_started)
        pthread_join(registry_thread, NULL);
    if (media_thread_started)
        pthread_join(media_thread, NULL);
    if (ret >= 0 && (registry_server.result < 0 || media_server.result < 0))
        ret = registry_server.result < 0 ? registry_server.result : media_server.result;
    if (ret < 0)
        av_log(NULL, AV_LOG_ERROR, "OMT discovery resolve test failed: %s\n", av_err2str(ret));
    return ret;
}

static int omt_test_one(const OMTTestCase *test)
{
    OMTTestServer server = { .test = test };
    AVFormatContext *format = NULL;
    AVPacket *packet = NULL;
    pthread_t thread;
    char listen_url[128], input_url[128];
    enum AVCodecID codec_id = test->codec == MKTAG('V', 'M', 'X', '1') ?
                              AV_CODEC_ID_VMIX : AV_CODEC_ID_RAWVIDEO;
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
    if (test->input_metadata) {
        if (packet->stream_index < 0 || packet->stream_index >= format->nb_streams ||
            format->streams[packet->stream_index]->codecpar->codec_type != AVMEDIA_TYPE_DATA ||
            packet->size != strlen(test->input_metadata) + 1 ||
            memcmp(packet->data, test->input_metadata, packet->size - 1) ||
            packet->data[packet->size - 1]) {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }
        av_packet_unref(packet);
        ret = av_read_frame(format, packet);
        if (ret < 0)
            goto end;
    }
    int video_index = test->input_metadata ? 1 : 0;
    if (format->nb_streams != video_index + 1 ||
        format->streams[video_index]->codecpar->codec_id != codec_id ||
        format->streams[video_index]->codecpar->format != test->pixel_format ||
        packet->size != test->expected_size ||
        memcmp(packet->data, test->expected_data, test->expected_size))
        ret = AVERROR_INVALIDDATA;
    if (ret >= 0 && test->metadata) {
        AVDictionary *metadata = NULL;
        AVDictionaryEntry *entry;
        size_t side_data_size;
        uint8_t *side_data = av_packet_get_side_data(packet,
                                                     AV_PKT_DATA_STRINGS_METADATA,
                                                     &side_data_size);
        if (!side_data || av_packet_unpack_dictionary(side_data, side_data_size,
                                                       &metadata) < 0) {
            ret = AVERROR_INVALIDDATA;
        } else {
            entry = av_dict_get(metadata, "omt.xml", NULL, 0);
            if (!entry || strcmp(entry->value, test->metadata))
                ret = AVERROR_INVALIDDATA;
        }
        av_dict_free(&metadata);
    }

end:
    av_packet_free(&packet);
    avformat_close_input(&format);
    pthread_join(thread, NULL);
    ffurl_closep(&server.listener);
    if (ret >= 0 && server.result < 0)
        ret = server.result;
    return ret;
}

static int omt_test_audio_receive(const uint8_t *wire_data, int wire_size,
                                  const uint8_t *expected, int expected_size)
{
    OMTAudioTestServer server = { .wire_data = wire_data, .wire_size = wire_size };
    AVFormatContext *format = NULL;
    AVPacket *packet = NULL;
    AVDictionary *options = NULL;
    pthread_t thread;
    char listen_url[128], input_url[128];
    int port = -1, ret = AVERROR(EADDRINUSE);

    for (int i = 0; i < 128; i++) {
        port = 49152 + (av_get_random_seed() + i * 6151) % (65536 - 49152);
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
    ret = pthread_create(&thread, NULL, omt_audio_test_server, &server);
    if (ret) {
        ffurl_closep(&server.listener);
        return AVERROR(ret);
    }
    ret = av_dict_set(&options, "channel", "audio", 0);
    if (ret < 0)
        goto end;
    snprintf(input_url, sizeof(input_url), "omt://127.0.0.1:%d", port);
    ret = avformat_open_input(&format, input_url, av_find_input_format("omt"), &options);
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
        format->streams[0]->codecpar->codec_id != AV_CODEC_ID_PCM_F32LE ||
        format->streams[0]->codecpar->sample_rate != 48000 ||
        format->streams[0]->codecpar->ch_layout.nb_channels != 2 ||
        packet->size != expected_size || memcmp(packet->data, expected, expected_size))
        ret = AVERROR_INVALIDDATA;

end:
    av_dict_free(&options);
    av_packet_free(&packet);
    avformat_close_input(&format);
    pthread_join(thread, NULL);
    ffurl_closep(&server.listener);
    if (ret >= 0 && server.result < 0)
        ret = server.result;
    return ret;
}

static int interrupt_after_deadline(void *opaque)
{
    int64_t deadline = *(int64_t *)opaque;
    return av_gettime_relative() >= deadline;
}

static int omt_test_read_timeout(void)
{
    OMTTimeoutTestServer server = { 0 };
    AVFormatContext *format = avformat_alloc_context();
    AVDictionary *options = NULL;
    pthread_t thread;
    char listen_url[128], input_url[128];
    int64_t deadline = av_gettime_relative() + 3 * AV_TIME_BASE;
    int port = -1, ret = AVERROR(EADDRINUSE);

    if (!format)
        return AVERROR(ENOMEM);
    format->interrupt_callback.callback = interrupt_after_deadline;
    format->interrupt_callback.opaque = &deadline;
    for (int i = 0; i < 128; i++) {
        port = 49152 + (av_get_random_seed() + i * 6151) % (65536 - 49152);
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
        goto end;
    ret = pthread_create(&thread, NULL, omt_timeout_test_server, &server);
    if (ret) {
        ret = AVERROR(ret);
        goto end;
    }
    ret = av_dict_set_int(&options, "rw_timeout", 100000, 0);
    if (ret < 0)
        goto join;
    snprintf(input_url, sizeof(input_url), "omt://127.0.0.1:%d", port);
    ret = avformat_open_input(&format, input_url, av_find_input_format("omt"), &options);
    if (ret >= 0) {
        AVPacket *packet = av_packet_alloc();
        if (!packet)
            ret = AVERROR(ENOMEM);
        else {
            ret = av_read_frame(format, packet);
            av_packet_free(&packet);
            ret = ret == AVERROR(ETIMEDOUT) ? 0 : AVERROR_INVALIDDATA;
        }
    }
    avformat_close_input(&format);
join:
    av_dict_free(&options);
    pthread_join(thread, NULL);
    ffurl_closep(&server.listener);
    if (ret >= 0 && server.result != 0 && server.result != AVERROR_EOF)
        ret = server.result;
    return ret;

end:
    av_dict_free(&options);
    avformat_free_context(format);
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
    uint8_t header[16], extended[32], wire_data[1024], wire_metadata[512];
    int width = test->codec == MKTAG('V', 'M', 'X', '1') ? 32 : 2;
    int height = width;
    enum AVCodecID codec_id = test->codec == MKTAG('V', 'M', 'X', '1') ?
                              AV_CODEC_ID_VMIX : AV_CODEC_ID_RAWVIDEO;
    uint32_t size;
    uint16_t metadata_size = test->metadata ? strlen(test->metadata) + 1 : 0;
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
        stream->codecpar->codec_id = codec_id;
        stream->codecpar->format = test->pixel_format;
        stream->codecpar->width = width;
        stream->codecpar->height = height;
        stream->codecpar->bits_per_raw_sample =
            test->pixel_format == AV_PIX_FMT_YUVA422P10LE || codec_id == AV_CODEC_ID_VMIX ?
            10 : 8;
        stream->time_base = (AVRational){ 1, 30 };
        stream->r_frame_rate = (AVRational){ 30, 1 };
        stream->avg_frame_rate = (AVRational){ 30, 1 };
        stream->sample_aspect_ratio = (AVRational){ 1, 1 };
        if (test->product_name &&
            (ret = av_dict_set(&format->metadata, "product_name", test->product_name, 0)) < 0)
            goto end;
        if (test->manufacturer &&
            (ret = av_dict_set(&format->metadata, "manufacturer", test->manufacturer, 0)) < 0)
            goto end;
        if (test->version &&
            (ret = av_dict_set(&format->metadata, "version", test->version, 0)) < 0)
            goto end;
        if (test->connection_metadata &&
            (ret = av_dict_set(&options, "connection_metadata",
                               test->connection_metadata, 0)) < 0)
            goto end;
        stage = "write output header";
        ret = avformat_write_header(format, &options);
        av_dict_free(&options);
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
    AV_WL32(header + 12, sizeof(subscribe));
    ret = write_all(client, header, sizeof(header));
    if (ret >= 0)
        ret = write_all(client, (const uint8_t *)subscribe, sizeof(subscribe));
    if (ret < 0)
        goto end;
    av_usleep(100000);

    int sender_info = test->product_name || test->manufacturer || test->version;
    int initial_metadata_count = sender_info + !!test->connection_metadata;
    for (int i = 0; i < initial_metadata_count; i++) {
        stage = "read OMT sender information";
        ret = ffurl_read_complete(client, header, sizeof(header));
        if (ret < 0 || ret != sizeof(header)) {
            ret = ret < 0 ? ret : AVERROR_EOF;
            goto end;
        }
        size = AV_RL32(header + 12);
        if (header[0] != 1 || header[1] != 1 || AV_RL16(header + 10) ||
            size <= 1 || size >= sizeof(wire_data)) {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }
        ret = ffurl_read_complete(client, wire_data, size);
        if (ret < 0 || ret != size || wire_data[size - 1]) {
            ret = ret < 0 ? ret : AVERROR_INVALIDDATA;
            goto end;
        }
        if (sender_info && i == 0) {
            if (!strstr((const char *)wire_data, "<OMTInfo") ||
                (test->product_name &&
                 !strstr((const char *)wire_data, "FFmpeg &amp; OMT")) ||
                (test->manufacturer && !strstr((const char *)wire_data, test->manufacturer)) ||
                (test->version && !strstr((const char *)wire_data, test->version))) {
                ret = AVERROR_INVALIDDATA;
                goto end;
            }
        } else if (strcmp((const char *)wire_data, test->connection_metadata)) {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }
    }
    stage = "read initial OMT tally";
    ret = read_omt_metadata(client, "<OMTTally Preview=\"false\" Program==\"false\" />");
    if (ret < 0)
        goto end;
    static const char tally_preview[] =
        "<OMTTally Preview=\"true\" Program==\"false\" />";
    memset(header, 0, sizeof(header));
    header[0] = 1;
    header[1] = 1;
    AV_WL32(header + 12, sizeof(tally_preview));
    ret = write_all(client, header, sizeof(header));
    if (ret >= 0)
        ret = write_all(client, (const uint8_t *)tally_preview, sizeof(tally_preview));
    if (ret < 0)
        goto end;
    stage = "read aggregated preview tally";
    ret = read_omt_metadata(client, tally_preview);
    if (ret < 0)
        goto end;

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
    if (test->metadata && (ret = set_omt_packet_metadata(packet, test->metadata)) < 0)
        goto end;
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
        AV_RL16(header + 10) != metadata_size ||
        size != sizeof(extended) + test->wire_size + metadata_size) {
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
    if (AV_RL32(extended) != test->codec || AV_RL32(extended + 4) != width ||
        AV_RL32(extended + 8) != height || AV_RL32(extended + 24) != test->flags) {
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
    } else if (metadata_size) {
        stage = "read OMT frame metadata";
        ret = ffurl_read_complete(client, wire_metadata, metadata_size);
        if (ret < 0 || ret != metadata_size ||
            memcmp(wire_metadata, test->metadata, metadata_size))
            ret = ret < 0 ? ret : AVERROR_INVALIDDATA;
        else
            ret = 0;
    } else {
        ret = 0;
    }

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

static int omt_test_audio_mux(const uint8_t *interleaved, int interleaved_size,
                              const uint8_t *wire_data, int wire_size)
{
    AVFormatContext *format = NULL;
    AVPacket *packet = NULL;
    URLContext *client = NULL;
    AVDictionary *options = NULL;
    AVStream *stream;
    char output_url[128], tcp_url[128];
    uint8_t header[16], extended[24], actual[128];
    uint32_t size;
    int port = -1, ret = AVERROR(EADDRINUSE), header_written = 0;

    for (int i = 0; i < 128; i++) {
        port = 49152 + (av_get_random_seed() + i * 3571) % (65536 - 49152);
        snprintf(output_url, sizeof(output_url), "omt://127.0.0.1:%d", port);
        ret = avformat_alloc_output_context2(&format,
                                             av_guess_format("omt", NULL, NULL),
                                             NULL, output_url);
        if (ret < 0)
            return ret;
        stream = avformat_new_stream(format, NULL);
        if (!stream) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        stream->codecpar->codec_id = AV_CODEC_ID_PCM_F32LE;
        stream->codecpar->sample_rate = 48000;
        stream->codecpar->bits_per_coded_sample = 32;
        stream->codecpar->block_align = 8;
        stream->codecpar->bit_rate = 48000 * 2 * 32;
        stream->codecpar->ch_layout.nb_channels = 2;
        stream->time_base = (AVRational){ 1, 48000 };
        ret = avformat_write_header(format, NULL);
        if (ret >= 0) {
            header_written = 1;
            break;
        }
        avformat_free_context(format);
        format = NULL;
        if (ret != AVERROR(EADDRINUSE))
            return ret;
    }
    if (!header_written)
        goto end;

    snprintf(tcp_url, sizeof(tcp_url), "tcp://127.0.0.1:%d", port);
    ret = av_dict_set_int(&options, "rw_timeout", 2000000, 0);
    if (ret < 0)
        goto end;
    ret = ffurl_open_whitelist(&client, tcp_url, AVIO_FLAG_READ_WRITE, NULL,
                               &options, NULL, NULL, NULL);
    av_dict_free(&options);
    if (ret < 0)
        goto end;
    static const char subscribe[] = "<OMTSubscribe Audio=\"true\" />";
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

    ret = read_omt_metadata(client, "<OMTTally Preview=\"false\" Program==\"false\" />");
    if (ret < 0)
        goto end;

    packet = av_packet_alloc();
    if (!packet) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    if ((ret = av_new_packet(packet, interleaved_size)) < 0)
        goto end;
    memcpy(packet->data, interleaved, interleaved_size);
    packet->stream_index = 0;
    packet->pts = packet->dts = 4800;
    packet->duration = 2;
    if ((ret = av_write_frame(format, packet)) < 0)
        goto end;

    ret = ffurl_read_complete(client, header, sizeof(header));
    if (ret < 0 || ret != sizeof(header)) {
        ret = ret < 0 ? ret : AVERROR_EOF;
        goto end;
    }
    size = AV_RL32(header + 12);
    if (header[0] != 1 || header[1] != 4 || AV_RL64(header + 2) != 1000000 ||
        AV_RL16(header + 10) || size != sizeof(extended) + wire_size) {
        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    ret = ffurl_read_complete(client, extended, sizeof(extended));
    if (ret < 0 || ret != sizeof(extended)) {
        ret = ret < 0 ? ret : AVERROR_EOF;
        goto end;
    }
    if (AV_RL32(extended) != MKTAG('F', 'P', 'A', '1') ||
        AV_RL32(extended + 4) != 48000 || AV_RL32(extended + 8) != 2 ||
        AV_RL32(extended + 12) != 2 || AV_RL32(extended + 16) != 3) {
        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    ret = ffurl_read_complete(client, actual, wire_size);
    if (ret < 0 || ret != wire_size) {
        ret = ret < 0 ? ret : AVERROR_EOF;
        goto end;
    }
    ret = memcmp(actual, wire_data, wire_size) ? AVERROR_INVALIDDATA : 0;

end:
    av_dict_free(&options);
    av_packet_free(&packet);
    ffurl_closep(&client);
    if (format) {
        if (header_written)
            av_write_trailer(format);
        avformat_free_context(format);
    }
    return ret;
}

typedef struct OMTDrainContext {
    URLContext *client;
    int expected_frames;
    int received_frames;
    int result;
} OMTDrainContext;

static void *omt_drain_receiver(void *arg)
{
    OMTDrainContext *drain = arg;
    uint8_t header[16], buffer[65536];
    int ret = 0;

    while (drain->received_frames < drain->expected_frames) {
        uint32_t size;

        ret = ffurl_read_complete(drain->client, header, sizeof(header));
        if (ret < 0 || ret != sizeof(header)) {
            ret = ret < 0 ? ret : AVERROR_EOF;
            break;
        }
        if (header[0] != 1 || (header[1] != 1 && header[1] != 2)) {
            ret = AVERROR_INVALIDDATA;
            break;
        }
        size = AV_RL32(header + 12);
        while (size) {
            int chunk = FFMIN(size, sizeof(buffer));
            ret = ffurl_read_complete(drain->client, buffer, chunk);
            if (ret < 0 || ret != chunk) {
                ret = ret < 0 ? ret : AVERROR_EOF;
                break;
            }
            size -= chunk;
        }
        if (ret < 0)
            break;
        if (header[1] == 1)
            continue;
        drain->received_frames++;
    }
    drain->result = ret;
    return NULL;
}

static int omt_test_slow_receiver_isolation(void)
{
    enum { WIDTH = 512, HEIGHT = 512, FRAME_COUNT = 24 };
    AVFormatContext *format = NULL;
    AVPacket *packet = NULL;
    URLContext *slow = NULL, *fast = NULL;
    AVDictionary *options = NULL;
    OMTDrainContext drain = { .expected_frames = FRAME_COUNT };
    AVStream *stream;
    char output_url[128], tcp_url[128];
    static const char subscribe[] = "<OMTSubscribe Video=\"true\" />";
    uint8_t metadata[16] = { 1, 1 };
    int port = -1, ret = AVERROR(EADDRINUSE), header_written = 0;
    int64_t start_time, elapsed = 0;
    pthread_t drain_thread;
    int drain_thread_started = 0;

    for (int i = 0; i < 128; i++) {
        port = 49152 + (av_get_random_seed() + i * 4421) % (65536 - 49152);
        snprintf(output_url, sizeof(output_url), "omt://127.0.0.1:%d", port);
        ret = avformat_alloc_output_context2(&format,
                                             av_guess_format("omt", NULL, NULL),
                                             NULL, output_url);
        if (ret < 0)
            return ret;
        stream = avformat_new_stream(format, NULL);
        if (!stream) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        stream->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
        stream->codecpar->format = AV_PIX_FMT_UYVY422;
        stream->codecpar->width = WIDTH;
        stream->codecpar->height = HEIGHT;
        stream->time_base = (AVRational){ 1, 30 };
        stream->r_frame_rate = stream->avg_frame_rate = (AVRational){ 30, 1 };
        stream->sample_aspect_ratio = (AVRational){ 1, 1 };
        ret = avformat_write_header(format, NULL);
        if (ret >= 0) {
            header_written = 1;
            break;
        }
        avformat_free_context(format);
        format = NULL;
        if (ret != AVERROR(EADDRINUSE))
            return ret;
    }
    if (!header_written)
        goto end;

    snprintf(tcp_url, sizeof(tcp_url), "tcp://127.0.0.1:%d", port);
    ret = av_dict_set_int(&options, "rw_timeout", 2000000, 0);
    if (ret < 0)
        goto end;
    ret = ffurl_open_whitelist(&slow, tcp_url, AVIO_FLAG_READ_WRITE, NULL,
                               &options, NULL, NULL, NULL);
    av_dict_free(&options);
    if (ret < 0)
        goto end;
    ret = av_dict_set_int(&options, "rw_timeout", 2000000, 0);
    if (ret < 0)
        goto end;
    ret = ffurl_open_whitelist(&fast, tcp_url, AVIO_FLAG_READ_WRITE, NULL,
                               &options, NULL, NULL, NULL);
    av_dict_free(&options);
    if (ret < 0)
        goto end;

    AV_WL32(metadata + 12, sizeof(subscribe) - 1);
    ret = write_all(slow, metadata, sizeof(metadata));
    if (ret >= 0)
        ret = write_all(slow, (const uint8_t *)subscribe, sizeof(subscribe) - 1);
    if (ret >= 0)
        ret = write_all(fast, metadata, sizeof(metadata));
    if (ret >= 0)
        ret = write_all(fast, (const uint8_t *)subscribe, sizeof(subscribe) - 1);
    if (ret < 0)
        goto end;
    av_usleep(100000);

    drain.client = fast;
    ret = pthread_create(&drain_thread, NULL, omt_drain_receiver, &drain);
    if (ret) {
        ret = AVERROR(ret);
        goto end;
    }
    drain_thread_started = 1;

    packet = av_packet_alloc();
    if (!packet) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    ret = av_new_packet(packet, WIDTH * HEIGHT * 2);
    if (ret < 0)
        goto end;
    memset(packet->data, 0x80, packet->size);
    start_time = av_gettime_relative();
    for (int i = 0; i < FRAME_COUNT; i++) {
        packet->pts = packet->dts = i;
        packet->duration = 1;
        if ((ret = av_write_frame(format, packet)) < 0)
            goto end;
        av_usleep(5000);
    }
    elapsed = av_gettime_relative() - start_time;
    ret = av_write_trailer(format);
    header_written = 0;
    if (ret < 0)
        goto end;
    ret = 0;

end:
    av_dict_free(&options);
    av_packet_free(&packet);
    ffurl_closep(&slow);
    ffurl_closep(&fast);
    if (format) {
        if (header_written)
            av_write_trailer(format);
        avformat_free_context(format);
    }
    if (drain_thread_started)
        pthread_join(drain_thread, NULL);
    if (ret >= 0 && (drain.received_frames < FRAME_COUNT - 2 || elapsed > 2000000))
        ret = AVERROR_INVALIDDATA;
    if (ret < 0)
        av_log(NULL, AV_LOG_ERROR,
               "OMT slow-receiver test failed: frames=%d elapsed=%"PRId64" us\n",
               drain.received_frames, elapsed);
    return ret;
}

int main(void)
{
    static const char vmix_p216_packet_base64[] =
        "AwMBUAIWAAAAA7AdQOoCIBCBvAVgJgHuJFmAAfbYsBYAAAADuAuhQHUD0EwOoLwAgowgXACBjCB8LgEA"
        "AMcOtUoNxw61Sg3HDrVKDcE4JcbsJBJ0a0JEqQ8rIRymlbkLaVEt44dapQbihRDEZStERE223pJK223S"
        "SUjcJB8EpwuVihzKYYtptbIUXtabLaebVojt2yypY4dapQbAxCISkKIRaPrQwUzl0t2zi9Ciq1RSKm9N"
        "UpRb02QTreJ3kFnOL1tNp44UliG06TVESelbVictHIelshCpDRKImiap4SEaTpkaWR02t26VLOyCcyPk"
        "SI9mMQYISWm3iS7GkS0227b2rUjFEyZEbJshWRocOy7meUbszS1SZi09yNSvNSRchWY7IsaJOPKlT2mK"
        "EUUzellptJrT3SsTko5D09FIVIaJZWmXVPCQjbVIrSyOknu1Sou8hSrOQhGZpmSCAqRKxJXi0qbpFTTL"
        "apaoQgEAANEEDhBRIkqMYi2UrkUY23aSVNukogMgjFh/YgixAzLtEkKdLWsS9tW6aIqSdvek6HDrVKDc"
        "cOtUoNwuGxYIyk2TECCWQQntb1TKV6WVyqktKkpG3jh1qlBuOHWqUG4TCIoEArOemV0jGVlO4xJSFLIp"
        "m2lgzEKyLFJvKZrIMGZluRNnFrORvetm3rW9bnIcpGyitlMyIrKcKnJrdNFJtES3tIpGt7txZsTiGzFZ"
        "GQWJEhMpG7aRDaKa1Rdq3pUogpiEbKL0Q7IispgoYu900UmmVvW0ikT1u1KAkZHkRURG22MEYoyZG3RR"
        "Il4slpZWSiZJE6cjMytpCGUpt7o4TEt6aWhK0I3pJIh7WqdJyEYgpRBS4or01s5xIVelrS2ISZktuk7e"
        "6Vp2RiSEEFLiyLbWznEBRbetLYhNHb06TtapW3Q=";
    static const uint8_t uyvy[] = {
        128, 16, 128, 64, 129, 32, 129, 80,
    };
    static const char frame_metadata[] = "<OMTInfo ProductName=\"api-test\" />";
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
        static const uint8_t audio_planar[] = {
                0, 0, 128, 62, 0, 0, 0, 63,
                0, 0, 64, 63, 0, 0, 128, 63,
        };
        static const uint8_t audio_interleaved[] = {
                0, 0, 128, 62, 0, 0, 64, 63,
                0, 0, 0, 63, 0, 0, 128, 63,
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
    uint8_t vmix_p216_packet[1024];
    int vmix_p216_size;
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
    if (ret >= 0)
        ret = omt_test_audio_receive(audio_planar, sizeof(audio_planar),
                                     audio_interleaved, sizeof(audio_interleaved));
    if (ret >= 0)
        ret = omt_test_audio_mux(audio_interleaved, sizeof(audio_interleaved),
                                 audio_planar, sizeof(audio_planar));
    if (ret < 0)
        av_log(NULL, AV_LOG_ERROR, "OMT API audio test failed: %s\n", av_err2str(ret));
    if (ret >= 0)
        ret = omt_test_slow_receiver_isolation();
    if (ret >= 0)
        ret = omt_test_read_timeout();
    if (ret >= 0)
        ret = omt_test_discovery_register();
    if (ret >= 0)
        ret = omt_test_discovery_resolve();
    if (ret >= 0) {
        OMTTestCase metadata_test = {
            .codec = MKTAG('U', 'Y', 'V', 'Y'),
            .pixel_format = AV_PIX_FMT_UYVY422,
            .wire_data = uyvy,
            .wire_size = sizeof(uyvy),
            .expected_data = uyvy,
            .expected_size = sizeof(uyvy),
            .metadata = frame_metadata,
            .product_name = "FFmpeg & OMT",
            .manufacturer = "Open Media",
            .version = "1.0",
            .connection_metadata = "<OMTCustom Name=\"connection\" />",
            .input_metadata = "<OMTInfo ProductName=\"Mock Sender\" />",
        };
        ret = omt_test_one(&metadata_test);
        if (ret >= 0)
            ret = omt_test_mux_one(&metadata_test);
    }
    if (ret >= 0)
        ret = ff_omt_dns_sd_test();
    vmix_p216_size = av_base64_decode(vmix_p216_packet, vmix_p216_packet_base64,
                                      sizeof(vmix_p216_packet));
    if (ret >= 0 && vmix_p216_size != 689)
        ret = AVERROR_INVALIDDATA;
    if (ret >= 0) {
        OMTTestCase vmix_test = {
            MKTAG('V', 'M', 'X', '1'), 16, AV_PIX_FMT_YUV422P10LE,
            vmix_p216_packet, vmix_p216_size,
            vmix_p216_packet, vmix_p216_size,
        };
        ret = omt_test_one(&vmix_test);
        if (ret >= 0)
            ret = omt_test_mux_one(&vmix_test);
        if (ret < 0)
            av_log(NULL, AV_LOG_ERROR, "OMT API 10-bit VMX1 test failed: %s\n",
                   av_err2str(ret));
    }
    avformat_network_deinit();
    return ret < 0;
}