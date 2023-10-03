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
#ifdef _POSIX_C_SOURCE
#undef _POSIX_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L /* for O_CLOEXEC */

#include <time.h>
#include "rkmpp.h"
#include <sys/mman.h>
#include <linux/dma-heap.h>
#include <fcntl.h>

static rkformat rkformats[15] = {
        { .av = AV_PIX_FMT_BGR24, .mpp = MPP_FMT_BGR888, .drm = DRM_FORMAT_BGR888, .rga = RK_FORMAT_BGR_888,
                .numplanes = 1, .mode = MUL, .planedata = { .plane[0] = {.width = 3},}},
        { .av = AV_PIX_FMT_RGBA, .mpp = MPP_FMT_RGBA8888, .drm = DRM_FORMAT_ABGR8888, .rga = RK_FORMAT_RGBA_8888,
                .numplanes = 1, .mode = MUL, .planedata = { .plane[0] = {.width = 4},}},
        { .av = AV_PIX_FMT_RGB0, .mpp = MPP_FMT_RGBA8888, .drm = DRM_FORMAT_XBGR8888, .rga = RK_FORMAT_RGBX_8888,
                .numplanes = 1, .mode = MUL, .planedata = { .plane[0] = {.width = 4},}},
        { .av = AV_PIX_FMT_BGRA, .mpp = MPP_FMT_BGRA8888, .drm = DRM_FORMAT_ARGB8888, .rga = RK_FORMAT_BGRA_8888,
                .numplanes = 1, .mode = MUL, .planedata = { .plane[0] = {.width = 4},}},
        { .av = AV_PIX_FMT_BGR0, .mpp = MPP_FMT_BGRA8888, .drm = DRM_FORMAT_XRGB8888, .rga = RK_FORMAT_BGRX_8888,
                .numplanes = 1, .mode = MUL, .planedata = { .plane[0] = {.width = 4},}},
        { .av = AV_PIX_FMT_YUYV422, .mpp = MPP_FMT_YUV422_YUYV, .drm = DRM_FORMAT_YUYV, .rga = RK_FORMAT_YUYV_422,
                .numplanes = 1, .mode = MUL, .planedata = { .plane[0] = {.width = 2},}},
        { .av = AV_PIX_FMT_UYVY422, .mpp = MPP_FMT_YUV422_UYVY, .drm = DRM_FORMAT_UYVY, .rga = RK_FORMAT_UYVY_422,
                .numplanes = 1, .mode = MUL, .planedata = { .plane[0] = {.width = 2},}},
        { .av = AV_PIX_FMT_NV12, .mpp = MPP_FMT_YUV420SP, .drm = DRM_FORMAT_NV12, .rga = RK_FORMAT_YCbCr_420_SP,
                .numplanes = 2, .mode = SHR, .planedata = { .plane[1] = {.height = 1},}},
        { .av = AV_PIX_FMT_NV15, .mpp = MPP_FMT_YUV420SP_10BIT, .drm = DRM_FORMAT_NV15, .rga = RK_FORMAT_YCbCr_420_SP_10B,
                .numplanes = 2, .mode = SHR, .planedata = { .plane[1] = {.height = 1},}},
        { .av = AV_PIX_FMT_NV16, .mpp = MPP_FMT_YUV422SP, .drm = DRM_FORMAT_NV16, .rga = RK_FORMAT_YCbCr_422_SP,
                .numplanes = 2, .mode = SHR},
        { .av = AV_PIX_FMT_NV24, .mpp = MPP_FMT_YUV444SP, .drm = DRM_FORMAT_NV24, .rga = RK_FORMAT_UNKNOWN,
                .numplanes = 2, .mode = MUL, .planedata = { .plane[1] = {.width = 2, .hstride=2},}},
        { .av = AV_PIX_FMT_YUV420P,.mpp = MPP_FMT_YUV420P, .drm = DRM_FORMAT_YUV420, .rga = RK_FORMAT_YCbCr_420_P,
                .numplanes = 3, .mode = SHR, .planedata = { .plane[1] = {.width = 1, .height = 1, .hstride=1},}},
        { .av = AV_PIX_FMT_YUV422P, .mpp = MPP_FMT_YUV422P,.drm = DRM_FORMAT_YUV422, .rga = RK_FORMAT_YCbCr_422_P,
                .numplanes = 3, .mode = SHR, .planedata = { .plane[1] = {.hstride = 1}}},
        { .av = AV_PIX_FMT_YUV444P, .mpp = MPP_FMT_YUV444P, .drm = DRM_FORMAT_YUV444,.rga = RK_FORMAT_UNKNOWN,
                .numplanes = 3, .mode = SHR},
        { .av = AV_PIX_FMT_BGR565, .mpp = MPP_FMT_BGR565, .drm = DRM_FORMAT_BGR565, .rga = RK_FORMAT_BGR_565},
};

