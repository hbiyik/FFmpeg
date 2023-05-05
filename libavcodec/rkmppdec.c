/*
 * RockChip MPP Video Decoder
 * Copyright (c) 2017 Lionel CHAZALLON
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

#include <drm_fourcc.h>
#include <pthread.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/rk_mpi.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <fcntl.h>

#include "avcodec.h"
#include "codec_internal.h"
#include "internal.h"
#include "decode.h"
#include "hwconfig.h"
#include "libavutil/buffer.h"
#include "libavutil/common.h"
#include "libavutil/frame.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_drm.h"
#include "libavutil/imgutils.h"
#include "libavutil/log.h"
#include "libyuv/planar_functions.h"
#include "libyuv/scale_uv.h"
#include "libyuv/scale.h"
#include "rga.h"

// HACK: Older BSP kernel use NA12 for NV15.
#ifndef DRM_FORMAT_NV15 // fourcc_code('N', 'V', '1', '5')
#define DRM_FORMAT_NV15 fourcc_code('N', 'A', '1', '2')
#endif

#define FPS_FRAME_MACD 30

typedef struct {
    MppCtx ctx;
    MppApi *mpi;
    MppBufferGroup frame_group;

    AVPacket packet;
    AVBufferRef *frames_ref;
    AVBufferRef *device_ref;

    char print_fps;

    uint64_t last_frame_time;
    uint64_t frames;
    uint64_t latencies[FPS_FRAME_MACD];

    uint32_t mpp_format;
    uint32_t rga_informat;
    uint32_t rga_outformat;
    uint32_t drm_format;
    uint32_t sw_format;
    int rga_fd;
    int8_t norga;
    int (*buffer_callback)(struct AVCodecContext *avctx, struct AVFrame *frame, MppFrame mppframe);

} RKMPPDecoder;

typedef struct {
    AVClass *av_class;
    AVBufferRef *decoder_ref;
} RKMPPDecodeContext;

typedef struct {
    MppFrame frame;
    AVBufferRef *decoder_ref;
} RKMPPFrameContext;

static MppCodingType rkmpp_get_codingtype(AVCodecContext *avctx)
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

static int rkmpp_close_decoder(AVCodecContext *avctx)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;

    av_packet_unref(&decoder->packet);

    av_buffer_unref(&rk_context->decoder_ref);
    return 0;
}

static void rkmpp_release_decoder(void *opaque, uint8_t *data)
{
    RKMPPDecoder *decoder = (RKMPPDecoder *)data;

    if (decoder->mpi) {
        decoder->mpi->reset(decoder->ctx);
        mpp_destroy(decoder->ctx);
        decoder->ctx = NULL;
    }

    if (decoder->frame_group) {
        mpp_buffer_group_put(decoder->frame_group);
        decoder->frame_group = NULL;
    }

    if (decoder->rga_fd) {
        close(decoder->rga_fd);
        decoder->rga_fd = 0;
    }

    av_buffer_unref(&decoder->frames_ref);
    av_buffer_unref(&decoder->device_ref);

    av_free(decoder);
}

static void rkmpp_release_drmbuf(void *opaque, uint8_t *data)
{
    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)data;
    AVBufferRef *framecontextref = (AVBufferRef *)opaque;
    RKMPPFrameContext *framecontext = (RKMPPFrameContext *)framecontextref->data;

    mpp_frame_deinit(&framecontext->frame);
    av_buffer_unref(&framecontext->decoder_ref);
    av_buffer_unref(&framecontextref);

    av_free(desc);
}

static void rkmpp_release_buf(void *opaque, uint8_t *data)
{
    MppFrame mppframe = opaque;
    mpp_frame_deinit(&mppframe);
}

static int rkmpp_set_nv12_buf(AVCodecContext *avctx, AVFrame *frame, MppFrame mppframe)
{
    MppBuffer buffer = mpp_frame_get_buffer(mppframe);
    int hstride = mpp_frame_get_hor_stride(mppframe);
    int vstride = mpp_frame_get_ver_stride(mppframe);

    frame->data[0] = mpp_buffer_get_ptr(buffer); // y
    frame->data[1] = frame->data[0] + hstride * vstride; // u + v
    frame->extended_data = frame->data;

       frame->linesize[0] = hstride;
       frame->linesize[1] = hstride;

       frame->buf[0] = av_buffer_create(frame->data[0], mpp_buffer_get_size(buffer),
            rkmpp_release_buf, mppframe,
            AV_BUFFER_FLAG_READONLY);
    if (!frame->buf[0]) {
        return AVERROR(ENOMEM);
    }

    return 0;
}

static int rkmpp_rga_convert_buf(AVCodecContext *avctx, AVFrame *frame, MppFrame mppframe)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;

    MppBuffer buffer = mpp_frame_get_buffer(mppframe);
    char *src = mpp_buffer_get_ptr(buffer);
    int width = mpp_frame_get_width(mppframe);
    int height = mpp_frame_get_height(mppframe);
    int hstride = mpp_frame_get_hor_stride(mppframe);
    int vstride = mpp_frame_get_ver_stride(mppframe);
    int ret;

    ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0)
        return ret;

    if (!decoder->norga && decoder->rga_fd >= 0){
        struct rga_req req = {
            .src = {
                .yrgb_addr = mpp_buffer_get_fd(buffer),
                .v_addr = hstride * vstride,
                .format = decoder->rga_informat,
                .act_w = width,
                .act_h = height,
                .vir_w = hstride,
                .vir_h = vstride,
                .rd_mode = RGA_RASTER_MODE,
            },
            .dst = {
                .uv_addr = (uintptr_t) frame->data[0],
                .v_addr = (uintptr_t) frame->data[1],
                .format = decoder->rga_outformat,
                .act_w = width,
                .act_h = height,
                .vir_w = frame->linesize[0],
                .vir_h = (frame->data[1] - frame->data[0]) / frame->linesize[0],
                .rd_mode = RGA_RASTER_MODE,
            },
            .mmu_info = {
                .mmu_en = 1,
                .mmu_flag = 0x80000521,
            },
        };

        ret = ioctl(decoder->rga_fd, RGA_BLIT_SYNC, &req);
        if (ret < 0){
            decoder->norga = 1;
            av_log(avctx, AV_LOG_WARNING, "RGA failed with code %d, falling back to soft conversion\n", ret);
        } else {
            rkmpp_release_buf(mppframe, NULL);
            return 0;
        }
    }

    if ((decoder->norga || decoder->rga_fd < 0) && decoder->rga_outformat == RGA_FORMAT_YCbCr_420_P){
        //data[0] points to buf[1] where the mppbuffer is referenced for y plane
        //so that we can still use y plane without extra copies
        //data[1,2] points to allready allocated AVBuffer Pool (buf[0]), we will convert to
        //that buffer only u+v planes, which is half the size operation
        frame->data[0] = mpp_buffer_get_ptr(buffer);
        frame->buf[1] = av_buffer_create(frame->data[0], mpp_buffer_get_size(buffer),
                rkmpp_release_buf, mppframe,
                AV_BUFFER_FLAG_READONLY);
        if (!frame->buf[1]) {
            return AVERROR(ENOMEM);
        }
        frame->linesize[0] = hstride;

        src += hstride * vstride;
        if(decoder->rga_informat == RGA_FORMAT_YCbCr_422_SP){
            /* In case the input format has 4:2:2 UV planes, it will have double the size of 4:2:0 UV Planes
             * Therefore we scale them to the half the size to the unused FFbuffer's Y Plane (We are using MPP 's Y)
             * Then we convert to Planar in the next step. Normally it should be possible to this in 1 step
             * But i can not find a way to do it in 1 step using libyuv. But thats fine enough
             */
            UVScale(src, hstride, frame->width, frame->height,
                    frame->buf[0]->data, hstride,
                    (frame->width + 1) >> 1, (frame->height + 1) >> 1, kFilterNone);
            src = frame->buf[0]->data;
        }
        SplitUVPlane(src, hstride, frame->data[1], frame->linesize[1], frame->data[2], frame->linesize[2],
                (frame->width + 1) >> 1, (frame->height + 1) >> 1);
        return 0;
    }

    return AVERROR_UNKNOWN;
}

