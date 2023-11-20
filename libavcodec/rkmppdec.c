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
#include "rkmpp.h"

static void rkmpp_frame_timestamp(MppFrameItem* item){
    clock_gettime(CLOCK_MONOTONIC, &item->timestamp);
}

static void rkmpp_frame_logtimecsv(AVCodecContext *avctx, MppFrameItem* item){
    RKMPPCodecContext *rk_context = avctx->priv_data;
    if (!rk_context->timing)
        return;
    if(item->old_timestamp.tv_sec)
        av_log(avctx, AV_LOG_INFO, "%lu,%lu,%lu,%lu,%lu,%lu,%lu\n",
                item->framenum,
                item->old_timestamp.tv_sec,
                item->old_timestamp.tv_nsec,
                item->timestamp.tv_sec,
                item->timestamp.tv_nsec,
                item->rga->timestamp.tv_sec,
                item->rga->timestamp.tv_nsec);
    else
        av_log(avctx, AV_LOG_INFO, "%lu,%lu,%lu,%lu,%lu\n",
                        item->framenum,
                        item->timestamp.tv_sec,
                        item->timestamp.tv_nsec,
                        item->rga->timestamp.tv_sec,
                        item->rga->timestamp.tv_nsec);
}

static int rkmpp_dec_defaults(AVCodecContext *avctx){
    RKMPPCodecContext *rk_context = avctx->priv_data;
    MppFrameFormat afbc = MPP_FRAME_FBC_AFBC_V2;

    if(rk_context->fbc == -1){
        // https://github.com/rockchip-linux/mpp/issues/453?#issuecomment-1776324884
        // RGA3 can not handle >8k
        if(rkmpp_get_codingtype(avctx) == MPP_VIDEO_CodingAV1 ||
                avctx->width > 7680 || avctx->height > 4320)
            rk_context->fbc = RKMPP_FBC_NONE;
        else
            rk_context->fbc = RKMPP_FBC_DECODER;
    }

    if (rk_context->fbc != RKMPP_FBC_NONE && rk_context->mpi->control(rk_context->ctx, MPP_DEC_SET_OUTPUT_FORMAT, &afbc)) {
        av_log(avctx, AV_LOG_ERROR, "Can not set afbc format");
        return AVERROR_UNKNOWN;
    }

    if(rk_context->libyuv == -1){
        // yuv420/2p > 4k & yuv420p10 and yuv444p modes require libyuv
        if(avctx->pix_fmt == AV_PIX_FMT_YUV420P10LE || avctx->pix_fmt == AV_PIX_FMT_YUV444P || \
                ((avctx->width > 3840 || avctx->height > 2160) && \
                        (avctx->pix_fmt == AV_PIX_FMT_YUV420P || avctx->pix_fmt == AV_PIX_FMT_YUV422P))){
            rk_context->libyuv = 1;
        } else {
            rk_context->libyuv = 0;
        }
    }

    if(rk_context->libyuv){
        av_log(avctx, AV_LOG_INFO, "Using partial libyuv soft conversion for %s (%dx%d)\n",
                av_get_pix_fmt_name(avctx->pix_fmt), avctx->width, avctx->height);
    }

    // disable NV15->P010 conversion by default it causes to much latency
    if(rk_context->drm_hdrbits == -1)
        rk_context->drm_hdrbits = rk_context->fbc == RKMPP_FBC_DRM ? 10 : 8;

    if(rk_context->fbc == RKMPP_FBC_DRM && rk_context->drm_hdrbits != 10){
        av_log(avctx, AV_LOG_ERROR, "FBC drm mode is only available with drmhdrbits=10");
        return AVERROR_UNKNOWN;
    }

    if(rk_context->rga_height || rk_context->rga_height){
        if(rk_context->libyuv){
            av_log(avctx, AV_LOG_ERROR, "Scaling is not supported in libyuv mode\n");
            return AVERROR_UNKNOWN;
        }
        if(rk_context->fbc == RKMPP_FBC_DRM){
            av_log(avctx, AV_LOG_ERROR, "Scaling is not supported in drm fbc mode\n");
            return AVERROR_UNKNOWN;
        }

        if(rkmpp_check_rga_dimensions(rk_context)){
            av_log(avctx, AV_LOG_ERROR, "Scaling is not in between %d and %d.\n", RKMPP_RGA_MIN_SIZE, RKMPP_RGA_MAX_SIZE);
            return AVERROR_UNKNOWN;
        }

        avctx->width = FFALIGN(rk_context->rga_width, RKMPP_DIM_ALIGN);
        avctx->height = FFALIGN(rk_context->rga_height, RKMPP_DIM_ALIGN);
    }

    avctx->coded_width = FFALIGN(avctx->width, 64);
    avctx->coded_height = FFALIGN(avctx->height, 64);

    return 0;
}

int rkmpp_init_decoder(AVCodecContext *avctx){
    RKMPPCodecContext *rk_context = avctx->priv_data;
    MppCompat *compatItem = NULL;
    char *env;
    int ret;

    ret = rk_context->mpi->control(rk_context->ctx, MPP_DEC_SET_DISABLE_ERROR, NULL) == MPP_OK;
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to prepare Codec (code = %d)\n", ret);
        return AVERROR_UNKNOWN;
    }

    rk_context->hwdevice_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_DRM);
    if (!rk_context->hwdevice_ref) {
        return AVERROR(ENOMEM);
    }

    ret = av_hwdevice_ctx_init(rk_context->hwdevice_ref);
    if (ret < 0)
        return ret;

    av_buffer_unref(&rk_context->hwframes_ref);
    rk_context->hwframes_ref = av_hwframe_ctx_alloc(rk_context->hwdevice_ref);
    if (!rk_context->hwframes_ref) {
        return AVERROR(ENOMEM);
    }

    // not aligning to odds of 256 saves up to %20~%25 memory size
    compatItem = mpp_compat_query_by_id(MPP_COMPAT_DEC_FBC_HDR_256_ODD);
    mpp_compat_update(compatItem, 0);
