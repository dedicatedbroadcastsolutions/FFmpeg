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

#include "avcodec.h"
#include "codec_internal.h"
#include "encode.h"
#include "fdctdsp.h"
#include "idctdsp.h"
#include "mathops.h"
#include "put_bits.h"
#include "put_golomb.h"

#define VMIX_QUALITY 80
#define VMIX_QUALITY_SCALE 20
#define VMIX_DC_SHIFT 3
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
    FDCTDSPContext fdsp;
} VMIXEncContext;

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
                                      VMIX_DC_SHIFT + 3);
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
                                                   quant[index] * VMIX_QUALITY_SCALE * VMIX_FDCT_SCALE) : 0;
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

    if (avctx->pix_fmt != AV_PIX_FMT_YUV422P ||
        avctx->width < 16 || avctx->height < 16 ||
        avctx->width > 7680 || avctx->height > 4080 ||
        avctx->width % 16 || avctx->height % 16) {
        av_log(avctx, AV_LOG_ERROR,
               "VMIX encoder requires YUV422P dimensions divisible by 16 (max 7680x4080)\n");
        return AVERROR(EINVAL);
    }
    avctx->bits_per_raw_sample = 8;
    ff_fdctdsp_init(&s->fdsp, avctx);
    return 0;
}

static int vmix_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                             const AVFrame *frame, int *got_packet)
{
    VMIXEncContext *s = avctx->priv_data;
    VMIXEncodeSlice *slices = NULL;
    size_t slice_capacity, total_capacity;
    int nb_slices;
    int ret = 0;

    *got_packet = 0;
    if (!frame)
        return 0;
    nb_slices = frame->height / 16;
    if (frame->format != AV_PIX_FMT_YUV422P || frame->width != avctx->width ||
        frame->height != avctx->height || frame->width < 16 || frame->height < 16 ||
        frame->width > 7680 || frame->height > 4080 ||
        frame->width % 16 || frame->height % 16)
        return AVERROR(EINVAL);
    for (int plane = 0; plane < 3; plane++) {
        int width = plane ? frame->width / 2 : frame->width;
        if (!frame->data[plane] || FFABS(frame->linesize[plane]) < width)
            return AVERROR(EINVAL);
    }
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
            ret = vmix_encode_plane(s, &dc_pb, &ac_pb, frame, plane, i * 16);
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
    pkt->data[1] = VMIX_DC_SHIFT;
    pkt->data[2] = 1;
    pkt->data[3] = VMIX_QUALITY;
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
    return ret;
}

const FFCodec ff_vmix_encoder = {
    .p.name           = "vmix",
    CODEC_LONG_NAME("vMix Video"),
    .p.type           = AVMEDIA_TYPE_VIDEO,
    .p.id             = AV_CODEC_ID_VMIX,
    .priv_data_size   = sizeof(VMIXEncContext),
    .init             = vmix_encode_init,
    FF_CODEC_ENCODE_CB(vmix_encode_frame),
    CODEC_PIXFMTS(AV_PIX_FMT_YUV422P),
};