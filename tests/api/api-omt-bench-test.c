#include <stdatomic.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <pthread.h>

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/common.h"
#include "libavutil/dict.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"

#include "libomt.h"

#define DEFAULT_WIDTH 1280
#define DEFAULT_HEIGHT 720
#define DEFAULT_FRAMES 300
#define MAX_FRAME_SIZE (256 * 1024 * 1024)
#define OMT_PORT_MIN 20000
#define OMT_PORT_RANGE 30000

typedef struct OMTBenchReceiver {
    omt_receive_t *receiver;
    const uint8_t *source;
    int width;
    int height;
    int expected_frames;
    atomic_int received_frames;
    atomic_int error;
    atomic_int stop;
    atomic_uint_fast64_t compressed_bytes;
    uint64_t squared_error;
} OMTBenchReceiver;

typedef struct OMTFFmpegReceiver {
    AVFormatContext *format;
    const uint8_t *source;
    int width;
    int height;
    int expected_frames;
    atomic_int received_frames;
    atomic_int error;
    atomic_int error_code;
    uint64_t bytes;
    uint64_t squared_error;
} OMTFFmpegReceiver;

static const char *stage;

static void omt_log(const char *message)
{
    fprintf(stderr, "libomt: %s\n", message);
}

static int64_t monotonic_us(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts))
        return -1;
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void *receive_frames(void *opaque)
{
    OMTBenchReceiver *state = opaque;
    int64_t deadline = monotonic_us() + 30000000;

    while (atomic_load(&state->received_frames) < state->expected_frames &&
           !atomic_load(&state->stop) && monotonic_us() < deadline) {
        OMTMediaFrame *frame = omt_receive(state->receiver, OMTFrameType_Video, 1000);

        if (!frame)
            continue;
        if (frame->Type != OMTFrameType_Video || frame->Codec != OMTCodec_UYVY ||
            frame->Width != state->width || frame->Height != state->height ||
            frame->DataLength != frame->Width * frame->Height * 2 || !frame->Data ||
            frame->CompressedLength <= 0) {
            atomic_store(&state->error, 1);
            break;
        }
        const uint8_t *decoded = frame->Data;
        for (int y = 0; y < state->height; y++) {
            const uint8_t *src = state->source + (size_t)y * state->width * 2;
            const uint8_t *row = decoded + (size_t)y * state->width * 2;
            for (int x = 0; x < state->width; x += 2) {
                int diff[4] = {
                    (int)row[x * 2 + 1] - src[x * 2],
                    (int)row[x * 2] - src[x * 2 + 1],
                    (int)row[x * 2 + 3] - src[x * 2 + 2],
                    (int)row[x * 2 + 2] - src[x * 2 + 3],
                };
                for (int i = 0; i < 4; i++)
                    state->squared_error += diff[i] * diff[i];
            }
        }
        atomic_fetch_add(&state->compressed_bytes, frame->CompressedLength);
        atomic_fetch_add(&state->received_frames, 1);
    }
    if (atomic_load(&state->received_frames) != state->expected_frames)
        atomic_store(&state->error, 1);
    return NULL;
}