#if 0
    // check https://github.com/rockchip-linux/mpp/issues/422#issuecomment-1784178924
    compatItem = mpp_compat_query_by_id(MPP_COMPAT_DEC_NFBC_NO_256_ODD);
    mpp_compat_update(compatItem, 0);
#endif
    rk_context->mpp_decode_fifo.limit = -1;
    rk_context->mpp_rga_fifo.limit = -1;

    // override the the pixfmt according env variable
    env = getenv("FFMPEG_RKMPP_PIXFMT");
    if(env != NULL){
        avctx->pix_fmt = av_get_pix_fmt(env);
        if(avctx->pix_fmt == AV_PIX_FMT_NONE){
            av_log(avctx, AV_LOG_ERROR, "Unknown format %s\n", env);
            return AVERROR_UNKNOWN;
        }
    } else {
        // check if selected format is supported
        enum AVPixelFormat fmt = avctx->pix_fmt;
        avctx->pix_fmt = AV_PIX_FMT_NONE;
        for (int n = 0; avctx->codec->pix_fmts[n] != AV_PIX_FMT_NONE; n++){
            if(fmt == avctx->codec->pix_fmts[n])
                avctx->pix_fmt = avctx->codec->pix_fmts[n];
        }
        // if not auto select
        if (avctx->pix_fmt == AV_PIX_FMT_NONE)
            avctx->pix_fmt = ff_get_format(avctx, avctx->codec->pix_fmts);
    }

    rk_context->drm_hdrbits = -1;
    env = getenv("FFMPEG_RKMPP_DRMHDRBITS");
    if (env != NULL){
        if(!strcmp("8", env))
            rk_context->drm_hdrbits = 8;
        else if(!strcmp("10", env))
            rk_context->drm_hdrbits = 10;
        else if(!strcmp("16", env))
            rk_context->drm_hdrbits = 16;
        else {
            av_log(avctx, AV_LOG_ERROR, "unknown DRMHDRBITS value, valid values are 8: NV12, 10: NV15, 16:p010\n");
            return AVERROR_UNKNOWN;
        }
    }

    rk_context->fbc = -1;
    env = getenv("FFMPEG_RKMPP_AFBC");
    if (env != NULL){
        if(!strcmp("decoder", env))
            rk_context->fbc = RKMPP_FBC_DECODER;
        else if(!strcmp("drm", env))
            rk_context->fbc = RKMPP_FBC_DRM;
        else if(!strcmp("none", env))
            rk_context->fbc = RKMPP_FBC_NONE;
        else {
            av_log(avctx, AV_LOG_ERROR, "unknown AFBC value, valid values are none,decoder,drm\n");
            return AVERROR_UNKNOWN;
        }
    }

    rk_context->libyuv = -1;
    env = getenv("FFMPEG_RKMPP_LIBYUV");
    if (env != NULL){
        if(!strcmp("1", env))
            rk_context->libyuv = 1;
        else if(!strcmp("0", env))
            rk_context->libyuv = 0;
        else {
            av_log(avctx, AV_LOG_ERROR, "unknown libyuv value, valid values are 0,1\n");
            return AVERROR_UNKNOWN;
        }
    }

    env = getenv("FFMPEG_RKMPP_TIMING");
    if (env != NULL){
        if(!strcmp("1", env))
            rk_context->timing = 1;
        else if(!strcmp("0", env))
            rk_context->timing = 0;
        else {
            av_log(avctx, AV_LOG_ERROR, "unknown timing value, valid values are 0,1\n");
            return AVERROR_UNKNOWN;
        }
    }

    if(rkmpp_dec_defaults(avctx))
        return AVERROR_UNKNOWN;

    return 0;
}

