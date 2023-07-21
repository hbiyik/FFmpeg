/*
 * RockChip MPP Video Codec
 * Copyright (c) 2023 Huseyin BIYIK
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
#include <fcntl.h>
#include <time.h>
#include "rkmpp.h"

static rkformat rkformats[13] = {
        { .av = AV_PIX_FMT_YUV420P, .mpp = MPP_FMT_YUV420P,        .drm = DRM_FORMAT_YUV420,   .rga = RK_FORMAT_YCbCr_420_P},
        { .av = AV_PIX_FMT_YUV422P, .mpp = MPP_FMT_YUV422P,        .drm = DRM_FORMAT_YUV422,   .rga = RK_FORMAT_YCbCr_422_P},
        { .av = AV_PIX_FMT_NV12,    .mpp = MPP_FMT_YUV420SP,       .drm = DRM_FORMAT_NV12,     .rga = RK_FORMAT_YCbCr_420_SP},
        { .av = AV_PIX_FMT_NV16,    .mpp = MPP_FMT_YUV422SP,       .drm = DRM_FORMAT_NV16,     .rga = RK_FORMAT_YCbCr_422_SP},
        { .av = AV_PIX_FMT_NONE,    .mpp = MPP_FMT_YUV420SP_10BIT, .drm = DRM_FORMAT_NV15,     .rga = RK_FORMAT_YCbCr_420_SP_10B},
        { .av = AV_PIX_FMT_BGR24,   .mpp = MPP_FMT_BGR888,         .drm = DRM_FORMAT_RGB888,   .rga = RK_FORMAT_BGR_888},
        { .av = AV_PIX_FMT_BGR0,    .mpp = MPP_FMT_BGRA8888,       .drm = DRM_FORMAT_XRGB8888, .rga = RK_FORMAT_BGRX_8888},
        { .av = AV_PIX_FMT_BGRA,    .mpp = MPP_FMT_BGRA8888,       .drm = DRM_FORMAT_ARGB8888, .rga = RK_FORMAT_BGRA_8888},
        { .av = AV_PIX_FMT_BGR565,  .mpp = MPP_FMT_BGR565,         .drm = DRM_FORMAT_RGB565,   .rga = RK_FORMAT_BGR_565},
        { .av = AV_PIX_FMT_YUYV422, .mpp = MPP_FMT_YUV422_YUYV,    .drm = DRM_FORMAT_YUYV,     .rga = RK_FORMAT_YUYV_422},
        { .av = AV_PIX_FMT_UYVY422, .mpp = MPP_FMT_YUV422_UYVY,    .drm = DRM_FORMAT_UYVY,     .rga = RK_FORMAT_UYVY_422},
        { .av = AV_PIX_FMT_NV24,    .mpp = MPP_FMT_YUV444SP,       .drm = DRM_FORMAT_NV24,     .rga = RK_FORMAT_UNKNOWN},
        { .av = AV_PIX_FMT_YUV444P, .mpp = MPP_FMT_YUV444P,        .drm = DRM_FORMAT_YUV444,   .rga = RK_FORMAT_UNKNOWN},
};

#define GETFORMAT(NAME, TYPE)\
int rkmpp_get_##NAME##_format(rkformat *format, TYPE informat){ \
    for(int i=0; i < 13; i++){ \
        if(rkformats[i].NAME == informat){ \
            format->av = rkformats[i].av;\
            format->mpp = rkformats[i].mpp;\
            format->drm = rkformats[i].drm;\
            format->rga = rkformats[i].rga;\
            return 0;\
        }\
    }\
    return -1;\
}

GETFORMAT(drm, uint32_t)
GETFORMAT(mpp, MppFrameFormat)
GETFORMAT(rga, enum _Rga_SURF_FORMAT)
GETFORMAT(av, enum AVPixelFormat)

MppCodingType rkmpp_get_codingtype(AVCodecContext *avctx)
{
    switch (avctx->codec_id) {
    case AV_CODEC_ID_H263:          return MPP_VIDEO_CodingH263;
    case AV_CODEC_ID_H264:          return MPP_VIDEO_CodingAVC;
    case AV_CODEC_ID_HEVC:          return MPP_VIDEO_CodingHEVC;
    case AV_CODEC_ID_AV1:           return MPP_VIDEO_CodingAV1;
    case AV_CODEC_ID_VP8:           return MPP_VIDEO_CodingVP8;
    case AV_CODEC_ID_VP9:           return MPP_VIDEO_CodingVP9;
    case AV_CODEC_ID_MPEG1VIDEO:    /* fallthrough */
    case AV_CODEC_ID_MPEG2VIDEO:    return MPP_VIDEO_CodingMPEG2;
    case AV_CODEC_ID_MPEG4:         return MPP_VIDEO_CodingMPEG4;
    default:                        return MPP_VIDEO_CodingUnused;
    }
}