static void *receive_ffmpeg_frames(void *opaque)
{
    OMTFFmpegReceiver *state = opaque;
    AVCodecContext *decoder = NULL;
    AVPacket *packet = av_packet_alloc();
    AVFrame *decoded = av_frame_alloc();

    if (!packet || !decoded) {
        atomic_store(&state->error, 1);
        goto end;
    }
    while (atomic_load(&state->received_frames) < state->expected_frames) {
        int ret = av_read_frame(state->format, packet);

        if (ret < 0 || packet->stream_index < 0 ||
            packet->stream_index >= state->format->nb_streams) {
            atomic_store(&state->error_code,
                         ret < 0 ? ret : AVERROR_INVALIDDATA);
            atomic_store(&state->error, 1);
            break;
        }
        if (state->format->streams[packet->stream_index]->codecpar->codec_type !=
            AVMEDIA_TYPE_VIDEO) {
            av_packet_unref(packet);
            continue;
        }
        state->bytes += packet->size;
        if (!decoder) {
            const AVCodec *codec = avcodec_find_decoder(
                state->format->streams[packet->stream_index]->codecpar->codec_id);
            if (!codec || !(decoder = avcodec_alloc_context3(codec)) ||
                avcodec_parameters_to_context(decoder,
                    state->format->streams[packet->stream_index]->codecpar) < 0) {
                atomic_store(&state->error_code, AVERROR_DECODER_NOT_FOUND);
                atomic_store(&state->error, 1);
                break;
            }
            decoder->thread_count = 1;
            if (avcodec_open2(decoder, codec, NULL) < 0) {
                atomic_store(&state->error_code, AVERROR_INVALIDDATA);
                atomic_store(&state->error, 1);
                break;
            }
        }
        ret = avcodec_send_packet(decoder, packet);
        if (ret >= 0)
            ret = avcodec_receive_frame(decoder, decoded);
        if (ret < 0 || decoded->format != AV_PIX_FMT_YUV422P ||
            decoded->width != state->width || decoded->height != state->height) {
            atomic_store(&state->error_code,
                         ret < 0 ? ret : AVERROR_INVALIDDATA);
            atomic_store(&state->error, 1);
            break;
        }
        for (int y = 0; y < state->height; y++) {
            const uint8_t *src = state->source + (size_t)y * state->width * 2;
            const uint8_t *y_plane = decoded->data[0] + (ptrdiff_t)y * decoded->linesize[0];
            const uint8_t *u_plane = decoded->data[1] + (ptrdiff_t)y * decoded->linesize[1];
            const uint8_t *v_plane = decoded->data[2] + (ptrdiff_t)y * decoded->linesize[2];

            for (int x = 0; x < state->width; x += 2) {
                int diff[4] = {
                    (int)y_plane[x] - src[x * 2],
                    (int)u_plane[x / 2] - src[x * 2 + 1],
                    (int)y_plane[x + 1] - src[x * 2 + 2],
                    (int)v_plane[x / 2] - src[x * 2 + 3],
                };
                for (int i = 0; i < 4; i++)
                    state->squared_error += diff[i] * diff[i];
            }
        }
        atomic_fetch_add(&state->received_frames, 1);
        av_packet_unref(packet);
        av_frame_unref(decoded);
    }
    if (atomic_load(&state->received_frames) != state->expected_frames)
        atomic_store(&state->error, 1);

end:
    avcodec_free_context(&decoder);
    av_packet_free(&packet);
    av_frame_free(&decoded);
    return NULL;
}