static int rkmpp_config_decoder(AVCodecContext *avctx, MppFrame mppframe){
    MppFrameFormat mpp_format = mpp_frame_get_fmt(mppframe) & MPP_FRAME_FMT_MASK;
    RKMPPCodecContext *rk_context = avctx->priv_data;
    int isfbc=MPP_FRAME_FMT_IS_FBC(mpp_frame_get_fmt(mppframe));
    int ret=0;

    if(rk_context->fbc && !isfbc){
        av_log(avctx, AV_LOG_WARNING, "AFBC mode is requested but decoder does not support it, not using AFBC\n");
        rk_context->fbc = RKMPP_FBC_NONE;
    }

    rkmpp_get_mpp_format(&rk_context->informat, mpp_format, mpp_frame_get_width(mppframe), mpp_frame_get_height(mppframe), 0,
            mpp_frame_get_hor_stride(mppframe), mpp_frame_get_ver_stride(mppframe), mpp_frame_get_offset_y(mppframe),
            mpp_frame_get_buf_size(mppframe), mpp_frame_get_fbc_hdr_stride(mppframe), 0);

    // determine the decoding flow
    if(rk_context->hascfg)
        return AVERROR(EAGAIN);
    else if (avctx->pix_fmt == AV_PIX_FMT_DRM_PRIME){
        AVHWFramesContext *hwframes;

        if(rk_context->fbc == RKMPP_FBC_DRM){ // directly use the FBC output
            rkmpp_get_av_format(&rk_context->outformat, rk_context->informat.av,
                    mpp_frame_get_width(mppframe), mpp_frame_get_height(mppframe), 0,
                    mpp_frame_get_hor_stride(mppframe), mpp_frame_get_ver_stride(mppframe), 0,
                    mpp_frame_get_buf_size(mppframe), mpp_frame_get_fbc_hdr_stride(mppframe), 0);
            rk_context->codec_flow = NOCONVERSION;
        } else {
            if(rk_context->informat.av == AV_PIX_FMT_NV15){ // convert to requested format
                if(rk_context->drm_hdrbits == 16) // P010
                    rkmpp_get_av_format(&rk_context->outformat, AV_PIX_FMT_P010LE,
                            avctx->width, avctx->height, RKMPP_DRM_STRIDE_ALIGN, 0, 0, 0, 0, 0, 0);
                else if (rk_context->drm_hdrbits == 8) // NV12
                    rkmpp_get_av_format(&rk_context->outformat, AV_PIX_FMT_NV12,
                            avctx->width, avctx->height, RKMPP_DRM_STRIDE_ALIGN, 0, 0, 0, 0, 0, 0);
                rk_context->codec_flow = CONVERT;
            } else if (rk_context->fbc == RKMPP_FBC_DECODER){
                // mpp is spitting FBC, in this mode we convert to un FBC frames with RGA
                rkmpp_get_av_format(&rk_context->outformat, rk_context->informat.av,
                        avctx->width, avctx->height, RKMPP_DRM_STRIDE_ALIGN, 0, 0, 0, 0, 0, 0);
                rk_context->codec_flow = CONVERT;
            } else if (mpp_frame_get_hor_stride(mppframe) != FFALIGN(avctx->width, RKMPP_DRM_STRIDE_ALIGN)){
                // drm prime frame strides must be 64 aligned when imported thorugh EGL,
                // in this case we must realign
                rkmpp_get_av_format(&rk_context->outformat, rk_context->informat.av,
                        avctx->width, avctx->height, RKMPP_DRM_STRIDE_ALIGN, 0, 0, 0, 0, 0, 0);
                rk_context->codec_flow = CONVERT;
            } else if(rk_context->rga_width || rk_context->rga_height){
                // scaling is requested
                rkmpp_get_av_format(&rk_context->outformat, rk_context->informat.av,
                        avctx->width, avctx->height, RKMPP_DRM_STRIDE_ALIGN, 0, 0, 0, 0, 0, 0);
                rk_context->codec_flow = CONVERT;
            } else {
                rkmpp_get_av_format(&rk_context->outformat, rk_context->informat.av,
                        avctx->width, avctx->height, 0,
                        mpp_frame_get_hor_stride(mppframe), mpp_frame_get_ver_stride(mppframe), 0,
                        mpp_frame_get_buf_size(mppframe), 0, 0);
                rk_context->codec_flow = NOCONVERSION;
            }
        }
        hwframes = (AVHWFramesContext*)rk_context->hwframes_ref->data;
        hwframes->format = AV_PIX_FMT_DRM_PRIME;
        hwframes->sw_format = rk_context->outformat.av;
        hwframes->width = rk_context->outformat.planedata.width;
        hwframes->height = rk_context->outformat.planedata.height;
        ret = av_hwframe_ctx_init(rk_context->hwframes_ref);
    } else if (avctx->pix_fmt != rk_context->informat.av){
        // check if we need swapping the mpp frames
        if( avctx->pix_fmt == AV_PIX_FMT_YUV420P10LE){
            // nv15(mpp) -> P010(rga3, swap) -> YUV420P10 (soft)
            rkmpp_get_av_format(&rk_context->swapformat, AV_PIX_FMT_P010LE,
                    avctx->width, avctx->height, RKMPP_STRIDE_ALIGN, 0, 0, 0, 0, 0, 0);
            // important note:
            // normally, P010 is msb aligned, with little endian byte order and LSB bit order
            // we force rga to output lsb aligned planes because,
            // the output format YUV420P10 is lsb aligned. This swap frame is no more P010
            // it does not have a name, but will be YUV420P10LE after UV Planes are split
            rk_context->swapformat.rga_msb_aligned = 0;
            rk_context->codec_flow = SWAPANDCONVERT;
        } else if (rk_context->informat.av == AV_PIX_FMT_NV15 && // 10bit to 8 bit case
                (avctx->pix_fmt == AV_PIX_FMT_YUV420P || avctx->pix_fmt == AV_PIX_FMT_YUV422P || avctx->pix_fmt == AV_PIX_FMT_YUV444P)){
           // nv15(mpp) -> NV12(rga3, swap) -> YUV420P (rga2 or soft when >4k)
            rkmpp_get_av_format(&rk_context->swapformat, AV_PIX_FMT_NV12,
                    avctx->width, avctx->height, RKMPP_STRIDE_ALIGN, 0, 0, 0, 0, 0, 0);
            rk_context->codec_flow = SWAPANDCONVERT;
        } else if (rk_context->informat.fbcstride && rk_context->libyuv){ // libyuv needs uncompressed Y plane
            rkmpp_get_av_format(&rk_context->swapformat, rk_context->informat.av,
                            avctx->width, avctx->height, RKMPP_STRIDE_ALIGN, 0, 0, 0, 0, 0, 0);
            rk_context->codec_flow = SWAPANDCONVERT;
        } else if (rk_context->informat.fbcstride && // rga2 can not afbc
                (avctx->pix_fmt == AV_PIX_FMT_YUV420P || avctx->pix_fmt == AV_PIX_FMT_YUV422P || avctx->pix_fmt == AV_PIX_FMT_YUV444P)){
            rkmpp_get_av_format(&rk_context->swapformat, rk_context->informat.av,
                            avctx->width, avctx->height, RKMPP_STRIDE_ALIGN, 0, 0, 0, 0, 0, 0);
            rk_context->codec_flow = SWAPANDCONVERT;
        } else
            rk_context->codec_flow = CONVERT;

        // determine the output format
        if(rk_context->codec_flow == SWAPANDCONVERT){ // give libyuv swap format u plane
            rkmpp_get_av_format(&rk_context->outformat, avctx->pix_fmt,
                            avctx->width, avctx->height, RKMPP_STRIDE_ALIGN, 0, 0, 0, 0, 0,
                            rk_context->libyuv ? &rk_context->swapformat.factors[1] : 0);
        } else if(rk_context->codec_flow == CONVERT){ // give libyuv input format u plane
            rkmpp_get_av_format(&rk_context->outformat, avctx->pix_fmt,
                            avctx->width, avctx->height, RKMPP_STRIDE_ALIGN, 0, 0, 0, 0, 0,
                            rk_context->libyuv ? &rk_context->informat.factors[1] : 0);
        }

    } else if (rk_context->informat.fbcstride) {
        rkmpp_get_av_format(&rk_context->outformat, rk_context->informat.av,
                avctx->width, avctx->height, RKMPP_STRIDE_ALIGN, 0, 0, 0, 0, 0, 0);
        rk_context->codec_flow = CONVERT;
    } else if (rk_context->rga_height || rk_context->rga_width) {
        // scaling is requested
        rkmpp_get_av_format(&rk_context->outformat, rk_context->informat.av,
                avctx->width, avctx->height, RKMPP_STRIDE_ALIGN, 0, 0, 0, 0, 0, 0);
        rk_context->codec_flow = CONVERT;
    } else {
        rkmpp_get_av_format(&rk_context->outformat, rk_context->informat.av,
                avctx->width, avctx->height, 0,
                mpp_frame_get_hor_stride(mppframe), mpp_frame_get_ver_stride(mppframe), 0,
                mpp_frame_get_buf_size(mppframe), 0, 0);
        rk_context->codec_flow = NOCONVERSION;
    }

    // log decoding flow
    switch(rk_context->codec_flow){
    case SWAPANDCONVERT:
        av_log(avctx, AV_LOG_INFO, "Pixfmt (%s), Conversion (%s%s->%s->%s%s)\n",
                av_get_pix_fmt_name(avctx->pix_fmt),
                av_get_pix_fmt_name(rk_context->informat.av),
                isfbc ? "[FBC]" : "",
                av_get_pix_fmt_name(rk_context->swapformat.av),
                av_get_pix_fmt_name(rk_context->outformat.av),
                rk_context->libyuv ? "[LIBYUV]" : "");
        if(rk_context->swapformat.qual > rk_context->outformat.qual)
            av_log(avctx, AV_LOG_WARNING, "Potential quality loss on conversion.\n");
        break;
    case CONVERT:
        av_log(avctx, AV_LOG_INFO, "Pixfmt (%s), Conversion (%s%s->%s%s)\n",
                av_get_pix_fmt_name(avctx->pix_fmt),
                av_get_pix_fmt_name(rk_context->informat.av),
                isfbc ? "[FBC]" : "",
                av_get_pix_fmt_name(rk_context->outformat.av),
                rk_context->libyuv ? "[LIBYUV]" : "");
        if(rk_context->informat.qual > rk_context->outformat.qual)
            av_log(avctx, AV_LOG_WARNING, "Potential quality loss on conversion.\n");
        break;
    case NOCONVERSION:
        av_log(avctx, AV_LOG_INFO, "Pixfmt (%s), Decoder Output (%s%s)\n",
                av_get_pix_fmt_name(avctx->pix_fmt),
                av_get_pix_fmt_name(rk_context->informat.av),
                isfbc ? "[FBC]" : "");
        break;
    }

    rk_context->dma_dec_count=10; // per core
    rk_context->dma_swap_count=3; // one for each core
    rk_context->dma_rga_count=RKMPP_RENDER_DEPTH;

    // when the decoder is dual core, we need double the size of buffers
    // H26x decoder can also be single core but there are dual core version
    // since we can not know this, we assume the worst case
    switch (rkmpp_get_codingtype(avctx)){
    case MPP_VIDEO_CodingAVC:
    case MPP_VIDEO_CodingHEVC:
        rk_context->dma_dec_count += 8;
        break;
    }

    // decoder frames are hold until rendering is finished
    if(rk_context->codec_flow == NOCONVERSION || (rk_context->libyuv && rk_context->codec_flow == CONVERT))
        rk_context->dma_dec_count += RKMPP_RENDER_DEPTH;
    // swap frame are hold until rendering is finished
    else if(rk_context->codec_flow == SWAPANDCONVERT && rk_context->libyuv)
        rk_context->dma_swap_count += RKMPP_RENDER_DEPTH;

    rk_context->mpp_decode_fifo.limit = 2;
    rk_context->mpp_rga_fifo.limit = 2;

    // allocate codec buffers
    ret = rkmpp_buffer_alloc(avctx, rk_context->informat.planedata.overshoot,
            &rk_context->buffer_group, rk_context->dma_dec_count);
    if (ret) {
       av_log(avctx, AV_LOG_ERROR, "Failed allocate mem for codec (code = %d)\n", ret);
       return AVERROR_UNKNOWN;
    }

    //set decoder external buffer
    ret = rk_context->mpi->control(rk_context->ctx, MPP_DEC_SET_EXT_BUF_GROUP, rk_context->buffer_group);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "Failed to assign buffer group for codec (code = %d)\n", ret);
        return AVERROR_UNKNOWN;
    }

    //allocate swap buffers
    if(rk_context->codec_flow == SWAPANDCONVERT){
        ret = rkmpp_buffer_alloc(avctx, rk_context->swapformat.planedata.overshoot,
                &rk_context->buffer_group_swap, rk_context->dma_swap_count);
        if (ret) {
           av_log(avctx, AV_LOG_ERROR, "Failed to allocate memory for swap (code = %d)\n", ret);
           return AVERROR_UNKNOWN;
        }
    }

    // allocate output buffers
    if (rk_context->codec_flow != NOCONVERSION){
        ret = rkmpp_buffer_alloc(avctx, rk_context->outformat.planedata.overshoot,
                &rk_context->buffer_group_rga, rk_context->dma_rga_count);
        if (ret) {
           av_log(avctx, AV_LOG_ERROR, "Failed to allocate memory for rga (code = %d)\n", ret);
           return AVERROR_UNKNOWN;
        }
    }

    rk_context->hascfg = 1;

    return 0;
}
static int rkmpp_get_frame(AVCodecContext *avctx, AVFrame *frame){
    RKMPPCodecContext *rk_context = avctx->priv_data;
    MppFrameItem* item = rk_context->mpp_rga_fifo.first;
    MppFrameItem* outitem = NULL;
    AVDRMFrameDescriptor *desc = NULL;
    MppBuffer mppbuffer = NULL;
    int mpp_mode, ret;

    if(!item){
        // In case last frame is received and eof is sent afterwards. Check if buffer is empty and EOF.
        if(!rk_context->mpp_rga_fifo.last && rk_context->eof)
            return AVERROR_EOF;
        av_log(avctx, AV_LOG_TRACE, "Waiting to receive output frame.\n");
        return AVERROR(EAGAIN);
    }

    // poll rga status
    ret = rkmpp_rga_wait(item->rga, MPP_TIMEOUT_NON_BLOCK, &item->timestamp);
    if(ret == AVERROR_UNKNOWN){
        av_log(avctx, AV_LOG_ERROR, "RGA timed out for out fifo frame %ld. Dropped\n", item->framenum);
        rkmpp_fifo_pop(&rk_context->mpp_rga_fifo);
        goto clean;
    } else if (ret < 0)
        return AVERROR(EAGAIN);

    if(item->rga)
        rkmpp_frame_timestamp(item->rga);
    rkmpp_frame_logtimecsv(avctx, item);
    av_log(avctx, AV_LOG_DEBUG, "Received frame %ld for outputting.\n", item->framenum);

    rkmpp_fifo_pop(&rk_context->mpp_rga_fifo);
    // check if we need to preserve the input frame until the end of rendering.
    if(!rk_context->libyuv && item->rga){
        MppFrameItem* item_orig = item;
        // we are done with input, clear it
        item = item->rga;
        item->framenum = item_orig->framenum;
        item_orig->rga = NULL;
        rkmpp_release_mppframe_item(item_orig, NULL);
        outitem = item;
    } else {
        outitem = item->rga ? item->rga : item;
        outitem->framenum = item->rga ? item->rga->framenum : item->framenum;
    }

    // keep output frame until rendering is finished
    if(rkmpp_set_mppframe_to_av(avctx, item, frame, RKMPP_MPPFRAME_BUFINDEX)){
        ret = AVERROR(ENOMEM);
        av_log(avctx, AV_LOG_ERROR, "Error setting output frame %ld to avbuffer.\n", item->framenum);
        goto clean;
    }

    // extract frame properties to avframe
    mpp_mode = mpp_frame_get_mode(outitem->mppframe);
    frame->format = outitem->format->av;
    frame->width = mpp_frame_get_width(outitem->mppframe);
    frame->height =  mpp_frame_get_height(outitem->mppframe);
    frame->color_range = mpp_frame_get_color_range(outitem->mppframe);
    frame->color_primaries = mpp_frame_get_color_primaries(outitem->mppframe);
    frame->color_trc = mpp_frame_get_color_trc(outitem->mppframe);
    frame->colorspace = mpp_frame_get_colorspace(outitem->mppframe);
    frame->pts = mpp_frame_get_pts(outitem->mppframe);
    frame->interlaced_frame = ((mpp_mode & MPP_FRAME_FLAG_FIELD_ORDER_MASK) == MPP_FRAME_FLAG_DEINTERLACED);
    frame->top_field_first  = ((mpp_mode & MPP_FRAME_FLAG_FIELD_ORDER_MASK) == MPP_FRAME_FLAG_TOP_FIRST);


    // wrap the mppframe buffer to AVFrame
    if (avctx->pix_fmt == AV_PIX_FMT_DRM_PRIME){
        AVDRMLayerDescriptor *layer = NULL;
        mppbuffer = mpp_frame_get_buffer(outitem->mppframe);

        desc = av_mallocz(sizeof(AVDRMFrameDescriptor));
        if (!desc || rkmpp_set_drmdesc_to_av(desc, frame) < 0) {
            av_log(avctx, AV_LOG_ERROR, "Error allocating drm descriptor for frame %ld.\n", outitem->framenum);
            ret = AVERROR(ENOMEM);
            goto clean;
        }

        desc->nb_objects = 1;
        desc->objects[0].fd = mpp_buffer_get_fd(mppbuffer);
        desc->objects[0].size = mpp_buffer_get_size(mppbuffer);

        desc->nb_layers = 1;
        layer = &desc->layers[0];

        if(outitem->format->fbcstride){
            layer->format = outitem->format->drm_fbc;
            layer->nb_planes = 1;
            layer->planes[0].object_index = 0;
            layer->planes[0].offset = outitem->format->planedata.plane[0].offset;
            for(int i=0; i < outitem->format->numplanes; i++){
                layer->planes[i].pitch += outitem->format->planedata.plane[i].hstride;
            }
            desc->objects[0].format_modifier =  DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 | AFBC_FORMAT_MOD_SPARSE);
        } else {
            layer->nb_planes = outitem->format->numplanes;
            layer->format = outitem->format->drm;
            for(int i=0; i < outitem->format->numplanes; i++){
                layer->planes[i].object_index = 0;
                layer->planes[i].offset = outitem->format->planedata.plane[i].offset;
                layer->planes[i].pitch = outitem->format->planedata.plane[i].hstride;
            }
        }

        frame->data[0]  = (uint8_t *)desc;
        frame->hw_frames_ctx = av_buffer_ref(rk_context->hwframes_ref);
        frame->format = AV_PIX_FMT_DRM_PRIME;
        if (!frame->hw_frames_ctx) {
            ret = AVERROR(ENOMEM);
            av_log(avctx, AV_LOG_ERROR, "Error allocating drm hwframe for frame %ld.\n", outitem->framenum);
            goto clean;
        }
    } else {
        uint8_t* bufferaddr = mpp_buffer_get_ptr(mpp_frame_get_buffer(outitem->mppframe));

        for(int i=0; i < outitem->format->numplanes; i++){
            frame->data[i] = bufferaddr + outitem->format->planedata.plane[i].offset;
            frame->linesize[i] = outitem->format->planedata.plane[i].hstride;
        }
        // when soft converting yuv framenum we only convert uv plane to outframe, y plane is still on inframe
        if(rk_context->libyuv){
            frame->data[0] = (uint8_t *)mpp_buffer_get_ptr(mpp_frame_get_buffer(item->mppframe)) + item->format->planedata.plane[0].offset;
            frame->linesize[0] = mpp_frame_get_hor_stride(item->mppframe);
        }
        frame->extended_data = frame->data;
        frame->format = outitem->format->av;
    }

    if(rk_context->eof == item->framenum)
        ret = AVERROR_EOF;
    else
        ret = 0;

    return ret;
