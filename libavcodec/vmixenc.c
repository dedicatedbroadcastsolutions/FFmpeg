/*
 * vMix encoder
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

#include <limits.h>
#include <string.h>

#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "libavutil/mem_internal.h"
#include "libavutil/opt.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "encode.h"
#include "fdctdsp.h"
#include "idctdsp.h"
#include "mathops.h"
#include "put_bits.h"
#include "put_golomb.h"

#define VMIX_QUALITY_DEFAULT 80
/* FFmpeg's FDCT output is scaled by 8 relative to the VMX encoder. */
#define VMIX_FDCT_SCALE 8
#define VMIX_MAX_PACKET_SIZE (256 * 1024 * 1024)

typedef struct VMIXEncodeSlice {
    uint8_t *dc;
    uint8_t *ac;
    int dc_size;
    int ac_size;
} VMIXEncodeSlice;

typedef struct VMIXEncContext {
    const AVClass *class;
    FDCTDSPContext fdsp;
    int profile;
    int quality;
    int quality_scale;
    int dc_shift;
} VMIXEncContext;

enum VMIXProfile {
        VMIX_PROFILE_OMT_LQ,
        VMIX_PROFILE_OMT_SQ,
        VMIX_PROFILE_OMT_HQ,
};

#define OFFSET(x) offsetof(VMIXEncContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption vmix_options[] = {
        { "omt_profile", "OMT VMX profile", OFFSET(profile), AV_OPT_TYPE_INT,
            { .i64 = VMIX_PROFILE_OMT_SQ }, VMIX_PROFILE_OMT_LQ, VMIX_PROFILE_OMT_HQ,
            VE, .unit = "omt_profile" },
        { "low", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = VMIX_PROFILE_OMT_LQ }, 0, 0,
            VE, .unit = "omt_profile" },
        { "medium", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = VMIX_PROFILE_OMT_SQ }, 0, 0,
            VE, .unit = "omt_profile" },
        { "high", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = VMIX_PROFILE_OMT_HQ }, 0, 0,
            VE, .unit = "omt_profile" },
        { "omt_quality", "VMX quality (52-98)", OFFSET(quality), AV_OPT_TYPE_INT,
            { .i64 = VMIX_QUALITY_DEFAULT }, 52, 98, VE },
        { NULL }
};

static const AVClass vmix_class = {
        .class_name = "vmix encoder",
        .item_name  = av_default_item_name,
        .option     = vmix_options,
        .version    = LIBAVUTIL_VERSION_INT,
};

static const uint8_t quant[64] = {
    16, 16, 19, 22, 26, 27, 29, 34,
    16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48,
    26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69,
    27, 29, 35, 38, 46, 56, 69, 83,
};

static const uint8_t vmix_quality_scales[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 18,
    20, 22, 24, 28, 32, 36, 40, 44, 48, 52, 56, 64,
};

static int vmix_quality_scale(int quality)
{
    int scale = 100 - quality;

    for (int i = 0; i < FF_ARRAY_ELEMS(vmix_quality_scales); i++)
        if (vmix_quality_scales[i] >= scale)
            return vmix_quality_scales[i];
    return vmix_quality_scales[FF_ARRAY_ELEMS(vmix_quality_scales) - 1];
}

static int vmix_profile_dc_shift(int profile, int height)
{
    if (profile == VMIX_PROFILE_OMT_HQ)
        return 0;
    if (profile == VMIX_PROFILE_OMT_SQ && height >= 1080 && height < 2160)
        return 0;
    return 3;
}

static int vmix_round_shift(int value, int shift)
{
    return (value + (1 << (shift - 1))) >> shift;
}

static int vmix_quantize(int value, int divisor)
{
    int64_t scaled = (int64_t)value * 16;
    int64_t magnitude = scaled < 0 ? -scaled : scaled;
    int level = (magnitude + divisor / 2) / divisor;

    return av_clip_int16(scaled < 0 ? -level : level);
}

static int vmix_shift_8(int value)
{
    return value < 0 ? -((-value + 255) >> 8) : value >> 8;
}