static int run_native_benchmark(const char *input, int width, int height,
                                int frames, int port)
{
    const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_VMIX);
    AVFormatContext *output = NULL;
    AVDictionary *options = NULL;
    AVCodecContext *enc = NULL;
    AVFrame *source = NULL;
    AVPacket *packet = NULL;
    FILE *file = NULL;
    uint8_t *input_frame = NULL;
    pthread_t thread;
    OMTFFmpegReceiver state = { .width = width, .height = height,
                                .expected_frames = frames };
    char url[64];
    int64_t frame_size_64 = (int64_t)width * height * 2;
    int frame_size;
    int64_t start, elapsed;
    int ret = AVERROR(EINVAL), thread_started = 0;
    const char *stage = "validate parameters";

    if (!encoder || width < 16 || height < 16 || width > 7680 || height > 4320 ||
        (width & 15) || (height & 15) || frames < 1 ||
        frame_size_64 <= 0 || frame_size_64 > MAX_FRAME_SIZE ||
        port < 1 || port > 65535)
        return AVERROR(EINVAL);
    frame_size = frame_size_64;
    stage = "open input frame";
    file = fopen(input, "rb");
    input_frame = av_malloc(frame_size);
    if (!file || !input_frame || fread(input_frame, 1, frame_size, file) != frame_size) {
        ret = AVERROR(EIO);
        goto end;
    }

    stage = "allocate codec resources";
    enc = avcodec_alloc_context3(encoder);
    source = av_frame_alloc();
    packet = av_packet_alloc();
    if (!enc || !source || !packet) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    enc->width = width;
    enc->height = height;
    enc->pix_fmt = AV_PIX_FMT_YUYV422;
    enc->time_base = (AVRational){ 1, 60 };
    enc->framerate = (AVRational){ 60, 1 };
    enc->thread_count = 1;
    source->width = width;
    source->height = height;
    source->format = AV_PIX_FMT_YUYV422;
    stage = "configure VMIX encoder";
    if ((ret = av_dict_set(&options, "omt_profile", "medium", 0)) < 0 ||
        (ret = av_dict_set_int(&options, "omt_quality", 80, 0)) < 0 ||
        (ret = avcodec_open2(enc, encoder, &options)) < 0 ||
        (ret = av_frame_get_buffer(source, 32)) < 0)
        goto end;
    av_dict_free(&options);
    for (int y = 0; y < height; y++)
        memcpy(source->data[0] + (ptrdiff_t)y * source->linesize[0],
               input_frame + (size_t)y * width * 2, width * 2);

    stage = "allocate OMT output";
    snprintf(url, sizeof(url), "omt://127.0.0.1:%d", port);
    if ((ret = avformat_alloc_output_context2(&output, NULL, "omt", url)) < 0 || !output)
        goto end;
    AVStream *stream = avformat_new_stream(output, NULL);
    if (!stream) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    if ((ret = avcodec_parameters_from_context(stream->codecpar, enc)) < 0)
        goto end;
    stream->codecpar->framerate = (AVRational){ 60, 1 };
    stream->time_base = (AVRational){ 1, 60 };
    stream->avg_frame_rate = stream->r_frame_rate = (AVRational){ 60, 1 };
    stage = "open OMT listener";
    if ((ret = avformat_write_header(output, NULL)) < 0)
        goto end;

    stage = "connect OMT demuxer";
    AVDictionary *input_options = NULL;
    if ((ret = av_dict_set_int(&input_options, "rw_timeout", 10000000, 0)) < 0 ||
        (ret = avformat_open_input(&state.format, url, av_find_input_format("omt"),
                                   &input_options)) < 0) {
        av_dict_free(&input_options);
        goto end;
    }
    av_dict_free(&input_options);
    state.source = input_frame;
    stage = "start OMT receiver thread";
    if (pthread_create(&thread, NULL, receive_ffmpeg_frames, &state)) {
        ret = AVERROR(errno);
        goto end;
    }
    thread_started = 1;
    av_usleep(100000);

    stage = "encode and send frames";
    start = av_gettime_relative();
    for (int i = 0; i < frames; i++) {
        source->pts = i;
        if ((ret = avcodec_send_frame(enc, source)) < 0 ||
            (ret = avcodec_receive_packet(enc, packet)) < 0)
            goto end;
        packet->stream_index = stream->index;
        if ((ret = av_write_frame(output, packet)) < 0)
            goto end;
        av_packet_unref(packet);
    }
    pthread_join(thread, NULL);
    thread_started = 0;
    elapsed = av_gettime_relative() - start;
    stage = "validate received frames";
    if (atomic_load(&state.error) || atomic_load(&state.received_frames) != frames) {
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    double samples = (double)width * height * 2 * frames;
    double psnr = state.squared_error ?
        10.0 * log10(65025.0 * samples / state.squared_error) : INFINITY;
    printf("omt,ffmpeg,end_to_end,%d,%d,%d,%.6f,%.2f,%" PRIu64 ",%.3f\n",
           width, height, frames, elapsed / 1000000.0,
           frames * 1000000.0 / FFMAX(elapsed, 1), state.bytes, psnr);
    ret = 0;

end:
    if (thread_started) {
        avformat_close_input(&state.format);
        pthread_join(thread, NULL);
    }
    if (output)
        av_write_trailer(output);
    avformat_free_context(output);
    avcodec_free_context(&enc);
    av_frame_free(&source);
    av_packet_free(&packet);
    av_free(input_frame);
    av_dict_free(&options);
    if (file)
        fclose(file);
    if (ret < 0)
        fprintf(stderr, "FFmpeg OMT benchmark failed during %s: %s; receiver: %s (%d/%d frames)\n",
            stage, av_err2str(ret),
            av_err2str(atomic_load(&state.error_code)),
            atomic_load(&state.received_frames), frames);
    return ret;
}