clean:
    if (outitem)
        rkmpp_release_mppframe_item(outitem, NULL);
    else if (item)
        rkmpp_release_mppframe_item(item, NULL);
    if (desc)
        av_free(desc);
    return ret;
}

static int rkmpp_convert_frame(AVCodecContext *avctx, AVFrame *frame){
   // do conversion
    RKMPPCodecContext *rk_context = avctx->priv_data;
    MppFrameItem* item = rk_context->mpp_decode_fifo.first;
    MppFrameItem* item_orig = item;
    int ret;

    if(!item){
        av_log(avctx, AV_LOG_TRACE, "Waiting to receive input frame.\n");
        return AVERROR(EAGAIN);
    }

    if(!rkmpp_fifo_isfull(&rk_context->mpp_rga_fifo)){
        av_log(avctx, AV_LOG_TRACE, "RGA Fifo full\n");
        return AVERROR(EAGAIN);
    }

    // poll rga status
    ret = rkmpp_rga_wait(item->rga, MPP_TIMEOUT_NON_BLOCK, &item->timestamp);
    if(ret == AVERROR_UNKNOWN){
        av_log(avctx, AV_LOG_ERROR, "RGA timed out for in fifo frame %ld. Dropped\n", item->framenum);
        goto clean;
    } else if (ret < 0)
        return AVERROR(EAGAIN);

    av_log(avctx, AV_LOG_DEBUG, "Received frame %ld for conversion.\n", item->framenum);

    if(rk_context->codec_flow == SWAPANDCONVERT){
        item = item->rga;
        item->framenum = item_orig->framenum;
        item->old_timestamp = item_orig->timestamp;
        item->format = &rk_context->swapformat;
        rkmpp_frame_timestamp(item);
        strcpy(item->type , "swap");
    }

    item->rga = av_mallocz(sizeof(MppFrameItem));
    item->rga->format = &rk_context->outformat;
    item->rga->mppframe = rkmpp_create_mpp_frame(item->rga->format->planedata.width, item->rga->format->planedata.height,
            item->rga->format, rk_context->buffer_group_rga, NULL, NULL);
    strcpy(item->rga->type , "output");

    if(!item->rga->mppframe){
        av_log(avctx, AV_LOG_WARNING, "Can not get out frame\n");
        goto cleanrga;
    }

    rkmpp_transfer_mpp_props(item->mppframe, item->rga->mppframe);
    if(rk_context->libyuv){
        item->rga->rgafence = 0;
        ret = 0;
        rkmpp_semi_to_planar_soft(item->mppframe, item->format, item->rga->mppframe, item->rga->format);
    } else {
        ret = rkmpp_rga_convert_mpp_mpp(avctx, item->mppframe, item->format, item->rga->mppframe, item->rga->format,
                    &item->rga->rgafence);
        if(ret)
            goto cleanrga;
    }

    rkmpp_fifo_pop(&rk_context->mpp_decode_fifo);
    if(rk_context->codec_flow == SWAPANDCONVERT){
        item_orig->rga = NULL;
        rkmpp_release_mppframe_item(item_orig, NULL);
    }
    rkmpp_fifo_push(&rk_context->mpp_rga_fifo, item);
    return ret;
clean:
    rkmpp_fifo_pop(&rk_context->mpp_decode_fifo);
    if (item)
        rkmpp_release_mppframe_item(item, NULL);
    return ret;
cleanrga:
    if(item->rga->mppframe)
        mpp_frame_deinit(&item->rga->mppframe);
    av_free(item->rga);
    item->rga = NULL;
    return AVERROR(EAGAIN);
}

