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


static int set_mppframe_to_avbuff(MppFrame mppframe, AVFrame * frame, int index){
    int i;

    // find the first available buffer in  [buf[0], buf[4]]
    for(i=0; i<5; i++){
        if(i > 3)
            return -1;
        else if(!frame->buf[i])
            break;
    }

    frame->buf[i] = av_buffer_create(mppframe, mpp_frame_get_buf_size(mppframe),
            rkmpp_release_mppframe, mppframe, AV_BUFFER_FLAG_READONLY);

    if(i >= 0){
        if(index >= 0)
            frame->data[index] = frame->buf[i]->data;
        return 0;
    }

    return -1;
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

static int rga_scale(uint64_t src_fd, uint64_t src_y, uint16_t src_width, uint16_t src_height, uint16_t src_hstride, uint16_t src_vstride,
        uint64_t dst_fd, uint64_t dst_y, uint16_t dst_width, uint16_t dst_height, uint16_t dst_hstride, uint16_t dst_vstride,
        enum _Rga_SURF_FORMAT informat, enum _Rga_SURF_FORMAT outformat){
    rga_info_t src = {0};
    rga_info_t dst = {0};

    if(dst_hstride < dst_width)
        dst_width = FFALIGN(dst_width, RKMPP_STRIDE_ALIGN);
    if(dst_vstride < dst_height)
        dst_height = FFALIGN(dst_height, RKMPP_STRIDE_ALIGN);
    if(src_hstride < src_width)
        src_width = FFALIGN(src_width, RKMPP_STRIDE_ALIGN);
    if(src_vstride < src_height)
        src_height = FFALIGN(src_height, RKMPP_STRIDE_ALIGN);

    src.fd = src_fd;
    src.virAddr = (void *)src_y;
    src.mmuFlag = 1;
    src.format = informat;
    rga_set_rect(&src.rect, 0, 0,
            src_width, src_height, src_hstride, src_vstride, informat);

    dst.fd = dst_fd;
    dst.virAddr = (void *)dst_y;
    dst.mmuFlag = 1;
    dst.format = outformat;
    rga_set_rect(&dst.rect, 0, 0,
            dst_width, dst_height, dst_hstride, dst_vstride, outformat);

    return c_RkRgaBlit(&src, &dst, NULL);
}

int rga_convert_mpp_mpp(AVCodecContext *avctx, MppFrame in_mppframe, MppFrame out_mppframe){
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;
    rkformat informat, outformat;

    if (!codec->norga){
        if(!out_mppframe)
            return -1;
        rkmpp_get_mpp_format(&informat, mpp_frame_get_fmt(in_mppframe) & MPP_FRAME_FMT_MASK);
        rkmpp_get_mpp_format(&outformat, mpp_frame_get_fmt(out_mppframe) & MPP_FRAME_FMT_MASK);
        if(rga_scale(mpp_buffer_get_fd(mpp_frame_get_buffer(in_mppframe)), 0,
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

static void mpp_nv12_av_yuv420p_soft(MppFrame mppframe, AVFrame *frame){
    // warning: mpp frame must not be released until displayed
    MppBuffer buffer = mpp_frame_get_buffer(mppframe);
    int hstride = mpp_frame_get_hor_stride(mppframe);
    int vstride = mpp_frame_get_ver_stride(mppframe);

    frame->data[0] = mpp_buffer_get_ptr(buffer); // use existing y plane
    frame->linesize[0] = hstride;

    // convert only uv plane from semi-planar to planar
    SplitUVPlane(frame->data[0] + hstride * vstride, hstride,
            frame->data[1], frame->linesize[1], frame->data[2], frame->linesize[2],
            (frame->width + 1) >> 1, (frame->height + 1) >> 1);
}

static void mpp_nv16_av_yuv420p_soft(MppFrame mppframe, AVFrame *frame){
    // warning: mpp frame must not be released until displayed
    MppBuffer buffer = mpp_frame_get_buffer(mppframe);
    int hstride = mpp_frame_get_hor_stride(mppframe);
    int vstride = mpp_frame_get_ver_stride(mppframe);
    char *src = (char *)mpp_buffer_get_ptr(buffer) + hstride * vstride;

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
}

static void mpp_nv16_av_nv12_soft(MppFrame mppframe, AVFrame *frame){
    // warning: mpp frame must not be released until displayed
    MppBuffer buffer = mpp_frame_get_buffer(mppframe);
    int hstride = mpp_frame_get_hor_stride(mppframe);
    int vstride = mpp_frame_get_ver_stride(mppframe);
    char *src = (char *)mpp_buffer_get_ptr(buffer) + hstride * vstride;

    // scale down uv plane by 2 and write it to uv plane of avbuffer
    UVScale(src, hstride, frame->width, frame->height,
            frame->data[1], frame->linesize[0],
            (frame->width + 1) >> 1, (frame->height + 1) >> 1, kFilterNone);

    // use existing y plane from mppbuffer
    frame->data[0] = mpp_buffer_get_ptr(buffer);
    frame->linesize[0] = hstride;
}

static MppFrame wrap_mpp_to_avframe(AVCodecContext *avctx, AVFrame *frame, MppFrame targetframe){
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;
    MppBuffer targetbuffer = NULL;
    int planesize;

    if(!targetframe)
        targetframe = create_mpp_frame(avctx->width, avctx->height, avctx->pix_fmt, codec->buffer_group, NULL, NULL);

    if(!targetframe)
        return NULL;

    targetbuffer = mpp_frame_get_buffer(targetframe);
    planesize = mpp_frame_get_hor_stride(targetframe) * mpp_frame_get_ver_stride(targetframe);

    frame->data[0] = mpp_buffer_get_ptr(targetbuffer);
    frame->linesize[0] = mpp_frame_get_hor_stride(targetframe);
    frame->width = avctx->width;
    frame->height = avctx->height;
    frame->extended_data = frame->data;

    switch(avctx->pix_fmt){
    case AV_PIX_FMT_YUV420P:
        frame->data[1] = frame->data[0] + planesize;
        frame->linesize[1] = (frame->linesize[0] + 1) >> 1;
        frame->data[2] = frame->data[1] + ((planesize + 1) >> 2);
        frame->linesize[2] = frame->linesize[1];
        return targetframe;
    case AV_PIX_FMT_NV12:
        frame->data[1] = frame->data[0] + planesize;
        frame->linesize[1] = frame->linesize[0];
        return targetframe;
    }

    rkmpp_release_mppframe(targetframe, NULL);
    return NULL;
}

MppFrame create_mpp_frame(int width, int height, enum AVPixelFormat avformat, MppBufferGroup buffer_group, AVDRMFrameDescriptor *desc, AVFrame *frame){
    MppFrame mppframe = NULL;
    MppBuffer mppbuffer = NULL;
    rkformat format;
    int avmap[3][4]; //offset, dststride, width, height of max 3 planes
    int size, ret, hstride, vstride;
    int hstride_mult = 1;
    int planes = 2;
    int haspitch = 0;
    int overshoot = 1024;

    ret = mpp_frame_init(&mppframe);

    if (ret) {
        goto clean;
     }

    vstride = FFALIGN(height, RKMPP_STRIDE_ALIGN);

    switch(avformat){
    case AV_PIX_FMT_NV12:
        planes = 2;
        hstride = FFALIGN(width, RKMPP_STRIDE_ALIGN);
        // y plane
        avmap[0][0] = 0;
        avmap[0][1] = hstride;
        avmap[0][2] = width,
        avmap[0][3] = height;
        // uv plane
        avmap[1][0] = hstride * vstride; // uv offset = y plane size
        avmap[1][1] = hstride; // uv stride = hstride
        avmap[1][2] = width; // uv width = width
        avmap[1][3] = (height + 1)>> 1; // uv height = height / 2
        size = avmap[1][0] + ((avmap[1][0] + 1) >> 1) + overshoot; // total size = y+uv planesize
        break;
    case AV_PIX_FMT_YUV420P:
        planes = 3;
        hstride = FFALIGN(width, RKMPP_STRIDE_ALIGN);
        // y plane
        avmap[0][0] = 0;
        avmap[0][1] = hstride;
        avmap[0][2] = width,
        avmap[0][3] = height;
        // u plane
        avmap[1][0] = hstride * vstride; // u offset = y plane size
        avmap[1][1] = (hstride + 1)>> 1; // u stride = hstride / 2
        avmap[1][2] = (width + 1)>> 1; // u width = width / 2
        avmap[1][3] = (height + 1)>> 1; // u height = height / 2
        // v plane
        avmap[2][0] = avmap[1][0] + ((avmap[1][0] + 1) >> 2); // v offset = y+u plane size
        avmap[2][1] = avmap[1][1]; // v stride = hstride / 2
        avmap[2][2] = avmap[1][2]; // v width = width / 2
        avmap[2][3] = avmap[1][3]; // v height = height / 2
        size = avmap[2][0] + ((avmap[1][0] + 1) >> 2) + overshoot; // total size = y+u+v planesize
        break;
    case AV_PIX_FMT_NV16:
        planes = 2;
        hstride = FFALIGN(width, RKMPP_STRIDE_ALIGN);
        // y plane
        avmap[0][0] = 0;
        avmap[0][1] = hstride;
        avmap[0][2] = width,
        avmap[0][3] = height;
        // uv plane
        avmap[1][0] = hstride * vstride; // uv offset = y plane size
        avmap[1][1] = hstride; // uv stride = hstride
        avmap[1][2] = width; // uv width = width
        avmap[1][3] = height; // uv height = height
        size = avmap[1][0] * 2 + overshoot; // total size = y+uv planesize
        break;
    case AV_PIX_FMT_YUV422P:
        planes = 3;
        hstride = FFALIGN(width, RKMPP_STRIDE_ALIGN);
        //y plane
        avmap[0][0] = 0;
        avmap[0][1] = hstride;
        avmap[0][2] = width,
        avmap[0][3] = height;
        //u plane
        avmap[1][0] = hstride * vstride; // u offset = y plane size
        avmap[1][1] = (hstride + 1)>> 1; // u stride = hstride / 2
        avmap[1][2] = width; // u width = width
        avmap[1][3] = height; // u height = height
        //v plane
        avmap[2][0] = avmap[1][0] + ((avmap[1][0] + 1) >> 1); // v offset = y+u plane size
        avmap[2][1] = avmap[1][1]; // v stride = hstride
        avmap[2][2] = avmap[1][2]; // v width = width
        avmap[2][3] = avmap[1][3]; // v height = height / 2
        size = avmap[1][0] * 2 + overshoot; // total size = y+u+v planesize
        break;
    case AV_PIX_FMT_NV24:
        planes = 2;
        hstride = FFALIGN(width, RKMPP_STRIDE_ALIGN);
        // y plane
        avmap[0][0] = 0;
        avmap[0][1] = hstride;
        avmap[0][2] = width,
        avmap[0][3] = height;
        // uv plane
        avmap[1][0] = hstride * vstride; // uv offset = y plane size
        avmap[1][1] = hstride << 1; // uv stride = hstride * 2
        avmap[1][2] = width << 1; // uv width = width * 2
        avmap[1][3] = height; // uv height = height
        size = avmap[1][0] * 3 + overshoot; // total size = y+u+v planesize
        break;
    case AV_PIX_FMT_YUV444P:
        planes = 3;
        hstride = FFALIGN(width, RKMPP_STRIDE_ALIGN);
        //y plane
        avmap[0][0] = 0;
        avmap[0][1] = hstride;
        avmap[0][2] = width,
        avmap[0][3] = height;
        //u plane
        avmap[1][0] = hstride * vstride; // u offset = y plane size
        avmap[1][1] = hstride; // u stride = hstride
        avmap[1][2] = width; // u width = width
        avmap[1][3] = height; // u height = height
        //v plane
        avmap[2][0] = avmap[1][0] * 2; // v offset = y+u plane size
        avmap[2][1] = avmap[1][1]; // v stride = hstride
        avmap[2][2] = avmap[1][2]; // v width = width
        avmap[2][3] = avmap[1][3]; // v height = height
        size = avmap[1][0] * 3 + overshoot; // total size = y+u+v planesize
        break;
    case AV_PIX_FMT_YUYV422:
    case AV_PIX_FMT_UYVY422:
        planes = 1;
        haspitch = 1;
        hstride_mult = 2;
        hstride = FFALIGN(width * hstride_mult, RKMPP_STRIDE_ALIGN);
        avmap[0][0] = 0;
        avmap[0][1] = hstride;
        avmap[0][2] = width << 1,
        avmap[0][3] = height;
        size = hstride * vstride;
        break;
    case AV_PIX_FMT_RGB24:
    case AV_PIX_FMT_BGR24:
        haspitch = 1;
        hstride_mult = 3;
        hstride = FFALIGN(width * hstride_mult, RKMPP_STRIDE_ALIGN);
        avmap[0][0] = 0;
        avmap[0][1] = hstride;
        avmap[0][2] = width * 3,
        avmap[0][3] = height;
        size = hstride * vstride;
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
        haspitch = 1;
        hstride_mult = 4;
        hstride = FFALIGN(width * hstride_mult, RKMPP_STRIDE_ALIGN);
        avmap[0][0] = 0;
        avmap[0][1] = hstride;
        avmap[0][2] = width << 2,
        avmap[0][3] = height;
        size = hstride * vstride;
        planes = 1;
        break;
    }

    if(desc){
        MppBufferInfo info;
        AVDRMLayerDescriptor *layer = &desc->layers[0];
        rkmpp_get_drm_format(&format, layer->format);

        size = desc->objects[0].size;
        if(haspitch)
            hstride = layer->planes[0].pitch;
        else
            hstride = layer->planes[0].pitch * hstride_mult;

        if(planes == 1)
            vstride = size / hstride;
        else
            vstride = layer->planes[1].offset / hstride;

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
         for(int i = 0; i < planes; i++){
             CopyPlane(frame->data[i], frame->linesize[i],
                     (char *)mpp_buffer_get_ptr(mppbuffer) + avmap[i][0], avmap[i][1], avmap[i][2], avmap[i][3]);
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
int mpp_nv15_av_yuv420p(AVCodecContext *avctx, MppFrame nv15frame, AVFrame *frame){
    // rga1 which supports yuv420P output does not support nv15 input
    // therefore this first converts NV15->NV12 with rga2 than NV12 -> yuv420P with libyuv
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;
    MppFrame nv12frame = create_mpp_frame(mpp_frame_get_width(nv15frame), mpp_frame_get_height(nv15frame),
            AV_PIX_FMT_NV12, codec->buffer_group, NULL, NULL);
    MppFrame yuv420pframe = NULL;
    int ret = rga_convert_mpp_mpp(avctx, nv15frame, nv12frame);

    rkmpp_release_mppframe(nv15frame, NULL);

    if(!ret){
        MppFrame yuv420pframe = wrap_mpp_to_avframe(avctx, frame, NULL);
        if(yuv420pframe &&
                !set_mppframe_to_avbuff(nv12frame, frame, RKMPP_MPPFRAME_BUFINDEX) &&
                !set_mppframe_to_avbuff(yuv420pframe, frame, RKMPP_MPPFRAME_BUFINDEX - 1)){
                    mpp_nv12_av_yuv420p_soft(nv12frame, frame);
                    return 0;
        }
    }

    if(nv12frame)
        rkmpp_release_mppframe(nv12frame, NULL);
    if(yuv420pframe)
        rkmpp_release_mppframe(yuv420pframe, NULL);
    return -1;
}

//for decoder
int mpp_nv12_av_nv12(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame){
    if(wrap_mpp_to_avframe(avctx, frame, mppframe)){
        return set_mppframe_to_avbuff(mppframe, frame, RKMPP_MPPFRAME_BUFINDEX);
    }

    rkmpp_release_mppframe(mppframe, NULL);
    return -1;
}
//for decoder
int mpp_nv15_av_nv12(AVCodecContext *avctx, MppFrame nv15frame, AVFrame *frame){
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;
    MppFrame nv12frame = create_mpp_frame(mpp_frame_get_width(nv15frame), mpp_frame_get_height(nv15frame),
            AV_PIX_FMT_NV12, codec->buffer_group, NULL, NULL);
    int ret = rga_convert_mpp_mpp(avctx, nv15frame, nv12frame);

    rkmpp_release_mppframe(nv15frame, NULL);

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
    MppFrame targetframe = wrap_mpp_to_avframe(avctx, frame, NULL);
    int ret=0;

    if(!targetframe){
        rkmpp_release_mppframe(mppframe, NULL);
        return -1;
    }

    if(set_mppframe_to_avbuff(targetframe, frame, RKMPP_MPPFRAME_BUFINDEX - 1))
        return -1;
    if(set_mppframe_to_avbuff(mppframe, frame, RKMPP_MPPFRAME_BUFINDEX))
        return -1;

    if(rga_convert_mpp_mpp(avctx, mppframe, targetframe)){
        if (informat == AV_PIX_FMT_NV16 && outformat == AV_PIX_FMT_NV12)
            mpp_nv16_av_nv12_soft(mppframe, frame);
        else if (informat == AV_PIX_FMT_NV16 && outformat == AV_PIX_FMT_YUV420P)
            mpp_nv16_av_yuv420p_soft(mppframe, frame);
        else if (informat == AV_PIX_FMT_NV12 && outformat == AV_PIX_FMT_YUV420P)
            mpp_nv12_av_yuv420p_soft(mppframe, frame);
        else {
            ret = -1;
            av_log(avctx, AV_LOG_ERROR, "No software conversion for %s -> %s available\n",
                    av_get_pix_fmt_name(informat), av_get_pix_fmt_name(outformat));
        }
    }

    return ret;
}

MppFrame import_drm_to_mpp(AVCodecContext *avctx, AVFrame *frame){
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;
    MppFrame mppframe = NULL;
    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor*) frame->data[0];
    AVDRMLayerDescriptor *layer = &desc->layers[0];
    rkformat format;
    char drmname[4];
    DRMFORMATNAME(drmname, layer->format)

    if(rkmpp_get_drm_format(&format, layer->format)){
        av_log(avctx, AV_LOG_ERROR, "Unsupported DRM Format %s\n", drmname);
        return NULL;
    }

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

    if(set_mppframe_to_avbuff(mppframe, frame, -1)){
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
