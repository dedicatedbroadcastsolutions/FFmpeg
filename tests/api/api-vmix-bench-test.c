#include <stdbool.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"

#include "vmxcodec.h"

#define DEFAULT_WIDTH 1280
#define DEFAULT_HEIGHT 720
#define DEFAULT_FRAMES 300
#define WARMUP_FRAMES 10
#define MAX_FRAME_WIDTH 7680
#define MAX_FRAME_HEIGHT 4320
#define MAX_PACKET_SIZE (256 * 1024 * 1024)

static void fill_yuyv(AVFrame *frame, int number)
{
    for (int y = 0; y < frame->height; y++) {
        uint8_t *row = frame->data[0] + (ptrdiff_t)y * frame->linesize[0];

        for (int x = 0; x < frame->width; x += 2) {
            int chroma = 96 + ((x / 2 + y + number) % 64);
            row[x * 2]     = 16 + ((x + number) % 220);
            row[x * 2 + 1] = chroma;
            row[x * 2 + 2] = 16 + ((x + y + number * 3) % 220);
            row[x * 2 + 3] = 160 - ((x / 2 + y + number) % 64);
        }
    }
}

static int compare_decoded(const AVFrame *source, const AVFrame *native,
                           const uint8_t *reference, uint64_t *native_sse,
                           uint64_t *reference_sse)
{
    if (native->format != AV_PIX_FMT_YUV422P)
        return AVERROR_INVALIDDATA;

    for (int y = 0; y < native->height; y++) {
        const uint8_t *src = source->data[0] + (ptrdiff_t)y * source->linesize[0];
        const uint8_t *y_plane = native->data[0] + (ptrdiff_t)y * native->linesize[0];
        const uint8_t *u_plane = native->data[1] + (ptrdiff_t)y * native->linesize[1];
        const uint8_t *v_plane = native->data[2] + (ptrdiff_t)y * native->linesize[2];
        const uint8_t *ref = reference + (ptrdiff_t)y * native->width * 2;

        for (int x = 0; x < native->width; x += 2) {
            const uint8_t decoded[] = {
                y_plane[x], u_plane[x / 2], y_plane[x + 1], v_plane[x / 2],
            };

            for (int i = 0; i < 4; i++) {
                int native_error = (int)decoded[i] - src[x * 2 + i];
                int reference_error = (int)ref[x * 2 + i] - src[x * 2 + i];

                *native_sse += native_error * native_error;
                *reference_sse += reference_error * reference_error;
            }
        }
    }

    return 0;
}

static int compare_outputs(const AVFrame *native, const uint8_t *reference,
                           int *max_error)
{
    int error_max = 0;

    if (native->format != AV_PIX_FMT_YUV422P)
        return AVERROR_INVALIDDATA;
    for (int y = 0; y < native->height; y++) {
        const uint8_t *y_plane = native->data[0] + (ptrdiff_t)y * native->linesize[0];
        const uint8_t *u_plane = native->data[1] + (ptrdiff_t)y * native->linesize[1];
        const uint8_t *v_plane = native->data[2] + (ptrdiff_t)y * native->linesize[2];
        const uint8_t *ref = reference + (ptrdiff_t)y * native->width * 2;

        for (int x = 0; x < native->width; x += 2) {
            const uint8_t decoded[] = {
                y_plane[x], u_plane[x / 2], y_plane[x + 1], v_plane[x / 2],
            };
            for (int i = 0; i < 4; i++)
                error_max = FFMAX(error_max, FFABS((int)decoded[i] - ref[x * 2 + i]));
        }
    }
    *max_error = FFMAX(*max_error, error_max);
    return 0;
}