static int rkmpp_decode_frame(AVCodecContext *avctx, int timeout){
    RKMPPCodecContext *rk_context = avctx->priv_data;
    MppFrame mppframe = NULL;
    MppFrameItem* item = NULL;
    MppBuffer mppbuffer = NULL;

    int ret=0;

    if(rk_context->eof)
        return AVERROR(EAGAIN);

    if(!rkmpp_fifo_isfull(&rk_context->mpp_decode_fifo) ||
            (rk_context->buffer_group_swap && !mpp_buffer_group_unused(rk_context->buffer_group_swap))){
        av_log(avctx, AV_LOG_TRACE, "Decode Fifo full\n");
        return AVERROR(EAGAIN);
    }

    rk_context->mpi->control(rk_context->ctx, MPP_SET_OUTPUT_TIMEOUT, (MppParam)&timeout);
    ret = rk_context->mpi->decode_get_frame(rk_context->ctx, &mppframe);

    if (ret != MPP_OK && ret != MPP_ERR_TIMEOUT) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get frame (code = %d)\n", ret);
        return AVERROR_UNKNOWN;
    }

    if (!mppframe) {
        av_log(avctx, AV_LOG_TRACE, "Timeout getting decoded frame.\n");
        return AVERROR(EAGAIN);
    }

    item = av_mallocz(sizeof(MppFrameItem));
    item->mppframe = mppframe;
    strcpy(item->type, "input");

    if (mpp_frame_get_eos(item->mppframe)) {
        av_log(avctx, AV_LOG_VERBOSE, "Received a EOS frame.\n");
        rk_context->eof = rk_context->frame_num;
        ret = AVERROR(EAGAIN);
        goto clean;
    }

    if (mpp_frame_get_discard(item->mppframe)) {
        av_log(avctx, AV_LOG_VERBOSE, "Received a discard frame.\n");
        ret = AVERROR(EAGAIN);
        goto clean;
    }

    if (mpp_frame_get_errinfo(item->mppframe)) {
        av_log(avctx, AV_LOG_ERROR, "Received a errinfo frame.\n");
        ret = AVERROR_UNKNOWN;
        goto clean;
    }

    if (mpp_frame_get_info_change(item->mppframe)) {
        if(rkmpp_config_decoder(avctx, item->mppframe)){
            ret = AVERROR_UNKNOWN;
            goto clean;
        }

        av_log(avctx, AV_LOG_VERBOSE, "Decoder noticed an info change\n");
        rk_context->mpi->control(rk_context->ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
        ret = 0;
        goto clean;
    }

    // now setup the frame buffer info
    mppbuffer = mpp_frame_get_buffer(item->mppframe);
    if (!mppbuffer) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get the frame buffer, frame is dropped (code = %d)\n", ret);
        ret = AVERROR(EAGAIN);
        goto clean;
    }

    // here we should have a valid frame
    rk_context->frame_num++;
    item->framenum = rk_context->frame_num;
    item->format = &rk_context->informat;
    rkmpp_frame_timestamp(item);

    av_log(avctx, AV_LOG_DEBUG, "Received frame %ld for decoding.\n", rk_context->frame_num);

    // swap the mpp output if needed for HDR
    if(rk_context->codec_flow == SWAPANDCONVERT){
        item->rga = av_mallocz(sizeof(MppFrameItem));
        item->rga->mppframe = rkmpp_create_mpp_frame(rk_context->swapformat.planedata.width,
                rk_context->swapformat.planedata.height, &rk_context->swapformat,
                rk_context->buffer_group_swap, NULL, NULL);
        strcpy(item->rga->type, "swap");
        if(!item->rga->mppframe){
            av_log(avctx, AV_LOG_ERROR, "Can not get swap frame buffer\n");
            ret = AVERROR(EAGAIN);
            goto clean;
        }
       if(rkmpp_rga_convert_mpp_mpp(avctx, item->mppframe, item->format,
               item->rga->mppframe, &rk_context->swapformat, &item->rga->rgafence)){
           av_log(avctx, AV_LOG_ERROR, "Can not convert NV15 frame\n");
           ret = AVERROR(ENOMEM);
           goto clean;
       } else {
           rkmpp_transfer_mpp_props(item->mppframe, item->rga->mppframe);
           item->rga->format = &rk_context->swapformat;
           rkmpp_fifo_push(&rk_context->mpp_decode_fifo, item);
       }
   } else if (rk_context->codec_flow == CONVERT)
       rkmpp_fifo_push(&rk_context->mpp_decode_fifo, item);
   else
       rkmpp_fifo_push(&rk_context->mpp_rga_fifo, item);

    return 0;
