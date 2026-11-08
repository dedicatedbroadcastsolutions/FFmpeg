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

#include <string.h>

#include "libavutil/base64.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "libavcodec/avcodec.h"

static const uint8_t vmix_packet[] = {
    0x03, 0x03, 0x01, 0x50, 0x01, 0x0b, 0x00, 0x00,
    0x00, 0x02, 0x60, 0x18, 0x47, 0x8e, 0x01, 0xe6,
    0x11, 0x00, 0x80, 0x90, 0x51, 0x00, 0x00, 0x00,
    0xc7, 0x04, 0x2d, 0x52, 0x95, 0x46, 0x1a, 0x35,
    0xc2, 0xc3, 0x82, 0x16, 0x4a, 0x52, 0x92, 0x30,
    0x44, 0x6b, 0x85, 0x89, 0x88, 0x1c, 0x93, 0x2b,
    0xa6, 0x92, 0x79, 0x15, 0x32, 0xbd, 0x10, 0xc5,
    0xb5, 0x22, 0xda, 0x21, 0x74, 0xde, 0x93, 0xd6,
    0xb7, 0x39, 0x71, 0x22, 0x51, 0x48, 0xde, 0x89,
    0xa2, 0xad, 0x2a, 0x49, 0xe8, 0x82, 0xc9, 0x4e,
    0x46, 0xf6, 0x8c, 0x46, 0xd3, 0xa5, 0xb5, 0xb4,
    0x94, 0xa0, 0xc5, 0x28, 0x3e, 0x14, 0xa0, 0xf4,
    0xd1, 0x12, 0x5b, 0x70, 0x72, 0x99, 0x6d, 0xc1,
    0xb8,
};

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