static int vmix_rgb_component(const uint8_t *pixel, const int16_t coeff[3], int offset)
{
    int b = pixel[0], g = pixel[1], r = pixel[2];
    int sum = r * coeff[0] + g * coeff[1] + b * coeff[2] + 128;
    return av_clip_uint8(vmix_shift_8(sum) + offset);
}

static void vmix_bgr0_to_yuv422(const AVFrame *frame, AVFrame *planar)
{
    static const int16_t coeff_601[3][3] = {
        { 66, 129, 25 }, { -38, -74, 112 }, { 112, -94, -18 },
    };
    static const int16_t coeff_709[3][3] = {
        { 47, 157, 16 }, { -26, -86, 112 }, { 112, -102, -10 },
    };
    int is_709 = frame->colorspace == AVCOL_SPC_BT709 ||
                 (frame->colorspace == AVCOL_SPC_UNSPECIFIED && frame->height >= 720);
    const int16_t (*coeff)[3] = is_709 ? coeff_709 : coeff_601;

    for (int y = 0; y < frame->height; y++) {
        const uint8_t *src = frame->data[0] + (ptrdiff_t)y * frame->linesize[0];
        uint8_t *y_dst = planar->data[0] + (ptrdiff_t)y * planar->linesize[0];
        uint8_t *u_dst = planar->data[1] + (ptrdiff_t)y * planar->linesize[1];
        uint8_t *v_dst = planar->data[2] + (ptrdiff_t)y * planar->linesize[2];

        for (int x = 0; x < frame->width; x += 2) {
            const uint8_t *pixel0 = src + x * 4;
            const uint8_t *pixel1 = pixel0 + 4;
            y_dst[x] = vmix_rgb_component(pixel0, coeff[0], 16);
            y_dst[x + 1] = vmix_rgb_component(pixel1, coeff[0], 16);
            u_dst[x / 2] = (vmix_rgb_component(pixel0, coeff[1], 128) +
                            vmix_rgb_component(pixel1, coeff[1], 128)) >> 1;
            v_dst[x / 2] = (vmix_rgb_component(pixel0, coeff[2], 128) +
                            vmix_rgb_component(pixel1, coeff[2], 128)) >> 1;
        }
    }
}

static void vmix_put_signed(PutBitContext *pb, int value)
{
    uint32_t magnitude = value < 0 ? -(int64_t)value : value;
    uint32_t code = value ? (value > 0 ? 2 * magnitude : 2 * magnitude - 1) : 0;
    set_ue_golomb_long(pb, code);
}

static void vmix_put_zero_run(PutBitContext *pb, unsigned int run)
{
    vmix_put_signed(pb, 0);
    set_ue_golomb_long(pb, run - 1);
}

static void vmix_align_put_bits(PutBitContext *pb)
{
    int padding = (-put_bits_count(pb)) & 7;
    if (padding)
        put_bits(pb, padding, 0);
}

static int vmix_encode_plane(VMIXEncContext *s, PutBitContext *dc_pb,
                             PutBitContext *ac_pb, const AVFrame *frame,
                             int plane, int slice_y)
{
    LOCAL_ALIGNED_16(int16_t, block, [64]);
    const uint8_t *src = frame->data[plane];
    int src_width = plane ? frame->width / 2 : frame->width;
    int dc_predictor = 0;
    unsigned int dc_run = 0, ac_run = 0;

    for (int y = slice_y; y < slice_y + 16; y += 8) {
        for (int x = 0; x < src_width; x += 8) {
            for (int row = 0; row < 8; row++) {
                const uint8_t *src_row = src + (ptrdiff_t)(y + row) * frame->linesize[plane] + x;
                for (int col = 0; col < 8; col++)
                    block[row * 8 + col] = src_row[col] - (plane ? 0 : 128);
            }
            s->fdsp.fdct(block);

            int dc = vmix_round_shift(vmix_quantize(block[0], quant[0]),
                                      s->dc_shift + 3);
            int dc_delta = dc - dc_predictor;
            if (!dc_delta) {
                dc_run++;
            } else {
                if (dc_run) {
                    vmix_put_zero_run(dc_pb, dc_run);
                    dc_run = 0;
                }
                vmix_put_signed(dc_pb, dc_delta);
            }
            dc_predictor = dc;

            for (int n = 0; n < 64; n++) {
                int index = ff_zigzag_direct[n];
                int level = index ? vmix_quantize(block[index],
                                                   quant[index] * s->quality_scale * VMIX_FDCT_SCALE) : 0;
                if (!level) {
                    ac_run++;
                } else {
                    if (ac_run) {
                        vmix_put_zero_run(ac_pb, ac_run);
                        ac_run = 0;
                    }
                    vmix_put_signed(ac_pb, level);
                }
            }
        }
    }
    if (dc_run)
        vmix_put_zero_run(dc_pb, dc_run);
    if (ac_run)
        vmix_put_zero_run(ac_pb, ac_run);
    vmix_align_put_bits(dc_pb);
    vmix_align_put_bits(ac_pb);
    return 0;
}