clean:
    if (item)
        rkmpp_release_mppframe_item(item, NULL);
    return ret;
}

static int rkmpp_send_packet(AVCodecContext *avctx, AVPacket *packet)
{
    RKMPPCodecContext *rk_context = avctx->priv_data;
    MppPacket mpkt;
    int64_t pts = packet->pts;
    int ret;
/*
    if(pts == AV_NOPTS_VALUE || pts < 0){
        if(!rk_context->ptsstep && avctx->framerate.den && avctx->framerate.num){
            int64_t x = avctx->pkt_timebase.den * (int64_t)avctx->framerate.den;
            int64_t y = avctx->pkt_timebase.num * (int64_t)avctx->framerate.num;
            rk_context->ptsstep = x / y;
        }
        if(rk_context->ptsstep && (packet->dts == AV_NOPTS_VALUE || packet->dts < 0)){
            pts = rk_context->pts;
            rk_context->pts += rk_context->ptsstep;
        } else {
            rk_context->pts = packet->dts;
            pts = packet->dts;
        }
    }
*/
    ret = mpp_packet_init(&mpkt, packet->data, packet->size);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init MPP packet (code = %d)\n", ret);
        return AVERROR_UNKNOWN;
    }

    mpp_packet_set_pts(mpkt, pts);

    ret = rk_context->mpi->decode_put_packet(rk_context->ctx, mpkt);
    mpp_packet_deinit(&mpkt);

    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_TRACE, "Decoder packet buffer full\n");
        return AVERROR(EAGAIN);
    }

    av_log(avctx, AV_LOG_DEBUG, "Wrote %d bytes to decoder\n", packet->size);
    return 0;
}

