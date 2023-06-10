/*
 * RockChip MPP Video Decoder
 * Copyright (c) 2023 Huseyin BYIIK
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


/*
 * Mpp decoder/encoder outputs & inputs generally semi-planar pictures
 * FFmpeg on the other hand uses planar pictures. Normally libavfilter has
 * several filters to handle this conversion however it is not cpu affective
 * This file handles several plane conversion with hardware accelerated RGA chip in
 * rockchip socs, whenever there is a failure it falls back to libyuv which is SIMD
 * optimized plane conversion library. Failure is expected time to time, because
 * RGA is not always consistent in between kernel versions of rockchips downstream
 *
 * Normally both RGA and enhancements in libyuv should be a part or libavfilter, but
 * currently this is easier. May be in future someone take this up and move to avfilter.
 */

#include <fcntl.h>
#include "rkrga.h"
#include "rkmpp.h"
#include "rkplane.h"
#include "libavutil/log.h"
#include "libavutil/error.h"
#include "libyuv/planar_functions.h"
#include "libyuv/scale_uv.h"
#include "libyuv/scale.h"


#define AV_VSTRIDE(AVFRAME) (FFALIGN(AVFRAME->buf[0] && AVFRAME->buf[1] ? AVFRAME->buf[0]->size / AVFRAME->linesize[0] : (AVFRAME->data[1] - AVFRAME->data[0]) / AVFRAME->linesize[0], 16))

static int rga_scale(uint64_t rga_fd,
        uint64_t src_fd, uint64_t src_y, uint16_t src_width, uint16_t src_height, uint16_t src_hstride, uint16_t src_vstride,
        uint64_t dst_fd, uint64_t dst_y, uint16_t dst_width, uint16_t dst_height, uint16_t dst_hstride, uint16_t dst_vstride,
        uint32_t informat, uint32_t outformat){
    struct rga_req req = {
        .src = {
            // fd when the source is mmapped, 0 when non mmap
            .yrgb_addr = src_fd,
            // y addr offset (when fd)/addr, uv = y + hstride * vstride, v = uv + (hstride * vstride) / 4
            .uv_addr = src_y,
            .format = informat,
            .act_w = src_width,
            .act_h = src_height,
            .vir_w = src_hstride,
            .vir_h = src_vstride,
            .rd_mode = RGA_RASTER_MODE,
        },
        .dst = {
             // fd when the destination is mmapped, 0 when non mmap
            .yrgb_addr = dst_fd,
            // y addr offset (when fd)/addr, uv = y + hstride * vstride, v = uv + (hstride * vstride) / 4
            .uv_addr = dst_y,
            .format = outformat,
            .act_w = dst_width,
            .act_h = dst_height,
            .vir_w = dst_hstride,
            .vir_h = dst_vstride,
            .rd_mode = RGA_RASTER_MODE,
        },
        .mmu_info = {
            .mmu_en = 1,
            .mmu_flag = 0x80000521,
        },
    };

    return ioctl(rga_fd, RGA_BLIT_SYNC, &req);
}