#define GETFORMAT(NAME, TYPE)\
int rkmpp_get_##NAME##_format(rkformat *format, TYPE informat){ \
    for(int i=0; i < FF_ARRAY_ELEMS(rkformats); i++){ \
        if(rkformats[i].NAME == informat){ \
            format->av = rkformats[i].av;\
            format->mpp = rkformats[i].mpp;\
            format->drm = rkformats[i].drm;\
            format->rga = rkformats[i].rga;\
            format->numplanes = rkformats[i].numplanes;\
            format->mode = rkformats[i].mode;\
            format->planedata = rkformats[i].planedata;\
            return 0;\
        }\
    }\
    return -1;\
}

GETFORMAT(drm, uint32_t)
GETFORMAT(mpp, MppFrameFormat)
GETFORMAT(rga, enum _Rga_SURF_FORMAT)
GETFORMAT(av, enum AVPixelFormat)

int rkmpp_planedata(rkformat *format, planedata *planes, int width, int height, int align){
    int hstride, size, totalsize = 0;

    planes->avformat = format->av;
    planes->vstride = FFALIGN(height, align);;
    planes->width = width;
    planes->height = height;
    planes->hstride = format->numplanes == 1 ? FFALIGN(width * format->planedata.plane[0].width, align) :  FFALIGN(width, align);
    hstride = planes->hstride;
    size = hstride * planes->vstride;

    for(int i=0; i<format->numplanes; i++){

        if(format->mode == SHR){
            if(format->planedata.plane[i].width)
                width = width >> format->planedata.plane[i].width;
            if(format->planedata.plane[i].height){
                height = height >> format->planedata.plane[i].height;
                size = size >> format->planedata.plane[i].height;
            }
            if(format->planedata.plane[i].hstride){
                hstride = hstride >> format->planedata.plane[i].hstride;
                size = size >> format->planedata.plane[i].hstride;
            }
        } else if(format->mode == MUL){
            if(format->planedata.plane[i].width)
                width = width * format->planedata.plane[i].width;
            if(format->planedata.plane[i].height){
                height = height * format->planedata.plane[i].height;
                size = size * format->planedata.plane[i].height;
            }
            if(format->planedata.plane[i].hstride){
                hstride = hstride * format->planedata.plane[i].hstride;
                size = size * format->planedata.plane[i].hstride;
            }
        }
        planes->plane[i].width = width;
        planes->plane[i].height = height;
        planes->plane[i].hstride = hstride;
        planes->plane[i].size =  size;
        planes->plane[i].offset = totalsize;
        totalsize += planes->plane[i].size;
    }

    planes->size = totalsize;
    return 0;
}

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

    if (codec->buffer_group)
        mpp_buffer_group_clear(codec->buffer_group);

    if (codec->buffer_group_rga)
        mpp_buffer_group_clear(codec->buffer_group_rga);

    if (codec->mpi) {
        codec->mpi->reset(codec->ctx);
        mpp_destroy(codec->ctx);
        codec->ctx = NULL;
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
    int width = avctx->width;
    int height = avctx->height;

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

    codec->dma_fd = open("/dev/dma_heap/system-dma32", O_RDWR);
    if (codec->dma_fd < 0) {
       av_log(avctx, AV_LOG_ERROR, "Failed to open system-dma32 heap\n");
       ret = AVERROR_UNKNOWN;
       goto fail;
    }

    if(ffcodec(avctx->codec)->cb_type == FF_CODEC_CB_TYPE_RECEIVE_FRAME){
        codec->init_callback = rkmpp_init_decoder;
        codec->mppctxtype = MPP_CTX_DEC;

        ret = 1;
        codec->mpi->control(codec->ctx, MPP_DEC_SET_PARSER_FAST_MODE, &ret);
    } else if ((ffcodec(avctx->codec)->cb_type == FF_CODEC_CB_TYPE_ENCODE)){
        codec->mppctxtype = MPP_CTX_ENC;
        codec->init_callback = rkmpp_init_encoder;
    } else {
        ret = AVERROR(ENOMEM);
        av_log(avctx, AV_LOG_DEBUG, "RKMPP Codec can not determine if the mode is decoder or encoder\n");
        goto fail;
    }

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
        av_log(avctx, AV_LOG_INFO, "Bypassing RGA and using libyuv soft conversion\n");
    }

    //nv12 format calculations are necessary for for NV15->NV12 conversion
    rkmpp_get_av_format(&rk_context->nv12format, AV_PIX_FMT_NV12);
    rkmpp_planedata(&rk_context->nv12format, &rk_context->nv12planes, width, height, RKMPP_STRIDE_ALIGN);

    ret = codec->init_callback(avctx);

    if(ret){
        av_log(avctx, AV_LOG_ERROR, "Failed to init Codec (code = %d).\n", ret);
        goto fail;
    }

    // when the pixfmt is drmprime,
    // decoder: we rely on mpp decoder to detect the actual frame format when first frame decoded
    // encoder: we rely on fist avframe received to encoder
    // normally, avctx should have actual frame format but due to missing implementation of other
    // devices/encoders/decoders, we dont rely on them
    if(!rkmpp_get_av_format(&rk_context->rkformat, avctx->pix_fmt))
        rkmpp_planedata(&rk_context->rkformat, &rk_context->avplanes, width, height, RKMPP_STRIDE_ALIGN);
    else if (avctx->pix_fmt != AV_PIX_FMT_DRM_PRIME) {
        av_log(avctx, AV_LOG_ERROR, "Unknown Picture format %s.\n", av_get_pix_fmt_name(avctx->pix_fmt)); // most likely never branches here
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    av_log(avctx, AV_LOG_INFO, "Picture format is %s.\n", av_get_pix_fmt_name(avctx->pix_fmt));

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
    codec->last_frame_time = codec->frames = codec->hascfg = 0;

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

void rkmpp_buffer_free(MppBufferInfo *dma_info)
{
    if (!dma_info)
        return;

    munmap(dma_info->ptr, dma_info->size);
    close(dma_info->fd);
    dma_info->index = 0;
}

MPP_RET rkmpp_buffer_set(AVCodecContext *avctx, size_t size, MppBufferGroup *buffer_group, int count)
{
    MPP_RET ret=MPP_SUCCESS;
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;

    if (*buffer_group) {
        if ((ret = mpp_buffer_group_clear(*buffer_group)) != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to clear external buffer group: %d\n", ret);
            return ret;
        }
    }

    ret = mpp_buffer_group_get_external(buffer_group, MPP_BUFFER_TYPE_DMA_HEAP);
    if (ret) {
       av_log(avctx, AV_LOG_ERROR, "Failed to get buffer group (code = %d)\n", ret);
       return ret;
    }

    for (int i=0; i < count; i++){
        struct dma_heap_allocation_data alloc = {
               .len = size,
               .fd_flags = O_CLOEXEC | O_RDWR,
           };

        MppBufferInfo buf_info = {
            .index = i,
            .type  = MPP_BUFFER_TYPE_DMA_HEAP,
            .size  = alloc.len,
        };

        if (ioctl(codec->dma_fd, DMA_HEAP_IOCTL_ALLOC, &alloc) == -1)
            return MPP_ERR_MALLOC;

        buf_info.fd = alloc.fd;
        buf_info.ptr = mmap(NULL, alloc.len, PROT_READ | PROT_WRITE, MAP_SHARED, alloc.fd, 0),
        ret = mpp_buffer_commit(*buffer_group, &buf_info);
        if (ret) {
            av_log(avctx, AV_LOG_ERROR, "Failed to commit external buffer group: %d\n", ret);
            return ret;
        }
    }

    return MPP_SUCCESS;
}