static int rkmpp_init_decoder(AVCodecContext *avctx)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = NULL;
    MppCodingType codectype = MPP_VIDEO_CodingUnused;
    char *env;
    int ret;

    // create a decoder and a ref to it
    decoder = av_mallocz(sizeof(RKMPPDecoder));
    if (!decoder) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    env = getenv("FFMPEG_RKMPP_LOG_FPS");
    if (env != NULL)
        decoder->print_fps = !!atoi(env);

    rk_context->decoder_ref = av_buffer_create((uint8_t *)decoder, sizeof(*decoder), rkmpp_release_decoder,
                                               NULL, AV_BUFFER_FLAG_READONLY);
    if (!rk_context->decoder_ref) {
        av_free(decoder);
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    av_log(avctx, AV_LOG_DEBUG, "Initializing RKMPP decoder.\n");

    codectype = rkmpp_get_codingtype(avctx);
    if (codectype == MPP_VIDEO_CodingUnused) {
        av_log(avctx, AV_LOG_ERROR, "Unknown codec type (%d).\n", avctx->codec_id);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    ret = mpp_check_support_format(MPP_CTX_DEC, codectype);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Codec type (%d) unsupported by MPP\n", avctx->codec_id);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    // Create the MPP context
    ret = mpp_create(&decoder->ctx, &decoder->mpi);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create MPP context (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    ret = 1;
    decoder->mpi->control(decoder->ctx, MPP_DEC_SET_PARSER_FAST_MODE, &ret);

    // initialize mpp
    ret = mpp_init(decoder->ctx, MPP_CTX_DEC, codectype);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to initialize MPP context (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    ret = mpp_buffer_group_get_internal(&decoder->frame_group, MPP_BUFFER_TYPE_ION);
    if (ret) {
       av_log(avctx, AV_LOG_ERROR, "Failed to get buffer group (code = %d)\n", ret);
       ret = AVERROR_UNKNOWN;
       goto fail;
    }

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
        else if(!strcmp(env, "YUV420PSOFT")){
               avctx->pix_fmt = AV_PIX_FMT_YUV420P;
               decoder->norga = 1;
        }
    }

    if (decoder->norga)
        decoder->rga_fd = -1;
    else {
        decoder->rga_fd = open("/dev/rga", O_RDWR);
        if (decoder->rga_fd < 0) {
           av_log(avctx, AV_LOG_WARNING, "Failed to open RGA, Falling back to libyuv\n");
        }
    }

    ret = decoder->mpi->control(decoder->ctx, MPP_DEC_SET_EXT_BUF_GROUP, decoder->frame_group);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "Failed to assign buffer group (code = %d)\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    decoder->mpi->control(decoder->ctx, MPP_DEC_SET_DISABLE_ERROR, NULL);

    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to prepare decoder (code = %d)\n", ret);
        goto fail;
    }

    av_log(avctx, AV_LOG_DEBUG, "RKMPP decoder initialized successfully.\n");

    decoder->device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_DRM);
    if (!decoder->device_ref) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ret = av_hwdevice_ctx_init(decoder->device_ref);
    if (ret < 0)
        goto fail;

    return 0;

fail:
    av_log(avctx, AV_LOG_ERROR, "Failed to initialize RKMPP decoder.\n");
    rkmpp_close_decoder(avctx);
    return ret;
}