static MppFrame avframe_to_mppframe(AVCodecContext *avctx, AVFrame *frame, MppBufferGroup buffer_group, int rga_format, int copy){
    MppFrame mppframe = NULL;
    MppBuffer mppbuffer = NULL;
    int ysize, size, ret, hstride, vstride;

    ret = mpp_frame_init(&mppframe);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init mpp frame %d\n", ret);
        goto clean;
     }

     mpp_frame_set_width(mppframe, frame->width);
     mpp_frame_set_height(mppframe, frame->height);

     hstride = FFALIGN(frame->width, RKMPP_STRIDE_ALIGN);
     if(rga_format == RGA_FORMAT_YCbCr_420_SP || RGA_FORMAT_YCbCr_420_P){
         vstride = FFALIGN(frame->height, RKMPP_STRIDE_ALIGN);
         ysize = hstride * vstride;
         size = ysize * 3 / 2;
     }

     mpp_frame_set_hor_stride(mppframe, hstride);
     mpp_frame_set_ver_stride(mppframe, vstride);

     ret = mpp_buffer_get(buffer_group, &mppbuffer, size);
     if (ret) {
         av_log(avctx, AV_LOG_ERROR, "failed to get buffer for input frame ret %d\n", ret);
         goto clean;
     }

     mpp_frame_set_buffer(mppframe, mppbuffer);
     mpp_frame_set_buf_size(mppframe, size);
     mpp_buffer_put(mppbuffer);
     if(copy){
         if (rga_format == RGA_FORMAT_YCbCr_420_SP){
             mpp_buffer_write(mppbuffer, 0, frame->data[0], frame->buf[0]->size);
             mpp_buffer_write(mppbuffer, ysize, frame->data[1], frame->buf[1]->size);
         } else if (rga_format == RGA_FORMAT_YCbCr_420_P){
             mpp_buffer_write(mppbuffer, 0, frame->data[0], frame->buf[0]->size);
             mpp_buffer_write(mppbuffer, ysize, frame->data[1], frame->buf[1]->size);
             mpp_buffer_write(mppbuffer, ysize * 5 / 4, frame->data[2], frame->buf[2]->size);
         }
     }

     return mppframe;

clean:
    if(mppbuffer)
        mpp_buffer_put(mppbuffer);
    if(mppframe)
        mpp_frame_deinit(&mppframe);
    return mppframe;
}

static int rga_convert_mpp_av(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame, int rga_informat, int rga_outformat){
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;
    MppBuffer buffer = mpp_frame_get_buffer(mppframe);

    if (!codec->norga && codec->rga_fd >= 0){
        if(rga_scale(codec->rga_fd,
                mpp_buffer_get_fd(buffer), 0,
                mpp_frame_get_width(mppframe), mpp_frame_get_height(mppframe),
                mpp_frame_get_hor_stride(mppframe),  mpp_frame_get_ver_stride(mppframe),
                0, (uint64_t) frame->data[0],
                frame->width, frame->height,
                frame->linesize[0], AV_VSTRIDE(frame),
                (uint32_t)rga_informat, (uint32_t)rga_outformat)){
                av_log(avctx, AV_LOG_WARNING, "RGA failed falling back to soft conversion\n");
                codec->norga = 1; // fallback to soft conversion
                return -1;
            }
   } else
       return -1;

    return 0;
}

static int rga_convert_av_mpp(AVCodecContext *avctx, AVFrame *frame, MppFrame mppframe, uint32_t rga_informat, uint32_t rga_outformat){
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;

    if (!codec->norga && codec->rga_fd >= 0){
        MppFrame in_mppframe = avframe_to_mppframe(avctx, frame, codec->buffer_group, rga_informat, 1);
        int ret;

        ret = rga_scale(codec->rga_fd,
                mpp_buffer_get_fd(mpp_frame_get_buffer(in_mppframe)), 0,
                mpp_frame_get_width(in_mppframe), mpp_frame_get_height(in_mppframe),
                mpp_frame_get_hor_stride(in_mppframe),  mpp_frame_get_ver_stride(in_mppframe),
                mpp_buffer_get_fd(mpp_frame_get_buffer(mppframe)), 0,
                mpp_frame_get_width(mppframe), mpp_frame_get_height(mppframe),
                mpp_frame_get_hor_stride(mppframe),  mpp_frame_get_ver_stride(mppframe),
                rga_informat, rga_outformat);

        mpp_frame_deinit(&in_mppframe);

        if (ret){
clean:
                av_log(avctx, AV_LOG_WARNING, "RGA failed falling back to soft conversion\n");
                codec->norga = 1; // fallback to soft conversion
                return -1;
            }
   } else
       return -1;

    return 0;
}