static int run_benchmark(int width, int height, int frames)
{
    const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_VMIX);
    const AVCodec *decoder = avcodec_find_decoder(AV_CODEC_ID_VMIX);
    AVCodecContext *enc = NULL, *dec = NULL;
    AVFrame *source = NULL, *decoded = NULL, *native_cross = NULL;
    AVPacket *packet = NULL, *reference_packet = NULL;
    VMX_SIZE size = { width, height };
    VMX_INSTANCE *ref_enc = NULL, *ref_dec = NULL;
    uint8_t *ref_packet = NULL, *ref_decoded = NULL, *ref_cross = NULL;
    size_t max_packet = (size_t)width * height * 4;
    int64_t ff_encode_us = 0, ff_decode_us = 0;
    int64_t ref_encode_us = 0, ref_decode_us = 0;
    uint64_t ff_bytes = 0, ref_bytes = 0;
    int max_error = 0, ret = 0;
    uint64_t native_sse = 0, reference_sse = 0;

    if (!encoder || !decoder)
        return AVERROR(ENOSYS);
    if (width < 16 || height < 16 || width > MAX_FRAME_WIDTH ||
        height > MAX_FRAME_HEIGHT || (width & 15) || (height & 15) ||
        frames < 1 || frames > INT_MAX - WARMUP_FRAMES ||
        max_packet > MAX_PACKET_SIZE)
        return AVERROR(EINVAL);

    enc = avcodec_alloc_context3(encoder);
    dec = avcodec_alloc_context3(decoder);
    source = av_frame_alloc();
    decoded = av_frame_alloc();
    native_cross = av_frame_alloc();
    packet = av_packet_alloc();
    reference_packet = av_packet_alloc();
    ref_packet = av_malloc(max_packet);
    ref_decoded = av_malloc((size_t)width * height * 2);
    ref_cross = av_malloc((size_t)width * height * 2);
    ref_enc = VMX_Create(size, VMX_PROFILE_OMT_SQ, VMX_COLORSPACE_UNDEFINED);
    ref_dec = VMX_Create(size, VMX_PROFILE_OMT_SQ, VMX_COLORSPACE_UNDEFINED);
    if (!enc || !dec || !source || !decoded || !native_cross || !packet ||
        !reference_packet || !ref_packet || !ref_decoded || !ref_cross ||
        !ref_enc || !ref_dec) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    enc->width = width;
    enc->height = height;
    enc->pix_fmt = AV_PIX_FMT_YUYV422;
    enc->time_base = (AVRational){ 1, 60 };
    enc->thread_count = 1;
    dec->width = width;
    dec->height = height;
    dec->bits_per_raw_sample = 8;
    dec->thread_count = 1;
    source->width = width;
    source->height = height;
    source->format = AV_PIX_FMT_YUYV422;

    AVDictionary *options = NULL;
    if ((ret = av_dict_set(&options, "omt_profile", "medium", 0)) < 0 ||
        (ret = av_dict_set_int(&options, "omt_quality", 80, 0)) < 0 ||
        (ret = avcodec_open2(enc, encoder, &options)) < 0 ||
        (ret = avcodec_open2(dec, decoder, NULL)) < 0 ||
        (ret = av_frame_get_buffer(source, 32)) < 0) {
        av_dict_free(&options);
        goto end;
    }
    av_dict_free(&options);
    VMX_SetThreads(ref_enc, 1);
    VMX_SetThreads(ref_dec, 1);
    VMX_SetQuality(ref_enc, 80);
    VMX_SetQuality(ref_dec, 80);

    for (int i = 0; i < frames + WARMUP_FRAMES; i++) {
        int64_t start;
        int ref_size;

        if ((ret = av_frame_make_writable(source)) < 0)
            goto end;
        source->pts = i;
        fill_yuyv(source, i);

        start = av_gettime_relative();
        if ((ret = avcodec_send_frame(enc, source)) < 0 ||
            (ret = avcodec_receive_packet(enc, packet)) < 0)
            goto end;
        if (i >= WARMUP_FRAMES) {
            ff_encode_us += av_gettime_relative() - start;
            ff_bytes += packet->size;
        }
        int expected_dc_shift = height >= 1080 && height < 2160 ? 0 : 3;
        if (packet->size < 5 || packet->data[0] != 3 ||
            packet->data[1] != expected_dc_shift || packet->data[3] != 80) {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }

        start = av_gettime_relative();
        VMX_SetQuality(ref_enc, 80);
        if (VMX_EncodeYUY2(ref_enc, source->data[0], source->linesize[0], 0) != VMX_ERR_OK) {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }
        ref_size = VMX_SaveTo(ref_enc, ref_packet, max_packet);
        if (ref_size <= 0) {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }
        if (i >= WARMUP_FRAMES) {
            ref_encode_us += av_gettime_relative() - start;
            ref_bytes += ref_size;
        }
        if (ref_size < 5 || ref_packet[0] != 3 ||
            ref_packet[1] != expected_dc_shift || ref_packet[3] != 80) {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }

        start = av_gettime_relative();
        if ((ret = avcodec_send_packet(dec, packet)) < 0 ||
            (ret = avcodec_receive_frame(dec, decoded)) < 0)
            goto end;
        if (i >= WARMUP_FRAMES)
            ff_decode_us += av_gettime_relative() - start;

        start = av_gettime_relative();
        if (VMX_LoadFrom(ref_dec, ref_packet, ref_size) != VMX_ERR_OK ||
            VMX_DecodeYUY2(ref_dec, ref_decoded, width * 2) != VMX_ERR_OK) {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }
        if (i >= WARMUP_FRAMES)
            ref_decode_us += av_gettime_relative() - start;

        if (i >= WARMUP_FRAMES &&
            (ret = compare_decoded(source, decoded, ref_decoded, &native_sse,
                                   &reference_sse)) < 0)
            goto end;

        if ((ret = av_new_packet(reference_packet, ref_size)) < 0)
            goto end;
        memcpy(reference_packet->data, ref_packet, ref_size);
        av_frame_unref(native_cross);
        if ((ret = avcodec_send_packet(dec, reference_packet)) < 0 ||
            (ret = avcodec_receive_frame(dec, native_cross)) < 0)
            goto end;
        if (VMX_LoadFrom(ref_dec, packet->data, packet->size) != VMX_ERR_OK ||
            VMX_DecodeYUY2(ref_dec, ref_cross, width * 2) != VMX_ERR_OK) {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }
        if (i >= WARMUP_FRAMES &&
            ((ret = compare_outputs(native_cross, ref_decoded, &max_error)) < 0 ||
             (ret = compare_outputs(decoded, ref_cross, &max_error)) < 0))
            goto end;
        av_packet_unref(reference_packet);
        av_packet_unref(packet);
        av_frame_unref(decoded);
    }

    double samples = (double)width * height * 2 * frames;
    double native_psnr = native_sse ? 10.0 * log10(65025.0 * samples / native_sse) : INFINITY;
    double reference_psnr = reference_sse ? 10.0 * log10(65025.0 * samples / reference_sse) : INFINITY;
    double psnr_delta = fabs(native_psnr - reference_psnr);
    if (max_error > 2 || psnr_delta > 0.1) {
        fprintf(stderr, "VMIX quality parity failed: PSNR delta %.3f dB (limit 0.1), max cross error %d (limit 2)\n",
                psnr_delta, max_error);
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

        printf("vmix,ffmpeg,encode,%d,%d,%d,1,%.6f,%.2f,%" PRIu64 ",%.3f,%d\n",
           width, height, frames, ff_encode_us / 1000000.0,
           frames * 1000000.0 / FFMAX(ff_encode_us, 1), ff_bytes, native_psnr, max_error);
        printf("vmix,libvmx,encode,%d,%d,%d,1,%.6f,%.2f,%" PRIu64 ",%.3f,%d\n",
           width, height, frames, ref_encode_us / 1000000.0,
           frames * 1000000.0 / FFMAX(ref_encode_us, 1), ref_bytes, reference_psnr, max_error);
        printf("vmix,ffmpeg,decode,%d,%d,%d,1,%.6f,%.2f,%" PRIu64 ",%.3f,%d\n",
           width, height, frames, ff_decode_us / 1000000.0,
           frames * 1000000.0 / FFMAX(ff_decode_us, 1), ff_bytes, native_psnr, max_error);
        printf("vmix,libvmx,decode,%d,%d,%d,1,%.6f,%.2f,%" PRIu64 ",%.3f,%d\n",
           width, height, frames, ref_decode_us / 1000000.0,
           frames * 1000000.0 / FFMAX(ref_decode_us, 1), ref_bytes, reference_psnr, max_error);
    fprintf(stderr, "quality,ffmpeg_psnr=%.3f,libvmx_psnr=%.3f,delta_db=%.3f,max_cross_error=%d\n",
            native_psnr, reference_psnr, psnr_delta, max_error);

end:
    VMX_Destroy(ref_enc);
    VMX_Destroy(ref_dec);
    av_free(ref_packet);
    av_free(ref_decoded);
    av_free(ref_cross);
    av_packet_free(&packet);
    av_packet_free(&reference_packet);
    av_frame_free(&source);
    av_frame_free(&decoded);
    av_frame_free(&native_cross);
    avcodec_free_context(&enc);
    avcodec_free_context(&dec);
    return ret;
}

int main(int argc, char **argv)
{
    int width = argc > 1 ? atoi(argv[1]) : DEFAULT_WIDTH;
    int height = argc > 2 ? atoi(argv[2]) : DEFAULT_HEIGHT;
    int frames = argc > 3 ? atoi(argv[3]) : DEFAULT_FRAMES;
    int ret;

    av_log_set_level(AV_LOG_ERROR);
    puts("domain,implementation,operation,width,height,frames,threads,seconds,fps,bytes,psnr_db,max_cross_error");
    ret = run_benchmark(width, height, frames);
    if (ret < 0) {
        fprintf(stderr, "VMIX benchmark failed: %s\n", av_err2str(ret));
        return 1;
    }
    return 0;
}