static int rkmpp_send_eos(AVCodecContext *avctx)
{
    RKMPPCodecContext *rk_context = avctx->priv_data;
    MppPacket mpkt;
    int ret;

    ret = mpp_packet_init(&mpkt, NULL, 0);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init EOS packet (code = %d)\n", ret);
        return AVERROR_UNKNOWN;
    }

    mpp_packet_set_eos(mpkt);

    do {
        ret = rk_context->mpi->decode_put_packet(rk_context->ctx, mpkt);
    } while (ret != MPP_OK);
    mpp_packet_deinit(&mpkt);

    return 0;
}

int rkmpp_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    AVCodecInternal *avci = avctx->internal;
    RKMPPCodecContext *rk_context = avctx->priv_data;
    AVPacket *packet = &rk_context->lastpacket;
    int ret_get_packet=0, ret_send_packet=0, ret_decode=0, ret_convert=0, ret_get_frame=0;

    if (!avci->draining){
        if(!packet->size){
            ret_get_packet = ff_decode_get_packet(avctx, packet);
            if(ret_get_packet == AVERROR_EOF){
                av_log(avctx, AV_LOG_VERBOSE, "Decoder Draining.\n");
                rkmpp_send_eos(avctx);
            } else if (ret_get_packet == AVERROR(EAGAIN))
                 av_log(avctx, AV_LOG_TRACE, "Decoder Can't get packet retrying.\n");
        }
send_loop:
        if(packet->size){
            ret_send_packet = rkmpp_send_packet(avctx, packet);
            if (ret_send_packet == 0){
                // send successful, continue until decoder input buffer is full
                av_packet_unref(packet);
                return AVERROR(EAGAIN);
            } else if (ret_send_packet < 0 && ret_send_packet != AVERROR(EAGAIN)) {
                // something went wrong, raise error
                av_log(avctx, AV_LOG_ERROR, "Decoder Failed to send data (code = %d)\n", ret_send_packet);
                return ret_send_packet;
            }
        }
    }