static av_cold int vmix_encode_init(AVCodecContext *avctx)
{
    VMIXEncContext *s = avctx->priv_data;

    if ((avctx->pix_fmt != AV_PIX_FMT_YUV422P &&
         avctx->pix_fmt != AV_PIX_FMT_UYVY422 &&
            avctx->pix_fmt != AV_PIX_FMT_YUYV422 &&
            avctx->pix_fmt != AV_PIX_FMT_NV12 &&
             avctx->pix_fmt != AV_PIX_FMT_YUV420P &&
             avctx->pix_fmt != AV_PIX_FMT_BGR0) ||
        avctx->width < 16 || avctx->height < 16 ||
        avctx->width > 7680 || avctx->height > 4080 ||
        avctx->width % 16 || avctx->height % 16) {
        av_log(avctx, AV_LOG_ERROR,
               "VMIX encoder requires YUV422P, UYVY422, YUYV422, NV12, YUV420P, or BGR0 dimensions divisible by 16\n");
        return AVERROR(EINVAL);
    }
    avctx->bits_per_raw_sample = 8;
    s->quality_scale = vmix_quality_scale(s->quality);
    s->dc_shift = vmix_profile_dc_shift(s->profile, avctx->height);
    ff_fdctdsp_init(&s->fdsp, avctx);
    return 0;
}