static uint64_t rkmpp_update_latency(AVCodecContext *avctx, uint64_t latency)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;
    struct timespec tv;
    uint64_t curr_time;
    float fps = 0.0f;

    if (!decoder->print_fps)
        return 0;

    clock_gettime(CLOCK_MONOTONIC, &tv);
    curr_time = tv.tv_sec * 10e5 + tv.tv_nsec / 10e2;
    if (latency == -1){
        latency = decoder->last_frame_time ? curr_time - decoder->last_frame_time : 0;
        decoder->last_frame_time = curr_time;
        decoder->latencies[++decoder->frames % FPS_FRAME_MACD] = latency;
        return latency;
    } else if (latency == 0 || decoder->frames < FPS_FRAME_MACD) {
        fps = -1.0f;
    } else {
       for(int i = 0; i < FPS_FRAME_MACD; i++) {
          fps += decoder->latencies[i];
       }
        fps = FPS_FRAME_MACD * 1000000.0f / fps;
    }
    av_log(avctx, AV_LOG_INFO,
           "[FFMPEG RKMPP] FPS(MACD%d): %6.1f || Frames: %" PRIu64 " || Latency: %" PRIu64 "us || Buffer Delay %" PRIu64 "us\n",
           FPS_FRAME_MACD, fps, decoder->frames, latency, (uint64_t)(curr_time - decoder->last_frame_time));

    return 0;
}