static int run_benchmark(const char *input, int width, int height, int frames, int port)
{
    FILE *file = NULL;
    uint8_t *frame_data = NULL;
    omt_send_t *sender = NULL;
    omt_receive_t *receiver = NULL;
    pthread_t thread;
    OMTBenchReceiver state = { 0 };
    OMTMediaFrame frame = { 0 };
    int64_t start, elapsed;
    int64_t frame_size_64 = (int64_t)width * height * 2;
    int frame_size;
    int ret = 1;

    if (width < 16 || height < 16 || width > 7680 || height > 4320 ||
        (width & 15) || (height & 15) || frames < 1 ||
        frame_size_64 <= 0 || frame_size_64 > MAX_FRAME_SIZE ||
        port < 1 || port > 65535)
        return 1;
    frame_size = frame_size_64;

    stage = "open input frames";
    file = fopen(input, "rb");
    frame_data = malloc(frame_size);
    if (!file || !frame_data)
        goto end;
    stage = "read first input frame";
    if (fread(frame_data, 1, frame_size, file) != frame_size)
        goto end;

    stage = "initialize reference logging";
    omt_setloggingcallback(omt_log);
    stage = "configure sender port";
    omt_settings_set_integer("NetworkPortStart", port);
    omt_settings_set_integer("NetworkPortEnd", port);
    stage = "create reference sender";
    sender = omt_send_create("FFmpeg parity benchmark", OMTQuality_Medium);
    if (!sender)
        goto end;

    char url[64];
    snprintf(url, sizeof(url), "omt://127.0.0.1:%d", port);
    stage = "create reference receiver";
    receiver = omt_receive_create(url, OMTFrameType_Video,
                                  OMTPreferredVideoFormat_UYVY,
                                  OMTReceiveFlags_IncludeCompressed);
    if (!receiver)
        goto end;

    state.receiver = receiver;
    state.source = frame_data;
    state.width = width;
    state.height = height;
    state.expected_frames = frames;
    stage = "start reference receiver thread";
    int thread_ret = pthread_create(&thread, NULL, receive_frames, &state);
    if (thread_ret) {
        ret = AVERROR(thread_ret);
        goto end;
    }

    stage = "connect loopback receiver";
    int64_t connect_deadline = monotonic_us() + 10000000;
    while (omt_send_connections(sender) < 1 && monotonic_us() < connect_deadline)
        nanosleep(&(struct timespec){ .tv_nsec = 10000000 }, NULL);
    if (omt_send_connections(sender) < 1) {
        atomic_store(&state.error, 1);
        atomic_store(&state.stop, 1);
        pthread_join(thread, NULL);
        goto end;
    }

    frame.Type = OMTFrameType_Video;
    frame.Codec = OMTCodec_YUY2;
    frame.Width = width;
    frame.Height = height;
    frame.Stride = width * 2;
    frame.FrameRateN = 60;
    frame.FrameRateD = 1;
    frame.AspectRatio = (float)width / height;
    frame.Data = frame_data;
    frame.DataLength = frame_size;

    start = monotonic_us();
    stage = "send loopback frames";
    for (int i = 0; i < frames; i++) {
        frame.Timestamp = (int64_t)i * 10000000 / 60;
        if (omt_send(sender, &frame) <= 0) {
            atomic_store(&state.error, 1);
            atomic_store(&state.stop, 1);
            break;
        }
    }
    pthread_join(thread, NULL);
    elapsed = monotonic_us() - start;

    stage = "validate received frame count";
    if (atomic_load(&state.error) || atomic_load(&state.received_frames) != frames)
        goto end;

    double samples = (double)width * height * 2 * frames;
    double psnr = state.squared_error ?
        10.0 * log10(65025.0 * samples / state.squared_error) : INFINITY;
        printf("omt,libomt,end_to_end,%d,%d,%d,%.6f,%.2f,%" PRIuFAST64 ",%.3f\n",
           width, height, frames, elapsed / 1000000.0,
            frames * 1000000.0 / (double)(elapsed ? elapsed : 1),
           atomic_load(&state.compressed_bytes), psnr);
    ret = 0;

end:
    if (ret)
        fprintf(stderr, "libomt benchmark failed during %s (%d/%d frames received)\n",
                stage, atomic_load(&state.received_frames), frames);
    if (receiver)
        omt_receive_destroy(receiver);
    if (sender)
        omt_send_destroy(sender);
    if (file)
        fclose(file);
    free(frame_data);
    return ret;
}

