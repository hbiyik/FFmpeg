/*
 * RockChip MPP Plane Conversions
 * Copyright (c) 2023 Huseyin BYIIK
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
#include "rkmpp.h"
#include "rkplane.h"
#include "libyuv/planar_functions.h"
#include "libyuv/scale_uv.h"
#include "libyuv/scale.h"


#define AV_VSTRIDE(AVFRAME) (FFALIGN(AVFRAME->buf[0] && AVFRAME->buf[1] ? AVFRAME->buf[0]->size / AVFRAME->linesize[0] : (AVFRAME->data[1] - AVFRAME->data[0]) / AVFRAME->linesize[0], 16))

static void rkmpp_release_mppframe(void *opaque, uint8_t *data)
{
    MppFrame mppframe = opaque;
    mpp_frame_deinit(&mppframe);
}

static void rkmpp_release_drm_desc(void *opaque, uint8_t *data)
{
    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)opaque;
    av_free(desc);
}


static int set_mppframe_to_avbuff(MppFrame mppframe, AVFrame * frame){
    int i;

    for(i=0; i<5; i++){
        if(i > 3)
            return -1;
        else if(!frame->buf[i])
            break;
    }

    frame->buf[i] = av_buffer_create(mppframe, mpp_frame_get_buf_size(mppframe),
            rkmpp_release_mppframe, mppframe, AV_BUFFER_FLAG_READONLY);

    return i;
}

static int set_drmdesc_to_avbuff(AVDRMFrameDescriptor *desc, AVFrame *frame){
    int i;

    for(i=0; i<5; i++){
        if(i > 3)
            return -1;
        else if(!frame->buf[i])
            break;
    }

    frame->buf[i] = av_buffer_create((unsigned char *) desc, sizeof(AVDRMFrameDescriptor),
            rkmpp_release_drm_desc, desc, AV_BUFFER_FLAG_READONLY);

    return i;
}
static int rga_scale(uint64_t rga_fd,
        uint64_t src_fd, uint64_t src_y, uint16_t src_width, uint16_t src_height, uint16_t src_hstride, uint16_t src_vstride,
        uint64_t dst_fd, uint64_t dst_y, uint16_t dst_width, uint16_t dst_height, uint16_t dst_hstride, uint16_t dst_vstride,
        enum rga_surf_format informat, enum rga_surf_format outformat){
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

int rga_convert_mpp_mpp(AVCodecContext *avctx, MppFrame in_mppframe, MppFrame out_mppframe){
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;
    rkformat informat, outformat;

    if (!codec->norga && codec->rga_fd >= 0){
        if(!out_mppframe)
            return -1;
        rkmpp_get_mpp_format(&informat, mpp_frame_get_fmt(in_mppframe) & MPP_FRAME_FMT_MASK);
        rkmpp_get_mpp_format(&outformat, mpp_frame_get_fmt(out_mppframe) & MPP_FRAME_FMT_MASK);
        if(rga_scale(codec->rga_fd,
            mpp_buffer_get_fd(mpp_frame_get_buffer(in_mppframe)), 0,
            mpp_frame_get_width(in_mppframe), mpp_frame_get_height(in_mppframe),
            mpp_frame_get_hor_stride(in_mppframe),  mpp_frame_get_ver_stride(in_mppframe),
            mpp_buffer_get_fd(mpp_frame_get_buffer(out_mppframe)), 0,
            mpp_frame_get_width(out_mppframe), mpp_frame_get_height(out_mppframe),
            mpp_frame_get_hor_stride(out_mppframe),  mpp_frame_get_ver_stride(out_mppframe),
            informat.rga,
            outformat.rga)){
                av_log(avctx, AV_LOG_WARNING, "RGA failed falling back to soft conversion\n");
                codec->norga = 1; // fallback to soft conversion
                return -1;
        } else
            return 0;
    }

    return -1;
}

static int rga_convert_mpp_av(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame,
        enum AVPixelFormat informat, enum AVPixelFormat outformat){
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;
    MppBuffer buffer = mpp_frame_get_buffer(mppframe);
    rkformat inrkformat, outrkformat;

    rkmpp_get_av_format(&inrkformat, informat);
    rkmpp_get_av_format(&outrkformat, outformat);

    if (!codec->norga && codec->rga_fd >= 0){
        if(rga_scale(codec->rga_fd,
                mpp_buffer_get_fd(buffer), 0,
                mpp_frame_get_width(mppframe), mpp_frame_get_height(mppframe),
                mpp_frame_get_hor_stride(mppframe),  mpp_frame_get_ver_stride(mppframe),
                0, (uint64_t) frame->data[0],
                frame->width, frame->height,
                frame->linesize[0], AV_VSTRIDE(frame),
                inrkformat.rga, outrkformat.rga)){
                    av_log(avctx, AV_LOG_WARNING, "RGA failed falling back to soft conversion\n");
                    codec->norga = 1; // fallback to soft conversion
                    return -1;
            }
   } else
       return -1;

    return 0;
}

static int mpp_nv12_av_yuv420p_soft(MppFrame mppframe, AVFrame *frame){
    // warning: mpp frame must not be released until displayed
    MppBuffer buffer = mpp_frame_get_buffer(mppframe);
    int hstride = mpp_frame_get_hor_stride(mppframe);
    int vstride = mpp_frame_get_ver_stride(mppframe);
    int ret;

    frame->data[0] = mpp_buffer_get_ptr(buffer); // use existing y plane
    frame->linesize[0] = hstride;

    // convert only uv plane from semi-planar to planar
    SplitUVPlane(frame->data[0] + hstride * vstride, hstride,
            frame->data[1], frame->linesize[1], frame->data[2], frame->linesize[2],
            (frame->width + 1) >> 1, (frame->height + 1) >> 1);

    ret = set_mppframe_to_avbuff(mppframe, frame);
    if(ret >= 0)
        frame->data[RKMPP_MPPFRAME_BUFINDEX] = frame->buf[ret]->data;
    return ret;
}

static int mpp_nv16_av_yuv420p_soft(MppFrame mppframe, AVFrame *frame){
    // warning: mpp frame must not be released until displayed
    MppBuffer buffer = mpp_frame_get_buffer(mppframe);
    int hstride = mpp_frame_get_hor_stride(mppframe);
    int vstride = mpp_frame_get_ver_stride(mppframe);
    char *src = (char *)mpp_buffer_get_ptr(buffer) + hstride * vstride;
    int ret;

    // scale down uv plane by 2 and write it to y plane of avbuffer temporarily
    UVScale(src, hstride, frame->width, frame->height,
            frame->data[0], frame->linesize[0],
            (frame->width + 1) >> 1, (frame->height + 1) >> 1, kFilterNone);

    // convert uv plane from semi-planar to planar
    SplitUVPlane(frame->data[0], frame->linesize[0],
            frame->data[1], frame->linesize[1], frame->data[2], frame->linesize[2],
            (frame->width + 1) >> 1, (frame->height + 1) >> 1);

    // use existing y plane from mppbuffer
    frame->data[0] = mpp_buffer_get_ptr(buffer);
    frame->linesize[0] = hstride;

    ret = set_mppframe_to_avbuff(mppframe, frame);
    if(ret >= 0)
        frame->data[RKMPP_MPPFRAME_BUFINDEX] = frame->buf[ret]->data;
    return ret;
}

static int mpp_nv16_av_nv12_soft(MppFrame mppframe, AVFrame *frame){
    // warning: mpp frame must not be released until displayed
    MppBuffer buffer = mpp_frame_get_buffer(mppframe);
    int hstride = mpp_frame_get_hor_stride(mppframe);
    int vstride = mpp_frame_get_ver_stride(mppframe);
    char *src = (char *)mpp_buffer_get_ptr(buffer) + hstride * vstride;
    int ret;

    // scale down uv plane by 2 and write it to uv plane of avbuffer
    UVScale(src, hstride, frame->width, frame->height,
            frame->data[1], frame->linesize[0],
            (frame->width + 1) >> 1, (frame->height + 1) >> 1, kFilterNone);

    // use existing y plane from mppbuffer
    frame->data[0] = mpp_buffer_get_ptr(buffer);
    frame->linesize[0] = hstride;

    ret = set_mppframe_to_avbuff(mppframe, frame);
    if(ret >= 0)
        frame->data[RKMPP_MPPFRAME_BUFINDEX] = frame->buf[ret]->data;
    return ret;
}

MppFrame create_mpp_frame(int width, int height, enum AVPixelFormat avformat, MppBufferGroup buffer_group, AVDRMFrameDescriptor *desc, AVFrame *frame){
    MppFrame mppframe = NULL;
    MppBuffer mppbuffer = NULL;
    rkformat format;
    int size, ret, hstride, vstride, planes;
    int planesizes[3];

    ret = mpp_frame_init(&mppframe);

    if (ret) {
        goto clean;
     }

    vstride = FFALIGN(height, RKMPP_STRIDE_ALIGN);

    switch(avformat){
    case AV_PIX_FMT_NV12:
        hstride = FFALIGN(width, RKMPP_STRIDE_ALIGN);
        planesizes[0] = hstride * vstride; // y plane
        planesizes[1] = (planesizes[0] + 1) >> 1; // u+v plane
        size = planesizes[0] + planesizes[1];
        planes = 2;
        break;
    case AV_PIX_FMT_YUV420P:
        hstride = FFALIGN(width, RKMPP_STRIDE_ALIGN);
        planesizes[0] = hstride * vstride; // y plane
        planesizes[1] = (planesizes[0] + 1) >> 2; // u plane
        planesizes[2] = planesizes[1]; // v plane
        size = planesizes[0] + planesizes[1] + planesizes[2];
        planes = 3;
        break;
    case AV_PIX_FMT_NV16:
        hstride = FFALIGN(width, RKMPP_STRIDE_ALIGN);
        planesizes[0] = hstride * vstride; // y plane
        planesizes[1] = planesizes[0]; // u+v plane
        size = planesizes[0] + planesizes[1];
        planes = 2;
        break;
    case AV_PIX_FMT_YUV422P:
        hstride = FFALIGN(width, RKMPP_STRIDE_ALIGN);
        planesizes[0] = hstride * vstride; // y plane
        planesizes[1] = (planesizes[0] + 1) >> 1; // u plane
        planesizes[2] = planesizes[1]; // v plane
        size = planesizes[0] + planesizes[1] + planesizes[2];
        planes = 3;
        break;
    case AV_PIX_FMT_NV24:
        hstride = FFALIGN(width, RKMPP_STRIDE_ALIGN);
        planesizes[0] = hstride * vstride; // y plane
        planesizes[1] = planesizes[0] * 2; // u+v plane
        size = planesizes[0] + planesizes[1];
        planes = 2;
        break;
    case AV_PIX_FMT_YUV444P:
        hstride = FFALIGN(width, RKMPP_STRIDE_ALIGN);
        planesizes[0] = hstride * vstride; // y plane
        planesizes[1] = planesizes[0]; // u plane
        planesizes[2] = planesizes[1]; // v plane
        size = planesizes[0] + planesizes[1] + planesizes[2];
        planes = 3;
        break;
    case AV_PIX_FMT_YUYV422:
    case AV_PIX_FMT_UYVY422:
        hstride = FFALIGN(width * 2, RKMPP_STRIDE_ALIGN);
        size = hstride * vstride;
        planesizes[0] = size; // whole plane
        planes = 1;
        break;
    case AV_PIX_FMT_RGB24:
    case AV_PIX_FMT_BGR24:
        hstride = FFALIGN(width * 3, RKMPP_STRIDE_ALIGN);
        size = hstride * vstride;
        planesizes[0] = size; // whole plane
        planes = 1;
        break;
    case AV_PIX_FMT_0RGB:
    case AV_PIX_FMT_0BGR:
    case AV_PIX_FMT_BGR0:
    case AV_PIX_FMT_RGB0:
    case AV_PIX_FMT_ARGB:
    case AV_PIX_FMT_ABGR:
    case AV_PIX_FMT_BGRA:
    case AV_PIX_FMT_RGBA:
        hstride = FFALIGN(width * 4, RKMPP_STRIDE_ALIGN);
        size = hstride * vstride;
        planesizes[0] = size; // whole plane
        planes = 1;
        break;
    }


    if(desc){
        MppBufferInfo info;
        AVDRMLayerDescriptor *layer = &desc->layers[0];

        size = FFMIN(desc->objects[0].size, size);
        //size = desc->objects[0].size;
        //hstride =layer->planes[0].pitch;
        //vstride = height;

        memset(&info, 0, sizeof(info));
        info.type   = MPP_BUFFER_TYPE_DRM;
        info.size   = size;
        info.fd     = desc->objects[0].fd;

        ret = mpp_buffer_import(&mppbuffer, &info);
        rkmpp_get_drm_format(&format, layer->format);
    } else {
        ret = mpp_buffer_get(buffer_group, &mppbuffer, size);
        rkmpp_get_av_format(&format, avformat);
    }

     if (ret)
         goto clean;

     mpp_frame_set_width(mppframe, width);
     mpp_frame_set_height(mppframe, height);
     mpp_frame_set_fmt(mppframe, format.mpp);
     mpp_frame_set_hor_stride(mppframe, hstride);
     mpp_frame_set_ver_stride(mppframe, vstride);
     mpp_frame_set_buffer(mppframe, mppbuffer);
     mpp_frame_set_buf_size(mppframe, size);
     mpp_buffer_put(mppbuffer);

     if(frame){
         //copy frame to mppframe
         int offset = 0;
         int bufnum = 0;
         int plane_size;
         for(int i = 0; i < planes; i++){
             if(frame->buf[i]){
                 // each plane has its own buffer
                 bufnum = i;
                 plane_size = FFMIN(frame->buf[bufnum]->size, planesizes[i]);
             } else if (i + 1 == planes){
                 // each plane has 1 buffer and its last plane: lastplanesize = buffersize - (currentaddr-firstaddr)
                 plane_size = FFMIN(frame->buf[bufnum]->size - (uintptr_t )frame->data[i] + (uintptr_t )frame->data[0], planesizes[i]);
             } else {
                 // each plane has 1 buffer and it not last or first plane
                 plane_size = planesizes[i];
             }
             mpp_buffer_write(mppbuffer, offset, frame->data[i], plane_size);
             offset += planesizes[i];
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
//for decoder
int mpp_nv15_av_yuv420p(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame){
    // rga1 which supports yuv420P output does not support nv15 input
    // therefore this first converts NV15->NV12 with rga2 than NV12 -> yuv420P with libyuv
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;
    MppFrame nv12frame = create_mpp_frame(mpp_frame_get_width(mppframe), mpp_frame_get_height(mppframe),
            AV_PIX_FMT_NV12, codec->buffer_group, NULL, NULL);
    int ret = rga_convert_mpp_mpp(avctx, mppframe, nv12frame);

    rkmpp_release_mppframe(mppframe, NULL);

    if(!ret){
        // if there is no avbuffer for frame, claim it
        if(!frame->buf[0])
            ff_get_buffer(avctx, frame, 0);
        // due to hdr being mostly 8k, rga1 the only rga support yuv420p output
        // wont convert this, therefore always use soft conv.
        ret = mpp_nv12_av_yuv420p_soft(nv12frame, frame);
    } else {
        if(nv12frame)
            rkmpp_release_mppframe(nv12frame, NULL);
        av_log(avctx, AV_LOG_ERROR, "RGA failed to convert NV15 -> YUV420P. No Soft Conversion Possible\n");
    }

    return ret;
}

//for decoder
int mpp_nv12_av_nv12(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame){
    MppBuffer buffer = mpp_frame_get_buffer(mppframe);
    int hstride = mpp_frame_get_hor_stride(mppframe);
    int vstride = mpp_frame_get_ver_stride(mppframe);
    int ret;

    frame->data[0] = mpp_buffer_get_ptr(buffer); // y
    frame->data[1] = frame->data[0] + hstride * vstride; // u + v
    frame->extended_data = frame->data;

    frame->linesize[0] = hstride;
    frame->linesize[1] = hstride;

    ret = set_mppframe_to_avbuff(mppframe, frame);
    if(ret >= 0)
        frame->data[RKMPP_MPPFRAME_BUFINDEX] = frame->buf[ret]->data;
    return ret;
}
//for decoder
int mpp_nv15_av_nv12(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame){
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;
    MppFrame nv12frame = create_mpp_frame(mpp_frame_get_width(mppframe), mpp_frame_get_height(mppframe),
            AV_PIX_FMT_NV12, codec->buffer_group, NULL, NULL);
    int ret = rga_convert_mpp_mpp(avctx, mppframe, nv12frame);

    rkmpp_release_mppframe(mppframe, NULL);

    if(!ret){
        ret = mpp_nv12_av_nv12(avctx, nv12frame, frame);
    } else {
        if(nv12frame)
            rkmpp_release_mppframe(nv12frame, NULL);
        av_log(avctx, AV_LOG_ERROR, "RGA failed to convert NV15 -> NV12. No Soft Conversion Possible\n");
    }

    return ret;
}

int convert_mpp_to_av(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame,
        enum AVPixelFormat informat, enum AVPixelFormat outformat){
    int ret = 0;

    if(!frame->buf[0])
        ff_get_buffer(avctx, frame, 0);

    if(rga_convert_mpp_av(avctx, mppframe, frame, informat, outformat)){
        if (informat == AV_PIX_FMT_NV16 && outformat == AV_PIX_FMT_NV12)
            ret = mpp_nv16_av_nv12_soft(mppframe, frame);
        else if (informat == AV_PIX_FMT_NV16 && outformat == AV_PIX_FMT_YUV420P)
            ret = mpp_nv16_av_yuv420p_soft(mppframe, frame);
        else if (informat == AV_PIX_FMT_NV12 && outformat == AV_PIX_FMT_YUV420P)
            ret = mpp_nv12_av_yuv420p_soft(mppframe, frame);
        else {
            ret = -1;
            av_log(avctx, AV_LOG_ERROR, "No software conversion for %s -> %s available\n",
                    av_get_pix_fmt_name(informat), av_get_pix_fmt_name(outformat));
        }
    } else{
        ret = set_mppframe_to_avbuff(mppframe, frame);
        if(ret >= 0)
            frame->data[RKMPP_MPPFRAME_BUFINDEX] = frame->buf[ret]->data;
    }

    if (ret < 0)
        rkmpp_release_mppframe(mppframe, NULL);
    return ret;
}

MppFrame import_drm_to_mpp(AVCodecContext *avctx, AVFrame *frame){
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;
    MppFrame mppframe = NULL;
    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor*) frame->data[0];
    AVDRMLayerDescriptor *layer = &desc->layers[0];
    rkformat format;

    rkmpp_get_drm_format(&format, layer->format);

    if(format.drm == DRM_FORMAT_NV15){
        // encoder does not support 10bit frames, we down scale them to 8bit
        MppFrame nv15frame = create_mpp_frame(frame->width, frame->height, AV_PIX_FMT_NONE, NULL, desc, NULL);
        if(nv15frame){
            mppframe = create_mpp_frame(frame->width, frame->height, AV_PIX_FMT_NV12, codec->buffer_group, NULL, NULL);
            if(mppframe && rga_convert_mpp_mpp(avctx, nv15frame, mppframe))
                rkmpp_release_mppframe(mppframe, NULL);
            rkmpp_release_mppframe(nv15frame, NULL);
        }
    } else {
        mppframe = create_mpp_frame(frame->width, frame->height, format.av, NULL, desc, NULL);
    }

    return mppframe;
}

int import_mpp_to_drm(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame)
{
    // mppframe & desc is cleared when AVFrame is released
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;
    AVDRMFrameDescriptor *desc = NULL;
    AVDRMLayerDescriptor *layer = NULL;
    rkformat format;
    MppBuffer buffer = mpp_frame_get_buffer(mppframe);
    int hstride = mpp_frame_get_hor_stride(mppframe);
    int vstride = mpp_frame_get_ver_stride(mppframe);
    int ret;

    rkmpp_get_mpp_format(&format, mpp_frame_get_fmt(mppframe) & MPP_FRAME_FMT_MASK);

    if(set_mppframe_to_avbuff(mppframe, frame) < 0){
        ret = AVERROR(ENOMEM);
        goto error;
    }

    desc = av_mallocz(sizeof(AVDRMFrameDescriptor));
    if (!desc || set_drmdesc_to_avbuff(desc, frame) < 0) {
        ret = AVERROR(ENOMEM);
        goto error;
    }

    desc->nb_objects = 1;
    desc->objects[0].fd = mpp_buffer_get_fd(buffer);
    desc->objects[0].size = mpp_buffer_get_size(buffer);

    desc->nb_layers = 1;
    layer = &desc->layers[0];
    layer->format = format.drm;
    layer->nb_planes = 2;

    layer->planes[0].object_index = 0;
    layer->planes[0].offset = 0;
    layer->planes[0].pitch = hstride;

    layer->planes[1].object_index = 0;
    layer->planes[1].offset = hstride * vstride;
    layer->planes[1].pitch = hstride;

    frame->data[0]  = (uint8_t *)desc;

    frame->hw_frames_ctx = av_buffer_ref(codec->hwframes_ref);
    if (!frame->hw_frames_ctx) {
        ret = AVERROR(ENOMEM);
        goto error;
    }

    return 0;

error:
    av_log(avctx, AV_LOG_ERROR, "Memory Error during importing mpp frame to drmprime\n");
    if (mppframe)
        rkmpp_release_mppframe(mppframe, NULL);
    if (desc)
        rkmpp_release_drm_desc(desc, NULL);

    return ret;
}

MppFrame get_mppframe_from_av(AVFrame *frame){
    if(frame->data[RKMPP_MPPFRAME_BUFINDEX]){
        rkmpp_frame_type * mppframe = (rkmpp_frame_type *) frame->data[RKMPP_MPPFRAME_BUFINDEX];
        if(mppframe->name && !strcmp(mppframe->name, "mpp_frame") &&
                mpp_frame_get_fmt(frame->data[RKMPP_MPPFRAME_BUFINDEX]) != MPP_FMT_YUV420SP_10BIT)
            return frame->data[RKMPP_MPPFRAME_BUFINDEX];
    }
    return NULL;
}