static int vmix_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                             const AVFrame *frame, int *got_packet)
{
    VMIXEncContext *s = avctx->priv_data;
    VMIXEncodeSlice *slices = NULL;
    AVFrame planar_frame = { 0 };
    const AVFrame *encode_frame = frame;
    uint8_t *planar_data = NULL;
    size_t slice_capacity, total_capacity;
    size_t luma_size, chroma_size, planar_size;
    int nb_slices;
    int ret = 0;

    *got_packet = 0;
    if (!frame)
        return 0;
    nb_slices = frame->height / 16;
    if (frame->format != avctx->pix_fmt || frame->width != avctx->width ||
        frame->height != avctx->height || frame->width < 16 || frame->height < 16 ||
        frame->width > 7680 || frame->height > 4080 ||
        frame->width % 16 || frame->height % 16)
        return AVERROR(EINVAL);
    if (frame->format == AV_PIX_FMT_YUV422P) {
        for (int plane = 0; plane < 3; plane++) {
            int width = plane ? frame->width / 2 : frame->width;
            if (!frame->data[plane] || FFABS(frame->linesize[plane]) < width)
                return AVERROR(EINVAL);
        }
    } else if (frame->format == AV_PIX_FMT_NV12) {
        if (!frame->data[0] || !frame->data[1] ||
            FFABS(frame->linesize[0]) < frame->width ||
            FFABS(frame->linesize[1]) < frame->width)
            return AVERROR(EINVAL);
    } else if (frame->format == AV_PIX_FMT_YUV420P) {
        if (!frame->data[0] || !frame->data[1] || !frame->data[2] ||
            FFABS(frame->linesize[0]) < frame->width ||
            FFABS(frame->linesize[1]) < frame->width / 2 ||
            FFABS(frame->linesize[2]) < frame->width / 2)
            return AVERROR(EINVAL);
    } else if (frame->format == AV_PIX_FMT_BGR0) {
        if (!frame->data[0] || FFABS(frame->linesize[0]) < frame->width * 4)
            return AVERROR(EINVAL);
    } else if (!frame->data[0] || FFABS(frame->linesize[0]) < frame->width * 2) {
        return AVERROR(EINVAL);
    }
    if (av_size_mult((size_t)frame->width, frame->height, &luma_size) < 0 ||
        av_size_mult(luma_size, 2, &planar_size) < 0)
        return AVERROR(EINVAL);
    chroma_size = luma_size / 2;

    if (av_size_mult((size_t)frame->width, 128, &slice_capacity) < 0 ||
        slice_capacity > SIZE_MAX - 4096)
        return AVERROR(EINVAL);
    slice_capacity += 4096;
    if (av_size_mult((size_t)nb_slices, 2 * (slice_capacity + 4), &total_capacity) < 0 ||
        total_capacity > SIZE_MAX - 5)
        return AVERROR(EINVAL);
    total_capacity += 5;
    if (total_capacity > VMIX_MAX_PACKET_SIZE || total_capacity > INT_MAX)
        return AVERROR(EINVAL);

    slices = av_calloc(nb_slices, sizeof(*slices));
    if (!slices)
        return AVERROR(ENOMEM);
    if (frame->format != AV_PIX_FMT_YUV422P) {
        planar_data = av_malloc(planar_size);
        if (!planar_data) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        planar_frame.width = frame->width;
        planar_frame.height = frame->height;
        planar_frame.format = AV_PIX_FMT_YUV422P;
        planar_frame.pts = frame->pts;
        planar_frame.data[0] = planar_data;
        planar_frame.data[1] = planar_data + luma_size;
        planar_frame.data[2] = planar_data + luma_size + chroma_size;
        planar_frame.linesize[0] = frame->width;
        planar_frame.linesize[1] = planar_frame.linesize[2] = frame->width / 2;
        if (frame->format == AV_PIX_FMT_NV12 || frame->format == AV_PIX_FMT_YUV420P) {
            for (int y = 0; y < frame->height; y++)
                memcpy(planar_frame.data[0] + (ptrdiff_t)y * planar_frame.linesize[0],
                       frame->data[0] + (ptrdiff_t)y * frame->linesize[0], frame->width);
            for (int y = 0; y < frame->height / 2; y++) {
                const uint8_t *uv = frame->format == AV_PIX_FMT_NV12 ?
                                    frame->data[1] + (ptrdiff_t)y * frame->linesize[1] : NULL;
                const uint8_t *u = frame->format == AV_PIX_FMT_YUV420P ?
                                   frame->data[1] + (ptrdiff_t)y * frame->linesize[1] : uv;
                const uint8_t *v = frame->format == AV_PIX_FMT_YUV420P ?
                                   frame->data[2] + (ptrdiff_t)y * frame->linesize[2] : uv + 1;
                uint8_t *u0 = planar_frame.data[1] + (ptrdiff_t)(y * 2) * planar_frame.linesize[1];
                uint8_t *u1 = u0 + planar_frame.linesize[1];
                uint8_t *v0 = planar_frame.data[2] + (ptrdiff_t)(y * 2) * planar_frame.linesize[2];
                uint8_t *v1 = v0 + planar_frame.linesize[2];
                for (int x = 0; x < frame->width / 2; x++) {
                    int source_x = frame->format == AV_PIX_FMT_NV12 ? x * 2 : x;
                    u0[x] = u1[x] = u[source_x];
                    v0[x] = v1[x] = v[frame->format == AV_PIX_FMT_NV12 ? source_x : x];
                }
            }
        } else if (frame->format == AV_PIX_FMT_BGR0) {
            vmix_bgr0_to_yuv422(frame, &planar_frame);
        } else {
            for (int y = 0; y < frame->height; y++) {
                const uint8_t *src = frame->data[0] + (ptrdiff_t)y * frame->linesize[0];
                uint8_t *dst_y = planar_frame.data[0] + (ptrdiff_t)y * planar_frame.linesize[0];
                uint8_t *dst_u = planar_frame.data[1] + (ptrdiff_t)y * planar_frame.linesize[1];
                uint8_t *dst_v = planar_frame.data[2] + (ptrdiff_t)y * planar_frame.linesize[2];
                for (int x = 0; x < frame->width; x += 2) {
                    if (frame->format == AV_PIX_FMT_UYVY422) {
                        dst_u[x / 2] = src[x * 2];
                        dst_y[x] = src[x * 2 + 1];
                        dst_v[x / 2] = src[x * 2 + 2];
                        dst_y[x + 1] = src[x * 2 + 3];
                    } else {
                        dst_y[x] = src[x * 2];
                        dst_u[x / 2] = src[x * 2 + 1];
                        dst_y[x + 1] = src[x * 2 + 2];
                        dst_v[x / 2] = src[x * 2 + 3];
                    }
                }
            }
        }
        encode_frame = &planar_frame;
    }
    for (int i = 0; i < nb_slices; i++) {
        PutBitContext dc_pb, ac_pb;

        slices[i].dc = av_malloc(slice_capacity);
        slices[i].ac = av_malloc(slice_capacity);
        if (!slices[i].dc || !slices[i].ac) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        init_put_bits(&dc_pb, slices[i].dc, slice_capacity);
        init_put_bits(&ac_pb, slices[i].ac, slice_capacity);
        for (int plane = 0; plane < 3; plane++) {
            ret = vmix_encode_plane(s, &dc_pb, &ac_pb, encode_frame, plane, i * 16);
            if (ret < 0)
                goto end;
        }
        flush_put_bits(&dc_pb);
        flush_put_bits(&ac_pb);
        slices[i].dc_size = put_bytes_output(&dc_pb);
        slices[i].ac_size = put_bytes_output(&ac_pb);
    }

    size_t packet_size = 5;
    for (int i = 0; i < nb_slices; i++)
        packet_size += 8 + slices[i].dc_size + slices[i].ac_size;
    ret = ff_alloc_packet(avctx, pkt, packet_size);
    if (ret < 0)
        goto end;
    pkt->data[0] = 3;
    pkt->data[1] = s->dc_shift;
    pkt->data[2] = 1;
    pkt->data[3] = 100 - s->quality_scale;
    pkt->data[4] = nb_slices;
    size_t offset = 5;
    for (int i = 0; i < nb_slices; i++) {
        AV_WL32(pkt->data + offset, slices[i].dc_size);
        offset += 4;
        memcpy(pkt->data + offset, slices[i].dc, slices[i].dc_size);
        offset += slices[i].dc_size;
    }
    for (int i = 0; i < nb_slices; i++) {
        AV_WL32(pkt->data + offset, slices[i].ac_size);
        offset += 4;
        memcpy(pkt->data + offset, slices[i].ac, slices[i].ac_size);
        offset += slices[i].ac_size;
    }
    pkt->pts = frame->pts;
    pkt->duration = 1;
    pkt->flags |= AV_PKT_FLAG_KEY;
    *got_packet = 1;

end:
    for (int i = 0; i < nb_slices; i++) {
        av_freep(&slices[i].dc);
        av_freep(&slices[i].ac);
    }
    av_free(slices);
    av_free(planar_data);
    return ret;
}

const FFCodec ff_vmix_encoder = {
    .p.name           = "vmix",
    CODEC_LONG_NAME("vMix Video"),
    .p.type           = AVMEDIA_TYPE_VIDEO,
    .p.id             = AV_CODEC_ID_VMIX,
    .p.priv_class     = &vmix_class,
    .priv_data_size   = sizeof(VMIXEncContext),
    .init             = vmix_encode_init,
    FF_CODEC_ENCODE_CB(vmix_encode_frame),
    CODEC_PIXFMTS(AV_PIX_FMT_YUV422P, AV_PIX_FMT_UYVY422, AV_PIX_FMT_YUYV422,
                  AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P, AV_PIX_FMT_BGR0),
};