static int rkmpp_set_drm_buf(AVCodecContext *avctx, AVFrame *frame, MppFrame mppframe)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;
    RKMPPFrameContext *framecontext = NULL;
    AVBufferRef *framecontextref = NULL;
    AVDRMFrameDescriptor *desc = NULL;
    AVDRMLayerDescriptor *layer = NULL;
    MppBuffer buffer = mpp_frame_get_buffer(mppframe);
    int ret;

    desc = av_mallocz(sizeof(AVDRMFrameDescriptor));
    if (!desc) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    desc->nb_objects = 1;
    desc->objects[0].fd = mpp_buffer_get_fd(buffer);
    desc->objects[0].size = mpp_buffer_get_size(buffer);

    desc->nb_layers = 1;
    layer = &desc->layers[0];
    layer->format = decoder->drm_format;
    layer->nb_planes = 2;

    layer->planes[0].object_index = 0;
    layer->planes[0].offset = 0;
    layer->planes[0].pitch = mpp_frame_get_hor_stride(mppframe);

    layer->planes[1].object_index = 0;
    layer->planes[1].offset = layer->planes[0].pitch * mpp_frame_get_ver_stride(mppframe);
    layer->planes[1].pitch = layer->planes[0].pitch;

    // we also allocate a struct in buf[0] that will allow to hold additionnal information
    // for releasing properly MPP frames and decoder
    framecontextref = av_buffer_allocz(sizeof(*framecontext));
    if (!framecontextref) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    // MPP decoder needs to be closed only when all frames have been released.
    framecontext = (RKMPPFrameContext *)framecontextref->data;
    framecontext->decoder_ref = av_buffer_ref(rk_context->decoder_ref);
    framecontext->frame = mppframe;

    frame->data[0]  = (uint8_t *)desc;
    frame->buf[0]   = av_buffer_create((uint8_t *)desc, sizeof(*desc), rkmpp_release_drmbuf,
                                       framecontextref, AV_BUFFER_FLAG_READONLY);

    if (!frame->buf[0]) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    frame->hw_frames_ctx = av_buffer_ref(decoder->frames_ref);
    if (!frame->hw_frames_ctx) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    return 0;

fail:
    if (framecontext)
        av_buffer_unref(&framecontext->decoder_ref);

    if (framecontextref)
        av_buffer_unref(&framecontextref);

    if (desc)
        av_free(desc);

    return ret;
}