static const uint8_t reference_uyvy[] = {
    0x64, 0x18, 0x75, 0x33, 0x67, 0x27, 0x74, 0x3e, 0x6c, 0x36, 0x73, 0x4e,
    0x72, 0x42, 0x73, 0x5c, 0x79, 0x3f, 0x73, 0x6e, 0x7f, 0x54, 0x75, 0x73,
    0x84, 0x61, 0x78, 0x80, 0x87, 0x67, 0x7a, 0x95, 0x65, 0x21, 0x75, 0x3d,
    0x67, 0x2f, 0x75, 0x49, 0x6c, 0x3e, 0x75, 0x58, 0x73, 0x4a, 0x76, 0x66,
    0x7a, 0x48, 0x78, 0x78, 0x80, 0x5c, 0x7b, 0x7e, 0x85, 0x69, 0x7d, 0x8b,
    0x87, 0x6f, 0x7f, 0x9e, 0x66, 0x2c, 0x74, 0x4a, 0x68, 0x38, 0x76, 0x58,
    0x6d, 0x46, 0x79, 0x67, 0x74, 0x54, 0x7c, 0x72, 0x7a, 0x53, 0x80, 0x85,
    0x81, 0x65, 0x83, 0x8d, 0x86, 0x71, 0x85, 0x99, 0x88, 0x79, 0x86, 0xab,
    0x67, 0x34, 0x75, 0x55, 0x6a, 0x3c, 0x78, 0x64, 0x6e, 0x4a, 0x7e, 0x72,
    0x75, 0x5a, 0x83, 0x7b, 0x7c, 0x5b, 0x87, 0x90, 0x82, 0x6a, 0x8a, 0x99,
    0x87, 0x75, 0x8a, 0xa5, 0x8a, 0x7f, 0x8a, 0xb4, 0x68, 0x38, 0x78, 0x5d,
    0x6b, 0x3e, 0x7c, 0x6e, 0x70, 0x4b, 0x83, 0x7c, 0x76, 0x5d, 0x89, 0x81,
    0x7d, 0x5f, 0x8c, 0x98, 0x84, 0x6b, 0x8c, 0xa3, 0x88, 0x77, 0x8a, 0xae,
    0x8b, 0x82, 0x89, 0xba, 0x6a, 0x3f, 0x7b, 0x67, 0x6c, 0x43, 0x80, 0x7a,
    0x71, 0x4f, 0x87, 0x87, 0x77, 0x62, 0x8c, 0x8a, 0x7e, 0x67, 0x8e, 0xa2,
    0x85, 0x70, 0x8a, 0xaf, 0x8a, 0x7b, 0x85, 0xba, 0x8c, 0x88, 0x81, 0xc3,
    0x6b, 0x4b, 0x7e, 0x75, 0x6d, 0x4c, 0x83, 0x89, 0x72, 0x58, 0x8a, 0x95,
    0x79, 0x6c, 0x8e, 0x97, 0x7f, 0x72, 0x8d, 0xb0, 0x86, 0x79, 0x86, 0xbe,
    0x8b, 0x83, 0x7e, 0xc8, 0x8d, 0x91, 0x78, 0xcf, 0x6b, 0x54, 0x80, 0x7f,
    0x6e, 0x54, 0x85, 0x94, 0x73, 0x60, 0x8b, 0xa0, 0x79, 0x75, 0x8f, 0xa0,
    0x80, 0x7b, 0x8c, 0xba, 0x86, 0x81, 0x83, 0xc9, 0x8b, 0x8b, 0x79, 0xd3,
    0x8e, 0x9a, 0x72, 0xd9, 0x6c, 0x59, 0x8e, 0x8e, 0x6f, 0x51, 0x8c, 0xa6,
    0x74, 0x71, 0x87, 0x9b, 0x7a, 0x89, 0x81, 0xb9, 0x81, 0x88, 0x7b, 0xbe,
    0x87, 0x85, 0x75, 0xcd, 0x8c, 0xa1, 0x71, 0xde, 0x8f, 0xa3, 0x6f, 0x31,
    0x6d, 0x59, 0x87, 0x8c, 0x6f, 0x71, 0x86, 0x85, 0x74, 0x71, 0x83, 0xc4,
    0x7b, 0x77, 0x7f, 0xc6, 0x82, 0x94, 0x7b, 0xc7, 0x88, 0x82, 0x77, 0xe6,
    0x8d, 0xa9, 0x74, 0x27, 0x8f, 0x9f, 0x72, 0x4b, 0x6e, 0x5e, 0x7d, 0x9e,
    0x70, 0x7b, 0x7d, 0xb1, 0x75, 0x79, 0x7c, 0xaa, 0x7c, 0x85, 0x7b, 0xc7,
    0x82, 0x78, 0x7a, 0xe7, 0x89, 0x90, 0x79, 0x1f, 0x8e, 0xaa, 0x78, 0x4e,
    0x90, 0x9c, 0x78, 0x55, 0x6f, 0x68, 0x75, 0xbc, 0x72, 0x6e, 0x76, 0xbe,
    0x76, 0x7b, 0x77, 0xbe, 0x7d, 0x9d, 0x79, 0xca, 0x84, 0xa7, 0x7a, 0x27,
    0x8a, 0xa3, 0x7c, 0x4c, 0x8f, 0x9b, 0x7d, 0x56, 0x92, 0xba, 0x7e, 0x58,
    0x70, 0x74, 0x73, 0xb2, 0x73, 0x75, 0x74, 0xc4, 0x78, 0x86, 0x76, 0xda,
    0x7e, 0x85, 0x79, 0xd5, 0x85, 0x90, 0x7d, 0x46, 0x8c, 0xad, 0x7f, 0x46,
    0x90, 0xac, 0x82, 0x5b, 0x93, 0xb3, 0x83, 0x65, 0x72, 0x7f, 0x76, 0xbe,
    0x74, 0x86, 0x78, 0xc9, 0x79, 0x8f, 0x7a, 0xe0, 0x7f, 0xb1, 0x7d, 0x35,
    0x86, 0xb9, 0x80, 0x50, 0x8d, 0x9e, 0x83, 0x4b, 0x92, 0xa9, 0x85, 0x69,
    0x94, 0xc6, 0x86, 0x79, 0x73, 0x8b, 0x7d, 0xaf, 0x75, 0x93, 0x7e, 0xec,
    0x7a, 0xa1, 0x7f, 0x29, 0x81, 0x94, 0x81, 0x45, 0x87, 0x9b, 0x84, 0x53,
    0x8e, 0xc5, 0x86, 0x60, 0x93, 0xb8, 0x87, 0x79, 0x95, 0xc7, 0x88, 0x7f,
    0x73, 0x94, 0x82, 0xd5, 0x76, 0x86, 0x82, 0x30, 0x7b, 0x8e, 0x83, 0x46,
    0x81, 0xa3, 0x85, 0x57, 0x88, 0xaf, 0x86, 0x65, 0x8e, 0xb4, 0x87, 0x6f,
    0x93, 0xd1, 0x88, 0x5a, 0x96, 0xd0, 0x89, 0x7a,
};