// for decoder
int mpp_nv12_av_yuv420p(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame){
    // if there is no avbuffer for frame, claim it
    if(!frame->buf[0])
        ff_get_buffer(avctx, frame, 0);

    // try at least once with rga
    if(rga_convert_mpp_av(avctx, mppframe, frame, RGA_FORMAT_YCbCr_420_SP, RGA_FORMAT_YCbCr_420_P)){
        MppBuffer buffer = mpp_frame_get_buffer(mppframe);
        int hstride = mpp_frame_get_hor_stride(mppframe);
        int vstride = mpp_frame_get_ver_stride(mppframe);
        frame->data[0] = mpp_buffer_get_ptr(buffer); // use existing y plane
        frame->linesize[0] = hstride; // just in case mpp and avframe calculates to different strides
        // convert only uv plane from semi-planar to planar
        SplitUVPlane(mpp_buffer_get_ptr(buffer) + hstride * vstride, hstride,
                frame->data[1], frame->linesize[1], frame->data[2], frame->linesize[2],
                (frame->width + 1) >> 1, (frame->height + 1) >> 1);
        return 0;
    } else
        return 0;

    return -1;
}

//for decoder
int mpp_nv15_av_yuv420p(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame){
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;
    MppFrame nv12frame = NULL;
    int ret;

    if(!frame->buf[0])
        ff_get_buffer(avctx, frame, 0);

    nv12frame = avframe_to_mppframe(avctx, frame, codec->buffer_group, RGA_FORMAT_YCbCr_420_SP, 0);
    // rga1 which supports yuv420P output does not support nv15 input
    // therefore this first converts NV15->NV12 with rga2 than NV12 -> yuv420P with rga1
    ret = rga_scale(codec->rga_fd,
            mpp_buffer_get_fd(mpp_frame_get_buffer(mppframe)), 0,
            mpp_frame_get_width(mppframe), mpp_frame_get_height(mppframe),
            mpp_frame_get_hor_stride(mppframe),  mpp_frame_get_ver_stride(mppframe),
            mpp_buffer_get_fd(mpp_frame_get_buffer(nv12frame)), 0,
            mpp_frame_get_width(nv12frame), mpp_frame_get_height(nv12frame),
            mpp_frame_get_hor_stride(nv12frame),  mpp_frame_get_ver_stride(nv12frame),
            RGA_FORMAT_YCbCr_420_SP_10B, RGA_FORMAT_YCbCr_420_SP);

    if(ret){
        av_log(avctx, AV_LOG_ERROR, "RGA failed to convert NV15 -> NV12. No Soft Conversion Possible\n");
        goto clean;
    }

    ret = mpp_nv12_av_yuv420p(avctx, nv12frame, frame);

    // now another mppframe with nv12 is used for conversion, there is still possibility
    // that soft conversion will depend on NV12 frame's y plane. Therefore we have keep
    // it in mem until used.
    if(!ret){
        frame->buf[2] = set_mppframe_to_avbuff(nv12frame);
        if(frame->buf[2])
            return ret;
    }

    ret = -1;
clean:
    mpp_frame_deinit(&nv12frame);
    return ret;
}

// for decoder
int mpp_nv16_av_yuv420p(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame){
    MppBuffer buffer = mpp_frame_get_buffer(mppframe);
    int hstride = mpp_frame_get_hor_stride(mppframe);
    int vstride = mpp_frame_get_ver_stride(mppframe);
    char *src = mpp_buffer_get_ptr(buffer);
    // if there is no avbuffer for frame, claim it
    if(!frame->buf[0])
        ff_get_buffer(avctx, frame, 0);

    // try at least once with rga
    if(rga_convert_mpp_av(avctx, mppframe, frame, RGA_FORMAT_YCbCr_422_SP, RGA_FORMAT_YCbCr_420_P)){
        // scale down uv plane by 2 and write it to y plane of avbuffer temporarily
        src += hstride * vstride;
        UVScale(src, hstride, frame->width, frame->height,
                frame->data[0], hstride,
                (frame->width + 1) >> 1, (frame->height + 1) >> 1, kFilterNone);

        // just in case mpp and avframe calculates to different strides
        frame->linesize[0] = hstride;

        // convert uv plane from semi-planar to planar
        SplitUVPlane(frame->data[0], hstride,
                frame->data[1], frame->linesize[1], frame->data[2], frame->linesize[2],
                (frame->width + 1) >> 1, (frame->height + 1) >> 1);

        // use existing y plane from mppbuffer
        frame->data[0] = mpp_buffer_get_ptr(buffer);
        return 0;
    } else
        return 0;

    return -1;
}