receive_loop:
    ret_decode = rkmpp_decode_frame(avctx, MPP_TIMEOUT_NON_BLOCK);
    ret_convert = rkmpp_convert_frame(avctx, frame);
    ret_get_frame = rkmpp_get_frame(avctx, frame);

    if(!ret_get_frame)
        return ret_get_frame;
    else if (ret_get_frame == AVERROR_EOF){
        av_log(avctx, AV_LOG_VERBOSE, "Decoder is at EOS.\n");
        return AVERROR_EOF;
    }

    if (ret_decode != AVERROR(EAGAIN) && ret_decode < 0){
        av_log(avctx, AV_LOG_ERROR, "Decoder Failed to decode frame (code = %d)\n", ret_decode);
        return ret_decode;
    }

    if (ret_convert != AVERROR(EAGAIN) && ret_convert < 0){
        av_log(avctx, AV_LOG_ERROR, "Decoder Failed to decode frame (code = %d)\n", ret_convert);
        return ret_convert;
    }

    if(ret_get_frame != AVERROR(EAGAIN) && ret_get_frame < 0){
        av_log(avctx, AV_LOG_TRACE, "Decoder Failed to getframe (code = %d)\n", ret_get_frame);
        return ret_get_frame;
    }

   if(ret_send_packet == AVERROR(EAGAIN) && ret_get_frame == AVERROR(EAGAIN))
       goto send_loop;
   else if(avci->draining){
       // some players (ie: kodi) are tricky, keep this here for sanity
       if(!rk_context->mpp_decode_fifo.used && !rk_context->mpp_rga_fifo.used)
           return AVERROR_EOF;
       goto receive_loop;
   }

   return AVERROR(EAGAIN);
}

RKMPP_DEC(h263, AV_CODEC_ID_H263, NULL)
RKMPP_DEC(h264, AV_CODEC_ID_H264, "h264_mp4toannexb")
RKMPP_DEC(hevc, AV_CODEC_ID_HEVC, "hevc_mp4toannexb")
RKMPP_DEC(av1, AV_CODEC_ID_AV1, NULL)
RKMPP_DEC(vp8,AV_CODEC_ID_VP8, NULL)
RKMPP_DEC(vp9, AV_CODEC_ID_VP9, NULL)
RKMPP_DEC(mpeg1, AV_CODEC_ID_MPEG1VIDEO, NULL)
RKMPP_DEC(mpeg2, AV_CODEC_ID_MPEG2VIDEO, NULL)
RKMPP_DEC(mpeg4,AV_CODEC_ID_MPEG4, "mpeg4_unpack_bframes")