static int open_decoder(AVCodecContext **decoder, int width, int height, int bit_depth)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_VMIX);

    if (!codec)
        return AVERROR_DECODER_NOT_FOUND;
    *decoder = avcodec_alloc_context3(codec);
    if (!*decoder)
        return AVERROR(ENOMEM);
    (*decoder)->width  = width;
    (*decoder)->height = height;
    (*decoder)->bits_per_raw_sample = bit_depth;
    return avcodec_open2(*decoder, codec, NULL);
}

static uint32_t hash_10bit_frame(const AVFrame *frame)
{
    uint32_t hash = 2166136261U;

    for (int plane = 0; plane < 3; plane++) {
        int width = plane ? frame->width / 2 : frame->width;
        for (int y = 0; y < frame->height; y++) {
            const uint8_t *row = frame->data[plane] + y * frame->linesize[plane];
            for (int x = 0; x < width; x++) {
                uint16_t sample = AV_RL16(row + x * 2);
                hash = (hash ^ (sample & 0xff)) * 16777619U;
                hash = (hash ^ (sample >> 8)) * 16777619U;
            }
        }
    }
    return hash;
}

static int test_p216(void)
{
    uint8_t packet_data[1024];
    AVCodecContext *decoder = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    int packet_size, ret;

    packet_size = av_base64_decode(packet_data, vmix_p216_packet_base64,
                                   sizeof(packet_data));
    if (packet_size != 689)
        return AVERROR_INVALIDDATA;
    ret = open_decoder(&decoder, 32, 32, 10);
    if (ret < 0)
        goto end;
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (!packet || !frame) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    if ((ret = av_new_packet(packet, packet_size)) < 0)
        goto end;
    memcpy(packet->data, packet_data, packet_size);
    if ((ret = avcodec_send_packet(decoder, packet)) < 0 ||
        (ret = avcodec_receive_frame(decoder, frame)) < 0)
        goto end;
    if (frame->width != 32 || frame->height != 32 ||
        frame->format != AV_PIX_FMT_YUV422P10LE ||
        hash_10bit_frame(frame) != 0x55b29440) {
        av_log(NULL, AV_LOG_ERROR,
               "VMIX P216 output changed (format=%d hash=%08x)\n",
               frame->format, hash_10bit_frame(frame));
        ret = AVERROR_INVALIDDATA;
    }

end:
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    return ret;
}

static int test_encode_roundtrip(const char *profile, int quality,
                                 uint8_t expected_dc_shift, int expected_quality)
{
    const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_VMIX);
    AVCodecContext *encoder_ctx = NULL, *decoder_ctx = NULL;
    AVFrame *source = NULL, *decoded = NULL;
    AVPacket *packet = NULL;
    AVDictionary *options = NULL;
    int ret;

    if (!encoder)
        return AVERROR_ENCODER_NOT_FOUND;
    encoder_ctx = avcodec_alloc_context3(encoder);
    source = av_frame_alloc();
    decoded = av_frame_alloc();
    packet = av_packet_alloc();
    if (!encoder_ctx || !source || !decoded || !packet) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    encoder_ctx->width = 16;
    encoder_ctx->height = 16;
    encoder_ctx->pix_fmt = AV_PIX_FMT_YUV422P;
    encoder_ctx->time_base = (AVRational){ 1, 25 };
    source->format = AV_PIX_FMT_YUV422P;
    source->width = 16;
    source->height = 16;
    if (profile && (ret = av_dict_set(&options, "omt_profile", profile, 0)) < 0)
        goto end;
    if ((ret = av_dict_set_int(&options, "omt_quality", quality, 0)) < 0 ||
        (ret = avcodec_open2(encoder_ctx, encoder, &options)) < 0 ||
        (ret = av_frame_get_buffer(source, 32)) < 0 ||
        (ret = open_decoder(&decoder_ctx, 16, 16, 8)) < 0)
        goto end;
    av_dict_free(&options);
    for (int pattern = 0; pattern < 2; pattern++) {
        int max_error = 0;
        int error_plane = 0, error_x = 0, error_y = 0;
        int source_sample = 0, decoded_sample = 0;

        av_frame_unref(decoded);
        av_packet_unref(packet);
        if ((ret = av_frame_make_writable(source)) < 0)
            goto end;
        source->pts = pattern;
        for (int plane = 0; plane < 3; plane++) {
            int width = plane ? 8 : 16;
            for (int y = 0; y < 16; y++) {
                uint8_t *row = source->data[plane] + y * source->linesize[plane];
                for (int x = 0; x < width; x++)
                    row[x] = pattern ?
                             (plane ? 96 + (x * 3 + y * 5 + plane * 17) % 64 :
                                      16 + (x * 7 + y * 5) % 220) :
                             (plane ? 128 : 80);
            }
        }
        if ((ret = avcodec_send_frame(encoder_ctx, source)) < 0 ||
            (ret = avcodec_receive_packet(encoder_ctx, packet)) < 0 ||
            (ret = avcodec_send_packet(decoder_ctx, packet)) < 0 ||
            (ret = avcodec_receive_frame(decoder_ctx, decoded)) < 0)
            goto end;
        if (packet->size <= 5 || packet->data[0] != 3 ||
            packet->data[1] != expected_dc_shift ||
            packet->data[3] != expected_quality) {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }
        if (decoded->format != AV_PIX_FMT_YUV422P) {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }
        for (int plane = 0; plane < 3; plane++) {
            int width = plane ? 8 : 16;
            for (int y = 0; y < 16; y++) {
                const uint8_t *src = source->data[plane] + y * source->linesize[plane];
                const uint8_t *dst = decoded->data[plane] + y * decoded->linesize[plane];
                for (int x = 0; x < width; x++) {
                    int error = FFABS(src[x] - dst[x]);
                    if (error > max_error) {
                        max_error = error;
                        error_plane = plane;
                        error_x = x;
                        error_y = y;
                        source_sample = src[x];
                        decoded_sample = dst[x];
                    }
                }
            }
        }
        if (max_error > (quality < 80 ? 128 : 64)) {
            av_log(NULL, AV_LOG_ERROR,
                   "VMIX encoder roundtrip %d max error %d at plane %d (%d,%d): %d vs %d\n",
                   pattern, max_error, error_plane, error_x, error_y,
                   source_sample, decoded_sample);
            ret = AVERROR_INVALIDDATA;
            goto end;
        }
    }