static int set_buffer_callback(RKMPPDecoder *decoder, AVCodecContext *avctx){
    if (avctx->pix_fmt == AV_PIX_FMT_DRM_PRIME){
        decoder->buffer_callback = rkmpp_set_drm_buf;
        switch(decoder->mpp_format){
        case MPP_FMT_YUV420SP_10BIT:
            decoder->drm_format = DRM_FORMAT_NV15;
            decoder->sw_format = AV_PIX_FMT_NONE;
            av_log(avctx, AV_LOG_INFO, "Decoder is set to use DRMPrime with NV15.\n");
            return 0;
        case MPP_FMT_YUV420SP:
            decoder->drm_format = DRM_FORMAT_NV12;
            decoder->sw_format = AV_PIX_FMT_NV12;
            av_log(avctx, AV_LOG_INFO, "Decoder is set to use DRMPrime with NV12.\n");
            return 0;
        case MPP_FMT_YUV422SP:
            decoder->drm_format = DRM_FORMAT_NV16;
            decoder->sw_format = AV_PIX_FMT_NV16;
            av_log(avctx, AV_LOG_INFO, "Decoder is set to use DRMPrime with NV16.\n");
            return 0;
        }
    } else if(avctx->pix_fmt == AV_PIX_FMT_NV12){
        decoder->rga_outformat = RGA_FORMAT_YCbCr_420_SP;
        switch(decoder->mpp_format){
        case MPP_FMT_YUV420SP_10BIT:
            decoder->rga_informat = RGA_FORMAT_YCbCr_420_SP_10B;
            decoder->buffer_callback = rkmpp_rga_convert_buf;
            av_log(avctx, AV_LOG_INFO, "Decoder is set to use AVBuffer with NV15->NV12 conversion through RGA3.\n");
            return 0;
        case MPP_FMT_YUV420SP:
            decoder->buffer_callback = rkmpp_set_nv12_buf;
            av_log(avctx, AV_LOG_INFO, "Decoder is set to use MppBuffer with NV12.\n");
            return 0;
        case MPP_FMT_YUV422SP:
            decoder->rga_informat = RGA_FORMAT_YCbCr_422_SP;
            decoder->buffer_callback = rkmpp_rga_convert_buf;
            av_log(avctx, AV_LOG_INFO, "Decoder is set to use AVBuffer with NV16->NV12 conversion through RGA3.\n");
            return 0;
        }
    } else if (avctx->pix_fmt == AV_PIX_FMT_YUV420P){
        decoder->rga_outformat = RGA_FORMAT_YCbCr_420_P;
        switch(decoder->mpp_format){
        case MPP_FMT_YUV420SP:
            decoder->rga_informat = RGA_FORMAT_YCbCr_420_SP;
            break;
        case MPP_FMT_YUV422SP:
            decoder->rga_informat = RGA_FORMAT_YCbCr_422_SP;
            break;
        }
        if(decoder->rga_informat){
            decoder->buffer_callback = rkmpp_rga_convert_buf;
            if(decoder->norga || decoder->rga_fd < 0)
                av_log(avctx, AV_LOG_INFO, "Decoder is set to use AVBuffer with NV12->YUV420P conversion through libyuv.\n");
            else
                av_log(avctx, AV_LOG_INFO, "Decoder is set to use AVBuffer with NV12->YUV420P conversion through RGA2.\n");
            return 0;
        }
    }
    av_log(avctx, AV_LOG_ERROR, "Unknown MPP format:%d and AVFormat:%d.\n", decoder->mpp_format, avctx->pix_fmt);
    return AVERROR_UNKNOWN;
}