int main(int argc, char **argv)
{
    const char *input;
    int width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT, frames = DEFAULT_FRAMES;
    int port = OMT_PORT_MIN + (getpid() % OMT_PORT_RANGE);

    if (argc == 5 && !strcmp(argv[1], "--generate")) {
        const char *path = argv[2];
        int width = atoi(argv[3]), height = atoi(argv[4]);
        FILE *file;
        uint8_t *data;
        size_t size;

        if (width < 16 || height < 16 || (width & 15) || (height & 15) ||
            (size_t)width > SIZE_MAX / (size_t)height / 2)
            return 2;
        size = (size_t)width * height * 2;
        data = malloc(size);
        file = fopen(path, "wb");
        if (!data || !file) {
            free(data);
            if (file)
                fclose(file);
            return 1;
        }
        for (int y = 0; y < height; y++) {
            uint8_t *row = data + (size_t)y * width * 2;
            for (int x = 0; x < width; x += 2) {
                row[x * 2]     = 16 + (x % 220);
                row[x * 2 + 1] = 96 + ((x / 2 + y) % 64);
                row[x * 2 + 2] = 16 + ((x + y) % 220);
                row[x * 2 + 3] = 160 - ((x / 2 + y) % 64);
            }
        }
        int written = fwrite(data, 1, size, file) == size;
        free(data);
        fclose(file);
        return written ? 0 : 1;
    }
    if (argc < 2 || argc > 7) {
        fprintf(stderr, "usage: %s input.yuyv [width height frames port]\n"
                        "       %s --native input.yuyv width height frames port\n"
                        "       %s --reference input.yuyv width height frames port\n"
                        "       %s --generate output.yuyv width height\n",
                argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }
    if ((!strcmp(argv[1], "--native") || !strcmp(argv[1], "--reference")) && argc == 7) {
        input = argv[2];
        width = atoi(argv[3]);
        height = atoi(argv[4]);
        frames = atoi(argv[5]);
        port = atoi(argv[6]);
        puts("domain,implementation,operation,width,height,frames,seconds,fps,bytes,psnr_db");
        if (!strcmp(argv[1], "--native"))
            return run_native_benchmark(input, width, height, frames, port) < 0;
        return run_benchmark(input, width, height, frames, port);
    }
    input = argv[1];
    if (argc > 2) width = atoi(argv[2]);
    if (argc > 3) height = atoi(argv[3]);
    if (argc > 4) frames = atoi(argv[4]);
    if (argc > 5) port = atoi(argv[5]);

    puts("domain,implementation,operation,width,height,frames,seconds,fps,bytes");
    return run_benchmark(input, width, height, frames, port);
}