int rkmpp_close_codec(AVCodecContext *avctx)
{
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;

    av_packet_unref(&codec->lastpacket);
    av_frame_unref(&codec->lastframe);

    av_buffer_unref(&rk_context->codec_ref);
    return 0;
}

void rkmpp_release_codec(void *opaque, uint8_t *data)
{
    RKMPPCodec *codec = (RKMPPCodec *)data;

    if (codec->mpi) {
        codec->mpi->reset(codec->ctx);
        mpp_destroy(codec->ctx);
        codec->ctx = NULL;
    }

    if (codec->buffer_group) {
        mpp_buffer_group_put(codec->buffer_group);
        codec->buffer_group = NULL;
    }

    if (codec->rga_fd) {
        close(codec->rga_fd);
        codec->rga_fd = 0;
    }

    if(codec->hwframes_ref)
        av_buffer_unref(&codec->hwframes_ref);
    if(codec->hwdevice_ref)
        av_buffer_unref(&codec->hwdevice_ref);

    av_free(codec);
}

int rkmpp_init_codec(AVCodecContext *avctx)
{
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = NULL;
    MppCodingType codectype = MPP_VIDEO_CodingUnused;
    char *env;
    int ret;

    // create a codec and a ref to it
    codec = av_mallocz(sizeof(RKMPPCodec));
    if (!codec) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    rk_context->codec_ref = av_buffer_create((uint8_t *)codec, sizeof(*codec), rkmpp_release_codec,
                                               NULL, AV_BUFFER_FLAG_READONLY);
    if (!rk_context->codec_ref) {
        av_free(codec);
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    env = getenv("FFMPEG_RKMPP_LOG_FPS");
    if (env != NULL)
        codec->print_fps = !!atoi(env);

    av_log(avctx, AV_LOG_DEBUG, "Initializing RKMPP Codec.\n");

    codectype = rkmpp_get_codingtype(avctx);
    if (codectype == MPP_VIDEO_CodingUnused) {
        av_log(avctx, AV_LOG_ERROR, "Unknown codec type (%d).\n", avctx->codec_id);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    ret = mpp_check_support_format(codec->mppctxtype, codectype);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Codec type (%d) unsupported by MPP\n", avctx->codec_id);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    // Create the MPP context
    ret = mpp_create(&codec->ctx, &codec->mpi);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create MPP context (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    if(ffcodec(avctx->codec)->cb_type == FF_CODEC_CB_TYPE_RECEIVE_FRAME){
        codec->init_callback = rkmpp_init_decoder;
        codec->mppctxtype = MPP_CTX_DEC;

        ret = 1;
        codec->mpi->control(codec->ctx, MPP_DEC_SET_PARSER_FAST_MODE, &ret);

        avctx->pix_fmt = ff_get_format(avctx, avctx->codec->pix_fmts);

        // override the the pixfmt according env variable
        env = getenv("FFMPEG_RKMPP_PIXFMT");
        if(env != NULL){
            if(!strcmp(env, "YUV420P"))
                avctx->pix_fmt = AV_PIX_FMT_YUV420P;
            else if (!strcmp(env, "NV12"))
                avctx->pix_fmt = AV_PIX_FMT_NV12;
            else if(!strcmp(env, "DRMPRIME"))
                avctx->pix_fmt = AV_PIX_FMT_DRM_PRIME;
        }
    } else if ((ffcodec(avctx->codec)->cb_type == FF_CODEC_CB_TYPE_ENCODE)){
        codec->mppctxtype = MPP_CTX_ENC;
        codec->init_callback = rkmpp_init_encoder;
    } else {
        ret = AVERROR(ENOMEM);
        av_log(avctx, AV_LOG_DEBUG, "RKMPP Codec can not determine if the mode is decoder or encoder\n");
        goto fail;
    }

    av_log(avctx, AV_LOG_INFO, "Picture format is %s.\n", av_get_pix_fmt_name(avctx->pix_fmt));

    // initialize mpp
    ret = mpp_init(codec->ctx, codec->mppctxtype, codectype);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to initialize MPP context (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    env = getenv("FFMPEG_RKMPP_NORGA");
    if(env != NULL){
        codec->norga = 1;
        codec->rga_fd = -1;
        av_log(avctx, AV_LOG_INFO, "Bypassing RGA and using libyuv soft conversion\n");
    }

    if (!codec->norga){
        codec->rga_fd = open("/dev/rga", O_RDWR);
        if (codec->rga_fd < 0) {
           av_log(avctx, AV_LOG_WARNING, "Failed to open RGA, Falling back to libyuv\n");
        }
    }

    ret = mpp_buffer_group_get_internal(&codec->buffer_group, MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_DMA32);
    if (ret) {
       av_log(avctx, AV_LOG_ERROR, "Failed to get buffer group (code = %d)\n", ret);
       ret = AVERROR_UNKNOWN;
       goto fail;
    }

    ret = codec->init_callback(avctx);
    if(ret){
        av_log(avctx, AV_LOG_ERROR, "Failed to init Codec (code = %d).\n", ret);
        goto fail;
    }

    return 0;

fail:
    av_log(avctx, AV_LOG_ERROR, "Failed to initialize RKMPP Codec.\n");
    rkmpp_close_codec(avctx);
    return ret;
}

void rkmpp_flush(AVCodecContext *avctx)
{
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;

    av_log(avctx, AV_LOG_DEBUG, "Flush.\n");

    codec->mpi->reset(codec->ctx);
    codec->norga = codec->last_frame_time = codec->frames = codec->hascfg = 0;

    av_packet_unref(&codec->lastpacket);
    av_frame_unref(&codec->lastframe);
}

uint64_t rkmpp_update_latency(AVCodecContext *avctx, int latency)
{
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;
    struct timespec tv;
    uint64_t curr_time;
    float fps = 0.0f;

    if (!codec->print_fps)
        return 0;

    clock_gettime(CLOCK_MONOTONIC, &tv);
    curr_time = tv.tv_sec * 10e5 + tv.tv_nsec / 10e2;
    if (latency == -1){
        latency = codec->last_frame_time ? curr_time - codec->last_frame_time : 0;
        codec->last_frame_time = curr_time;
        codec->latencies[codec->frames % RKMPP_FPS_FRAME_MACD] = latency;
        return latency;
    } else if (latency == 0 || codec->frames < RKMPP_FPS_FRAME_MACD) {
        fps = -1.0f;
    } else {
       for(int i = 0; i < RKMPP_FPS_FRAME_MACD; i++) {
          fps += codec->latencies[i];
       }
        fps = RKMPP_FPS_FRAME_MACD * 1000000.0f / fps;
    }
    av_log(avctx, AV_LOG_INFO,
           "[FFMPEG RKMPP] FPS(MACD%d): %6.1f || Frames: %" PRIu64 " || Latency: %d us || Buffer Delay %" PRIu64 "us\n",
           RKMPP_FPS_FRAME_MACD, fps, codec->frames, latency, (uint64_t)(curr_time - codec->last_frame_time));

    return 0;
}