static int rkmpp_get_frame(AVCodecContext *avctx, AVFrame *frame, int timeout)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;
    MppFrame mppframe = NULL;
    MppBuffer buffer = NULL;
    int ret, mode, latency;

    decoder->mpi->control(decoder->ctx, MPP_SET_OUTPUT_TIMEOUT, (MppParam)&timeout);

    ret = decoder->mpi->decode_get_frame(decoder->ctx, &mppframe);
    if (ret != MPP_OK && ret != MPP_ERR_TIMEOUT) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get frame (code = %d)\n", ret);
        return AVERROR_UNKNOWN;
    }

    if (!mppframe) {
        av_log(avctx, AV_LOG_DEBUG, "Timeout getting decoded frame.\n");
        return AVERROR(EAGAIN);
    }

    if (mpp_frame_get_eos(mppframe)) {
        av_log(avctx, AV_LOG_DEBUG, "Received a EOS frame.\n");
        ret = AVERROR_EOF;
        goto fail;
    }

    if (mpp_frame_get_discard(mppframe)) {
        av_log(avctx, AV_LOG_DEBUG, "Received a discard frame.\n");
        ret = AVERROR(EAGAIN);
        goto fail;
    }

    if (mpp_frame_get_errinfo(mppframe)) {
        av_log(avctx, AV_LOG_ERROR, "Received a errinfo frame.\n");
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    if (mpp_frame_get_info_change(mppframe)) {
        AVHWFramesContext *hwframes;
        decoder->mpp_format = mpp_frame_get_fmt(mppframe) & MPP_FRAME_FMT_MASK;

        av_log(avctx, AV_LOG_INFO, "Decoder noticed an info change (%dx%d), format=%d\n",
               (int)mpp_frame_get_width(mppframe), (int)mpp_frame_get_height(mppframe),
               (int)mpp_frame_get_fmt(mppframe));

        avctx->width = mpp_frame_get_width(mppframe);
        avctx->height = mpp_frame_get_height(mppframe);

        // chromium would align planes' width and height to 32, adding this
        // hack to avoid breaking the plane buffers' contiguous.
        avctx->coded_width = FFALIGN(avctx->width, 64);
        avctx->coded_height = FFALIGN(avctx->height, 64);

        decoder->mpi->control(decoder->ctx, MPP_DEC_SET_FRAME_INFO, (MppParam) mppframe);
        decoder->mpi->control(decoder->ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);

        ret = set_buffer_callback(decoder, avctx);
        if (ret)
            goto fail;

        av_buffer_unref(&decoder->frames_ref);

        decoder->frames_ref = av_hwframe_ctx_alloc(decoder->device_ref);
        if (!decoder->frames_ref) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        hwframes = (AVHWFramesContext*)decoder->frames_ref->data;
        hwframes->format    = AV_PIX_FMT_DRM_PRIME;
        hwframes->sw_format = decoder->sw_format;
        hwframes->width     = avctx->width;
        hwframes->height    = avctx->height;
        ret = av_hwframe_ctx_init(decoder->frames_ref);
        if (!ret) // FIX ME!
            ret = AVERROR(EAGAIN);

        goto fail;
    }

    // here we should have a valid frame
    av_log(avctx, AV_LOG_DEBUG, "Received a frame.\n");

    // now setup the frame buffer info
    buffer = mpp_frame_get_buffer(mppframe);
    if (!buffer) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get the frame buffer, frame is dropped (code = %d)\n", ret);
        ret = AVERROR(EAGAIN);
        goto fail;
    }

    latency = rkmpp_update_latency(avctx, -1);

    if(!decoder->buffer_callback){
        ret = AVERROR_UNKNOWN;
        av_log(avctx, AV_LOG_ERROR, "Decoder can't set output for MPP format:%d and AVFormat:%d.\n", decoder->mpp_format, avctx->pix_fmt);
        goto fail;
    }

    ret = decoder->buffer_callback(avctx, frame, mppframe);

    if(ret){
        av_log(avctx, AV_LOG_ERROR, "Failed set frame buffer (code = %d)\n", ret);
        goto fail;
    }

    latency = rkmpp_update_latency(avctx, latency);

    // setup general frame fields
    frame->format           = avctx->pix_fmt;
    frame->width            = mpp_frame_get_width(mppframe);
    frame->height           = mpp_frame_get_height(mppframe);
    frame->pts              = mpp_frame_get_pts(mppframe);
    frame->color_range      = mpp_frame_get_color_range(mppframe);
    frame->color_primaries  = mpp_frame_get_color_primaries(mppframe);
    frame->color_trc        = mpp_frame_get_color_trc(mppframe);
    frame->colorspace       = mpp_frame_get_colorspace(mppframe);

    // when mpp can not determine the color space, it returns reserved (0) value
    // firefox does not understand this and instead expect unspecified (2) values
    frame->color_primaries  = frame->color_primaries == AVCOL_PRI_RESERVED0 ? AVCOL_PRI_UNSPECIFIED : frame->color_primaries;
    frame->color_trc        = frame->color_trc == AVCOL_TRC_RESERVED0 ? AVCOL_TRC_UNSPECIFIED : frame->color_trc;
    frame->colorspace        = frame->colorspace == AVCOL_SPC_RGB ? AVCOL_SPC_UNSPECIFIED: frame->color_trc;

    mode = mpp_frame_get_mode(mppframe);
    frame->interlaced_frame = ((mode & MPP_FRAME_FLAG_FIELD_ORDER_MASK) == MPP_FRAME_FLAG_DEINTERLACED);
    frame->top_field_first  = ((mode & MPP_FRAME_FLAG_FIELD_ORDER_MASK) == MPP_FRAME_FLAG_TOP_FIRST);

    return 0;