end:
    av_dict_free(&options);
    av_packet_free(&packet);
    av_frame_free(&decoded);
    av_frame_free(&source);
    avcodec_free_context(&decoder_ctx);
    avcodec_free_context(&encoder_ctx);
    return ret;
}

int main(void)
{
    AVCodecContext *decoder = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    uint8_t image[sizeof(reference_uyvy)];
    int ret;
    uint64_t squared_error = 0;
    int max_error = 0;

    if ((ret = open_decoder(&decoder, 16, 16, 8)) < 0)
        goto end;
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (!packet || !frame) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    if ((ret = av_new_packet(packet, sizeof(vmix_packet))) < 0)
        goto end;
    memcpy(packet->data, vmix_packet, sizeof(vmix_packet));
    if ((ret = avcodec_send_packet(decoder, packet)) < 0 ||
        (ret = avcodec_receive_frame(decoder, frame)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "VMIX reference packet failed to decode: %s\n",
               av_err2str(ret));
        goto end;
    }

    if (frame->width != 16 || frame->height != 16 ||
        frame->format != AV_PIX_FMT_YUV422P) {
        av_log(NULL, AV_LOG_ERROR, "Unexpected VMIX output: %dx%d format %d\n",
               frame->width, frame->height, frame->format);
        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x += 2) {
            uint8_t *dst = image + (y * 16 + x) * 2;
            dst[0] = frame->data[1][y * frame->linesize[1] + x / 2];
            dst[1] = frame->data[0][y * frame->linesize[0] + x];
            dst[2] = frame->data[2][y * frame->linesize[2] + x / 2];
            dst[3] = frame->data[0][y * frame->linesize[0] + x + 1];
        }
    }
    for (size_t i = 0; i < sizeof(image); i++) {
        int error = image[i] - reference_uyvy[i];
        error = FFABS(error);
        max_error = FFMAX(max_error, error);
        squared_error += error * error;
    }
    if (max_error > 1 || squared_error > 14) {
        av_log(NULL, AV_LOG_ERROR,
               "VMIX reference mismatch: max error %d, squared error %" PRIu64 "\n",
               max_error, squared_error);
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    avcodec_free_context(&decoder);
    if ((ret = open_decoder(&decoder, 16, 16, 8)) < 0)
        goto end;
    packet->data[0] = 0;
    ret = avcodec_send_packet(decoder, packet);
    if (ret >= 0)
        ret = avcodec_receive_frame(decoder, frame);
    if (ret >= 0)
        ret = AVERROR_INVALIDDATA;
    else
        ret = 0;
    if (ret >= 0)
        ret = test_p216();
    if (ret >= 0)
        ret = test_encode_roundtrip(NULL, 80, 3, 80);
    if (ret >= 0)
        ret = test_encode_roundtrip("high", 60, 0, 60);

end:
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    return ret < 0;
}