//for decoder
int mpp_nv15_av_nv12(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame){
    // if there is no avbuffer for frame, claim it
    if(!frame->buf[0])
        ff_get_buffer(avctx, frame, 0);

    if(rga_convert_mpp_av(avctx, mppframe, frame, RGA_FORMAT_YCbCr_420_SP_10B, RGA_FORMAT_YCbCr_420_SP))
        av_log(avctx, AV_LOG_ERROR, "RGA failed to convert NV15 -> NV12. No Soft Conversion Possible\n");
    return 0;
}

//for decoder
int mpp_nv16_av_nv12(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame){
    MppBuffer buffer = mpp_frame_get_buffer(mppframe);
    int hstride = mpp_frame_get_hor_stride(mppframe);
    int vstride = mpp_frame_get_ver_stride(mppframe);
    char *src = mpp_buffer_get_ptr(buffer);
    // if there is no avbuffer for frame, claim it
    if(!frame->buf[0])
        ff_get_buffer(avctx, frame, 0);

    if(rga_convert_mpp_av(avctx, mppframe, frame, RGA_FORMAT_YCbCr_422_SP, RGA_FORMAT_YCbCr_420_SP)){
        // scale down uv plane by 2 and write it to uv plane of avbuffer
        src += hstride * vstride;
        UVScale(src, hstride, frame->width, frame->height,
                frame->data[1], hstride,
                (frame->width + 1) >> 1, (frame->height + 1) >> 1, kFilterNone);

        frame->linesize[0] = hstride;
        frame->linesize[1] = hstride;

        // use existing y plane from mppbuffer
        frame->data[0] = mpp_buffer_get_ptr(buffer);
        return 0;
    } else
        return 0;

    return -1;
}

// for encoder
MppFrame av_yuv420p_mpp_nv12(AVCodecContext *avctx, AVFrame *frame){
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;
    MppFrame out_mppframe = avframe_to_mppframe(avctx, frame, codec->buffer_group, RGA_FORMAT_YCbCr_420_SP, 0);
    if(rga_convert_av_mpp(avctx, frame, out_mppframe, RGA_FORMAT_YCbCr_420_P, RGA_FORMAT_YCbCr_420_SP)){
            MppBuffer out_buffer = mpp_frame_get_buffer(out_mppframe);
            int planesize = mpp_frame_get_hor_stride(out_mppframe) * mpp_frame_get_ver_stride(out_mppframe);
            char *dst = (char *) mpp_buffer_get_ptr(out_buffer) + planesize;
            //copy y plane directly
            mpp_buffer_write(out_buffer, 0, frame->data[0], planesize);
            // convert planar to semi-planar
            MergeUVPlane(frame->data[1], frame->linesize[1], frame->data[2], frame->linesize[2], dst, frame->linesize[0],
                    (frame->width + 1) >> 1, (frame->height + 1) >> 1);
        }

    return out_mppframe;
}

// for encoder
MppFrame av_nv12_mpp_nv12(AVCodecContext *avctx, AVFrame *frame){
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;
    MppFrame mppframe = avframe_to_mppframe(avctx, frame, codec->buffer_group, RGA_FORMAT_YCbCr_420_SP, 1);
    return mppframe;
}