fail:
    if (mppframe)
        mpp_frame_deinit(&mppframe);
    return ret;
}

static int rkmpp_send_packet(AVCodecContext *avctx, AVPacket *packet)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;
    MppPacket mpkt;
    int64_t pts = packet->pts;
    int ret;

    ret = mpp_packet_init(&mpkt, packet->data, packet->size);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init MPP packet (code = %d)\n", ret);
        return AVERROR_UNKNOWN;
    }

    mpp_packet_set_pts(mpkt, pts);

    ret = decoder->mpi->decode_put_packet(decoder->ctx, mpkt);
    mpp_packet_deinit(&mpkt);

    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_DEBUG, "Buffer full\n");
        return AVERROR(EAGAIN);
    }

    av_log(avctx, AV_LOG_DEBUG, "Wrote %d bytes to decoder\n", packet->size);
    return 0;
}

static int rkmpp_send_eos(AVCodecContext *avctx)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;
    MppPacket mpkt;
    int ret;

    ret = mpp_packet_init(&mpkt, NULL, 0);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init EOS packet (code = %d)\n", ret);
        return AVERROR_UNKNOWN;
    }

    mpp_packet_set_eos(mpkt);

    do {
        ret = decoder->mpi->decode_put_packet(decoder->ctx, mpkt);
    } while (ret != MPP_OK);
    mpp_packet_deinit(&mpkt);

    return 0;
}

static int rkmpp_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    AVCodecInternal *avci = avctx->internal;
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;
    AVPacket *packet = &decoder->packet;
    int ret_send, ret_get;

    // get packet if not already available from previous iteration
    if (!avci->draining){
        if(!packet->size){
            switch(ff_decode_get_packet(avctx, packet)){
                case AVERROR_EOF:
                    av_log(avctx, AV_LOG_DEBUG, "Draining.\n");
                    return rkmpp_send_eos(avctx);
                case AVERROR(EAGAIN):
                    av_log(avctx, AV_LOG_DEBUG, "Can't get packet retrying.\n");
                    return AVERROR(EAGAIN);
                }
        }

sendpacket:
        // there is definitely a packet to send to decoder here
        ret_send = rkmpp_send_packet(avctx, packet);
        if (ret_send == 0){
            // send successful, continue until decoder input buffer is full
            av_packet_unref(packet);
            return AVERROR(EAGAIN);
        } else if (ret_send < 0 && ret_send != AVERROR(EAGAIN)) {
            // something went wrong, raise error
            av_log(avctx, AV_LOG_ERROR, "Failed to send data (code = %d)\n", ret_send);
            return ret_send;
        }
    }

    // were here only when draining and buffer is full
    ret_get = rkmpp_get_frame(avctx, frame, MPP_TIMEOUT_BLOCK);

    if (ret_get == AVERROR_EOF){
        av_log(avctx, AV_LOG_DEBUG, "Decoder is at EOS.\n");
    // this is not likely but lets handle it in case synchronization issues of mpp
    } else if (ret_get == AVERROR(EAGAIN) && ret_send == AVERROR(EAGAIN))
        goto sendpacket;
    // only for logging
    else if (ret_get < 0 && ret_get != AVERROR(EAGAIN)) // FIXME
        av_log(avctx, AV_LOG_ERROR, "Failed to get frame (code = %d)\n", ret_get);

   return ret_get;
}

static void rkmpp_flush(AVCodecContext *avctx)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;

    av_log(avctx, AV_LOG_DEBUG, "Flush.\n");

    decoder->mpi->reset(decoder->ctx);

    decoder->norga = 0;

    decoder->last_frame_time = decoder->frames = 0;

    av_packet_unref(&decoder->packet);
}

#define RKMPP_DEC_CLASS(NAME) \
    static const AVClass rkmpp_##NAME##_dec_class = { \
        .class_name = "rkmpp_" #NAME "_dec", \
        .version    = LIBAVUTIL_VERSION_INT, \
    };

#define RKMPP_DEC(NAME, ID, BSFS) \
    RKMPP_DEC_CLASS(NAME) \
    const FFCodec ff_##NAME##_rkmpp_decoder = { \
        .p.name         = #NAME "_rkmpp", \
        CODEC_LONG_NAME(#NAME " (rkmpp)"), \
        .p.type         = AVMEDIA_TYPE_VIDEO, \
        .p.id           = ID, \
        .priv_data_size = sizeof(RKMPPDecodeContext), \
        .init           = rkmpp_init_decoder, \
        .close          = rkmpp_close_decoder, \
        FF_CODEC_RECEIVE_FRAME_CB(rkmpp_receive_frame), \
        .flush          = rkmpp_flush, \
        .p.priv_class   = &rkmpp_##NAME##_dec_class, \
        .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_HARDWARE, \
        .p.pix_fmts     = (const enum AVPixelFormat[]) { AV_PIX_FMT_DRM_PRIME, \
                                                         AV_PIX_FMT_YUV420P, \
                                                         AV_PIX_FMT_NV12, \
                                                         AV_PIX_FMT_NONE}, \
        .hw_configs     = (const AVCodecHWConfigInternal *const []) { HW_CONFIG_INTERNAL(DRM_PRIME), \
                                                                      HW_CONFIG_INTERNAL(NV12), \
                                                                      NULL}, \
        .bsfs           = BSFS, \
        .p.wrapper_name = "rkmpp", \
        .caps_internal  = FF_CODEC_CAP_NOT_INIT_THREADSAFE | FF_CODEC_CAP_CONTIGUOUS_BUFFERS \
    };

RKMPP_DEC(h263,  AV_CODEC_ID_H263,          NULL)
RKMPP_DEC(h264,  AV_CODEC_ID_H264,          "h264_mp4toannexb")
RKMPP_DEC(hevc,  AV_CODEC_ID_HEVC,          "hevc_mp4toannexb")
RKMPP_DEC(av1,   AV_CODEC_ID_AV1,           NULL)
RKMPP_DEC(vp8,   AV_CODEC_ID_VP8,           NULL)
RKMPP_DEC(vp9,   AV_CODEC_ID_VP9,           NULL)
RKMPP_DEC(mpeg1, AV_CODEC_ID_MPEG1VIDEO,    NULL)
RKMPP_DEC(mpeg2, AV_CODEC_ID_MPEG2VIDEO,    NULL)
RKMPP_DEC(mpeg4, AV_CODEC_ID_MPEG4,         "mpeg4_unpack_bframes")
