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

#include <poll.h>
#include <fcntl.h>
#include "rkplane.h"
#include "libyuv/planar_functions.h"
#include "libyuv/scale_uv.h"
#include "libyuv/scale.h"

static rkformat rkformats[16] = {
        { .av = AV_PIX_FMT_BGR24, .mpp = MPP_FMT_BGR888, .drm = DRM_FORMAT_BGR888, .rga = RK_FORMAT_BGR_888,
                .numplanes = 1, .qual = 2, .factors[0] = { .width = 3}},
        { .av = AV_PIX_FMT_RGBA, .mpp = MPP_FMT_RGBA8888, .drm = DRM_FORMAT_ABGR8888, .rga = RK_FORMAT_RGBA_8888,
                .numplanes = 1, .qual = 2, .factors[0] = { .width = 4}},
        { .av = AV_PIX_FMT_RGB0, .mpp = MPP_FMT_RGBA8888, .drm = DRM_FORMAT_XBGR8888, .rga = RK_FORMAT_RGBX_8888,
                .numplanes = 1, .qual = 2, .factors[0] = { .width = 4}},
        { .av = AV_PIX_FMT_BGRA, .mpp = MPP_FMT_BGRA8888, .drm = DRM_FORMAT_ARGB8888, .rga = RK_FORMAT_BGRA_8888,
                .numplanes = 1, .qual = 2, .factors[0] = { .width = 4}},
        { .av = AV_PIX_FMT_BGR0, .mpp = MPP_FMT_BGRA8888, .drm = DRM_FORMAT_XRGB8888, .rga = RK_FORMAT_BGRX_8888,
                .numplanes = 1, .qual = 2, .factors[0] = { .width = 4}},
        { .av = AV_PIX_FMT_YUYV422, .mpp = MPP_FMT_YUV422_YUYV, .drm = DRM_FORMAT_YUYV, .rga = RK_FORMAT_YUYV_422,
                .numplanes = 1, .qual = 1, .factors[0] = { .width = 2}},
        { .av = AV_PIX_FMT_UYVY422, .mpp = MPP_FMT_YUV422_UYVY, .drm = DRM_FORMAT_UYVY, .rga = RK_FORMAT_UYVY_422,
                .numplanes = 1, .qual = 1, .factors[0] = { .width = 2}},
        { .av = AV_PIX_FMT_NV12, .mpp = MPP_FMT_YUV420SP, .drm = DRM_FORMAT_NV12, .drm_fbc=DRM_FORMAT_YUV420_8BIT, .rga = RK_FORMAT_YCbCr_420_SP,
                .numplanes = 2, .factors[1] = { .height_div = 2}},
        { .av = AV_PIX_FMT_NV15, .mpp = MPP_FMT_YUV420SP_10BIT, .drm = DRM_FORMAT_NV15, .drm_fbc=DRM_FORMAT_YUV420_10BIT, .rga = RK_FORMAT_YCbCr_420_SP_10B,
                .numplanes = 2, .qual = 10, .bpp = 5, .bpp_div = 4, .factors[1] = { .height_div = 2}},
        { .av = AV_PIX_FMT_P010LE, .mpp = MPP_FMT_BUTT, .drm = DRM_FORMAT_P010, .rga = RK_FORMAT_YCbCr_420_SP_10B,
                .numplanes = 2, .qual = 10, .bpp = 2, .factors[1] = { .height_div = 2}, .rga_uncompact = 1, .rga_msb_aligned = 1},
        { .av = AV_PIX_FMT_YUV420P, .mpp = MPP_FMT_YUV420P, .drm = DRM_FORMAT_YUV420, .rga = RK_FORMAT_YCbCr_420_P,
                .numplanes = 3, .factors[1] = { .width_div = 2, .height_div = 2, .hstride_div=2}},
        { .av = AV_PIX_FMT_YUV420P10LE, .mpp = MPP_FMT_BUTT, .drm = DRM_FORMAT_P010, .rga = RK_FORMAT_UNKNOWN,
                .numplanes = 3, .qual = 10, .bpp = 2, .factors[1] = { .width_div = 2, .height_div = 2, .hstride_div=2}},
        { .av = AV_PIX_FMT_NV16, .mpp = MPP_FMT_YUV422SP, .drm = DRM_FORMAT_NV16, .drm_fbc=DRM_FORMAT_YUV420_8BIT, .rga = RK_FORMAT_YCbCr_422_SP,
                .numplanes = 2, .qual = 1,},
        { .av = AV_PIX_FMT_YUV422P, .mpp = MPP_FMT_YUV422P, .drm = DRM_FORMAT_YUV422, .rga = RK_FORMAT_YCbCr_422_P,
                .numplanes = 3, .qual = 1, .factors[1] = { .hstride_div = 2}},
        { .av = AV_PIX_FMT_NV24, .mpp = MPP_FMT_YUV444SP, .drm = DRM_FORMAT_NV24, .rga = RK_FORMAT_UNKNOWN,
                .numplanes = 2, .qual = 2, .factors[1] = { .width = 2, .hstride = 2}},
        { .av = AV_PIX_FMT_YUV444P, .mpp = MPP_FMT_YUV444P, .drm = DRM_FORMAT_YUV444, .rga = RK_FORMAT_UNKNOWN,
                .numplanes = 3, .qual = 2}
};

#define GETFORMAT(NAME, TYPE)\
int rkmpp_get_##NAME##_format(rkformat *format, TYPE informat, int width, int height, int align,\
        int hstride, int vstride, int ypixoffset, int overshoot, int fbcstride, factor* uvfactor){ \
    for(int i=0; i < FF_ARRAY_ELEMS(rkformats); i++){ \
        if(rkformats[i].NAME == informat){ \
            rkmpp_planedata(&rkformats[i], width, height, align, hstride, vstride, ypixoffset, overshoot, fbcstride, uvfactor); \
            format->av = rkformats[i].av;\
            format->mpp = rkformats[i].mpp;\
            format->drm = rkformats[i].drm;\
            format->drm_fbc = rkformats[i].drm_fbc;\
            format->rga = rkformats[i].rga;\
            format->planedata = rkformats[i].planedata;\
            format->numplanes = rkformats[i].numplanes;\
            format->bpp = rkformats[i].bpp;\
            format->bpp_div = rkformats[i].bpp_div;\
            format->qual = rkformats[i].qual;\
            format->rga_uncompact = rkformats[i].rga_uncompact;\
            format->rga_msb_aligned = rkformats[i].rga_msb_aligned;\
            format->fbcstride = fbcstride;\
            format->ypixoffset = ypixoffset;\
            memcpy(format->factors, rkformats[i].factors, sizeof(rkformats[i].factors));\
            return 0;\
        }\
    }\
    return -1;\
}

GETFORMAT(drm, uint32_t)
GETFORMAT(mpp, MppFrameFormat)
GETFORMAT(rga, enum _Rga_SURF_FORMAT)
GETFORMAT(av, enum AVPixelFormat)

static int rkmpp_scale_fract(int val, int fract1, int fract1_div,
       int fract2, int fract2_div, int fract3, int fract3_div){
    int fract, fract_div;
    fract1 = fract1 ? fract1 : 1;
    fract1_div = fract1_div ? fract1_div : 1;
    fract2 = fract2 ? fract2 : 1;
    fract2_div = fract2_div ? fract2_div : 1;
    fract3 = fract3 ? fract3 : 1;
    fract3_div = fract3_div ? fract3_div : 1;
    av_reduce(&fract, &fract_div, fract1 * fract2 * fract3,
            fract1_div * fract2_div * fract3_div, INT_MAX);
    val *= fract;
    val /= fract_div;
    return val;
}

void rkmpp_planedata(rkformat *format, int width, int height, int align, int hstride, int vstride,
        int ypixoffset, int overshoot, int fbcstride, factor* planar_uvfactor){
    planedata *planedata = &format->planedata;
    factor *factors = format->factors;
    int size, totalsize=0;

    planedata->width = width;
    planedata->height = height;

    // follow if there is an existing aligned stride
    if (hstride)
        planedata->hstride = hstride;
    else if (format->numplanes == 1)
        planedata->hstride = FFALIGN(rkmpp_scale_fract(width, factors[0].width, factors[0].width_div,
                format->bpp, format->bpp_div, 0, 0), align);
    else
        planedata->hstride = FFALIGN(rkmpp_scale_fract(width, format->bpp, format->bpp_div, 0, 0, 0, 0),
                align);

    if (vstride)
        planedata->vstride = vstride;
    else
        planedata->vstride = FFALIGN(height + ypixoffset, align);

    if(fbcstride){
        planedata->hstride_pix = fbcstride;
        planedata->vstride_pix = FFALIGN(height + ypixoffset, 16);
    } else if(format->numplanes == 1){
        planedata->hstride_pix = FFALIGN(width, 8);
        planedata->vstride_pix = FFALIGN(height + ypixoffset, 8);
    } else {
        planedata->hstride_pix = planedata->hstride;
        planedata->vstride_pix = planedata->vstride;
    }

    hstride = planedata->hstride;
    size = hstride * planedata->vstride;

    // calculate size and stride for each plane
    // skip y plane if there is libyuv conversion.
    // In libyuv we are only interested in U+V planes, Y plane is on another buffer
    for(int i = planar_uvfactor ? 1 : 0; i < format->numplanes; i++){
        width = rkmpp_scale_fract(width, factors[i].width, factors[i].width_div, 0, 0, 0, 0);
        height = rkmpp_scale_fract(height, factors[i].height, factors[i].height_div, 0, 0, 0, 0);
        hstride = rkmpp_scale_fract(hstride, factors[i].hstride, factors[i].hstride_div, 0, 0, 0, 0);
        size = rkmpp_scale_fract(size, factors[i].hstride, factors[i].hstride_div,
                factors[i].height, factors[i].height_div, 0, 0);

        totalsize += ypixoffset * hstride;
        planedata->plane[i].width = width;
        planedata->plane[i].height = height;
        planedata->plane[i].hstride = hstride;
        planedata->plane[i].size =  size;
        planedata->plane[i].offset = totalsize;
        totalsize += planedata->plane[i].size;
    }

    // planaer_uvfactor: plane[1] factors planar format
    // the actual format must be semi-planar
    if(planar_uvfactor){
        int in_width, in_height;
        planedata->uvplane.depth = format->bpp == 2 ? 16 : 8;
        // semi_planar_width_factor = 2 * planar_width_factor * planar_hstride_factor / planar_height_factor
        planedata->uvplane.splitw = rkmpp_scale_fract(planedata->width * 2, factors[1].hstride, factors[1].hstride_div,
                factors[1].width, factors[1].width_div, factors[1].height_div, factors[1].height);
        // semi_planar_height_factor = planar_height_factor
        planedata->uvplane.splith = rkmpp_scale_fract(planedata->height, factors[1].height, factors[1].height_div,
                0, 0, 0, 0);
        // semi_planar_stride_factor = 2 * planar_stride_factor
        planedata->uvplane.hstride = planedata->plane[1].hstride * 2;

        if(planedata->uvplane.depth == 16){
            // due to design of splituvrow_16 function
            // TODO: uniform this using bpp
            planedata->uvplane.splitw *= 2;
            planedata->uvplane.splith /= 2;
        }

        in_width = rkmpp_scale_fract(planedata->width, planar_uvfactor->width, planar_uvfactor->width_div, 0, 0, 0, 0);
        in_height = rkmpp_scale_fract(planedata->height, planar_uvfactor->height, planar_uvfactor->height_div, 0, 0, 0, 0);

        // semi-planar UV plane needs initial scaling, ie: NV16->YUV420P
        // TODO: makes this bpp generic for future NV20
        if(planedata->uvplane.depth == 8 && (planedata->uvplane.splitw != in_width || planedata->uvplane.splith != in_height)){
            planedata->uvplane.offset = totalsize;
            planedata->uvplane.size = rkmpp_scale_fract(planedata->uvplane.hstride * planedata->vstride,
                    factors[1].height, factors[1].height_div, 0, 0, 0, 0);
            totalsize += planedata->uvplane.size;
        }
    }

    planedata->size = totalsize;
    if (overshoot)
        planedata->overshoot = overshoot;
    else
        planedata->overshoot = totalsize + 102400;
}

void rkmpp_release_mppframe_item(void *opaque, uint8_t *data)
{
    MppFrameItem* mppitem = (MppFrameItem*)opaque;

    if(mppitem){
        if(mppitem->rga)
            rkmpp_release_mppframe_item(mppitem->rga, NULL);
        if(mppitem->mppframe)
            mpp_frame_deinit(&mppitem->mppframe);
        av_log(NULL, AV_LOG_DEBUG, "Released %s frame num: %ld, fence:%d\n",
                mppitem->type, mppitem->framenum, mppitem->rgafence);
        av_free(mppitem);
    }
}

static void rkmpp_release_drm_desc(void *opaque, uint8_t *data)
{
    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)opaque;
    av_free(desc);
}


int rkmpp_set_mppframe_to_av(AVCodecContext *avctx, MppFrameItem *mppitem, AVFrame * frame, int index){
    int i;

    // find the first available buffer in  [buf[0], buf[4]]
    for(i=0; i<5; i++){
        if(i > 3)
            return -1;
        else if(!frame->buf[i])
            break;
    }

    frame->buf[i] = av_buffer_create(mppitem->mppframe, mpp_frame_get_buf_size(mppitem->mppframe),
            rkmpp_release_mppframe_item, mppitem, AV_BUFFER_FLAG_READONLY);

    if(i >= 0){
        if(index >= 0)
            frame->data[index] = frame->buf[i]->data;
        return 0;
    }

    return -1;
}

int rkmpp_set_drmdesc_to_av(AVDRMFrameDescriptor *desc, AVFrame *frame){
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

int rkmpp_rga_wait(MppFrameItem* item, int timeout, struct timespec* ts){
    // from rga_sync.cpp
    struct pollfd fds;
    int ret;

    if (!item || item->rgafence <= 0)
        return 0;

    fds.fd = item->rgafence;
    fds.events = POLLIN;

    do {
        ret = poll(&fds, 1, timeout);
        if (ret > 0) {
            if (fds.revents & (POLLERR | POLLNVAL)) {
                errno = EINVAL;
                return AVERROR(EAGAIN);
            }
            close(item->rgafence);
            return 0;
        } else if (ret == 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if(ts && ts->tv_sec && (now.tv_sec - ts->tv_sec) > 2)
                return AVERROR_UNKNOWN;
            errno = ETIME;
            return AVERROR(EAGAIN);
        }
    } while (ret == -1 && (errno == EINTR || errno == EAGAIN));

    return ret;
}

static int rkmpp_rga_scale(uint64_t src_fd, uint16_t src_width, uint16_t src_height, uint16_t src_hstride, uint16_t src_vstride,
        uint64_t dst_fd, uint16_t dst_width, uint16_t dst_height, uint16_t dst_hstride, uint16_t dst_vstride,
        rkformat* informat, rkformat* outformat, int* outfence){
    int ret=0;
    rga_info_t src = {0};
    rga_info_t dst = {0};

    src.fd = src_fd;
    src.mmuFlag = 1;
    src.format = informat->rga;
    rga_set_rect(&src.rect, 0, informat->ypixoffset, src_width, src_height,
            informat->planedata.hstride_pix, informat->planedata.vstride_pix, informat->rga);
    src.is_10b_compact = informat->rga_uncompact;
    src.is_10b_endian = informat->rga_msb_aligned;
    src.rd_mode = informat->fbcstride ? IM_FBC_MODE : IM_RASTER_MODE;

    dst.fd = dst_fd;
    dst.mmuFlag = 1;
    dst.format = outformat->rga;
    rga_set_rect(&dst.rect, 0, outformat->ypixoffset, dst_width, dst_height,
            outformat->planedata.hstride_pix, outformat->planedata.vstride_pix, outformat->rga);
    dst.is_10b_compact = outformat->rga_uncompact;
    dst.is_10b_endian = outformat->rga_msb_aligned;
    dst.sync_mode = RGA_BLIT_ASYNC;
    dst.in_fence_fd = -1;
    dst.out_fence_fd = *outfence ? *outfence : -1;
    dst.rd_mode = outformat->fbcstride ? IM_FBC_MODE : IM_RASTER_MODE;

    if(src.is_10b_compact || dst.is_10b_compact)
        dst.core = IM_SCHEDULER_RGA3_CORE0 | IM_SCHEDULER_RGA3_CORE1;

    ret = c_RkRgaBlit(&src, &dst, NULL);
    *outfence = dst.out_fence_fd;

    return ret;
}

int rkmpp_rga_convert_mpp_mpp(AVCodecContext *avctx, MppFrame in_mppframe, rkformat* in_format,
        MppFrame out_mppframe, rkformat* out_format, int* outfence){

    if(!out_mppframe)
        return -1;
    if(rkmpp_rga_scale(mpp_buffer_get_fd(mpp_frame_get_buffer(in_mppframe)),
        mpp_frame_get_width(in_mppframe), mpp_frame_get_height(in_mppframe),
        mpp_frame_get_hor_stride(in_mppframe), mpp_frame_get_ver_stride(in_mppframe),
        mpp_buffer_get_fd(mpp_frame_get_buffer(out_mppframe)),
        mpp_frame_get_width(out_mppframe), mpp_frame_get_height(out_mppframe),
        mpp_frame_get_hor_stride(out_mppframe), mpp_frame_get_ver_stride(out_mppframe),
        in_format, out_format, outfence)){
            av_log(avctx, AV_LOG_WARNING, "RGA conversion failed\n");
            return -1;
    } else
        return 0;

    return -1;
}

static int rkmpp_soft_scale(uint8_t* src_uv, int src_stride_uv,  int src_width, int src_height,
        uint8_t* dst_uv, int dst_stride_uv, int dst_width, int dst_height, int depth){
    if(depth == 10 || depth == 16)
        return UVScale_16((const uint16_t*) src_uv, src_stride_uv, src_width, src_height,
                (uint16_t* )dst_uv, dst_stride_uv, dst_width, dst_height, kFilterNone);
    else if (depth == 8)
        return UVScale((const uint8_t*) src_uv, src_stride_uv, src_width, src_height,
                (uint8_t*) dst_uv, dst_stride_uv, dst_width, dst_height,
                kFilterNone);
    return -1;
}

static int rkmpp_soft_split(uint8_t* src_uv, int src_stride_uv,
        uint8_t* dst_u, int dst_stride_u, uint8_t* dst_v, int dst_stride_v,
        int width, int height, int depth){
    if(depth == 10 || depth == 16)
        SplitUVPlane_16((const uint16_t*) src_uv, src_stride_uv,
                        (uint16_t*) dst_u, dst_stride_u, (uint16_t*) dst_v, dst_stride_v,
                        width, height, depth);
    else if (depth == 8)
        SplitUVPlane((const uint8_t*) src_uv, src_stride_uv,
                     (uint8_t*) dst_u, dst_stride_u, (uint8_t*) dst_v, dst_stride_v,
                      width, height);
    else
        return -1;
    return 0;
}

void rkmpp_semi_to_planar_soft(MppFrame mpp_inframe, rkformat* informat, MppFrame mpp_outframe, rkformat* outformat){
    // warning: mpp frame must not be released until displayed
    MppBuffer in_buffer = mpp_frame_get_buffer(mpp_inframe);
    MppBuffer out_buffer = mpp_frame_get_buffer(mpp_outframe);
    uint8_t* src = (uint8_t*)mpp_buffer_get_ptr(in_buffer);
    uint8_t* dst = (uint8_t*)mpp_buffer_get_ptr(out_buffer);
    int hstride;

    if(outformat->planedata.uvplane.size){
        // scale down uv plane by scale and write it to y plane of outframe temporarily
        rkmpp_soft_scale(src + informat->planedata.plane[1].offset, informat->planedata.plane[1].hstride,
                informat->planedata.plane[1].width, informat->planedata.plane[1].height,
                dst + outformat->planedata.uvplane.offset, outformat->planedata.uvplane.hstride,
                outformat->planedata.uvplane.splitw, outformat->planedata.uvplane.splith,
                outformat->planedata.uvplane.depth);

        src = dst + outformat->planedata.uvplane.offset;
        hstride = outformat->planedata.uvplane.hstride;
    } else {
        src +=  informat->planedata.plane[1].offset;
        hstride = informat->planedata.hstride;
    }

    // convert uv plane from semi-planar to planar
    rkmpp_soft_split(src, hstride,
            dst + outformat->planedata.plane[1].offset, outformat->planedata.plane[1].hstride,
            dst + outformat->planedata.plane[2].offset, outformat->planedata.plane[2].hstride,
            outformat->planedata.uvplane.splitw, outformat->planedata.uvplane.splith,
            outformat->planedata.uvplane.depth);
}

MppFrame rkmpp_create_mpp_frame(int width, int height, rkformat *rkformat, MppBufferGroup buffer_group,
        AVDRMFrameDescriptor *desc, AVFrame *frame){
    MppFrame mppframe = NULL;
    MppBuffer mppbuffer = NULL;
    //int avmap[3][4]; //offset, dststride, width, height of max 3 planes
    int size, ret, hstride, vstride, hstride_pix;

    // create buffer, assign strides and size props
    if(desc){
        // and calculate strides and plane size from actual drm buffer
        // note! this is not aligned to mpp requirements, but should be fine.
        MppBufferInfo info;
        AVDRMLayerDescriptor *layer = &desc->layers[0];

        size = desc->objects[0].size;
        hstride = layer->planes[0].pitch;
        vstride = rkformat->numplanes == 1 ? size / hstride : layer->planes[1].offset / hstride;
        hstride_pix = rkformat->numplanes == 1 ? width : hstride;

        memset(&info, 0, sizeof(info));
        info.type   = MPP_BUFFER_TYPE_DRM;
        info.size   = size;
        info.fd     = desc->objects[0].fd;

        ret = mpp_buffer_import(&mppbuffer, &info);
    } else {
        // use precalculated strides and size from rkmpp_planedata, this follow avctx context frame format
        width = rkformat->planedata.width;
        height = rkformat->planedata.height;
        vstride = rkformat->planedata.vstride;
        hstride = rkformat->planedata.hstride;
        hstride_pix = rkformat->planedata.hstride_pix;
        size = rkformat->planedata.size;

        ret = mpp_buffer_get(buffer_group, &mppbuffer, size);
        if (ret)
            goto clean;

        // copy av frame to mpp buffer
        if(frame){
            for(int i = 0; i < rkformat->numplanes; i++){
                CopyPlane(frame->data[i], frame->linesize[i],
                        (char *)mpp_buffer_get_ptr(mppbuffer) + rkformat->planedata.plane[i].offset,
                        rkformat->planedata.plane[i].hstride,
                        rkformat->planedata.plane[i].width, rkformat->planedata.plane[i].height);
            }
        }
    }

    ret = mpp_frame_init(&mppframe);
    if (ret)
        goto clean;

     mpp_frame_set_width(mppframe, width);
     mpp_frame_set_height(mppframe, height);
     mpp_frame_set_fmt(mppframe, rkformat->mpp);
     mpp_frame_set_hor_stride(mppframe, hstride);
     mpp_frame_set_ver_stride(mppframe, vstride);
     mpp_frame_set_hor_stride_pixel(mppframe, hstride_pix);
     mpp_frame_set_buffer(mppframe, mppbuffer);
     mpp_frame_set_buf_size(mppframe, size);
     mpp_buffer_put(mppbuffer);

     return mppframe;

clean:
     if(mppbuffer)
         mpp_buffer_put(mppbuffer);
     if(mppframe)
         mpp_frame_deinit(&mppframe);
     return mppframe;
}

MppFrame rkmpp_mppframe_from_av(AVFrame *frame){
    if(frame->data[RKMPP_MPPFRAME_BUFINDEX]){
        rkmpp_frame_type * mppframe = (rkmpp_frame_type *) frame->data[RKMPP_MPPFRAME_BUFINDEX];
        if(mppframe->name && !strcmp(mppframe->name, "mpp_frame") &&
                mpp_frame_get_fmt(frame->data[RKMPP_MPPFRAME_BUFINDEX]) != MPP_FMT_YUV420SP_10BIT)
            return frame->data[RKMPP_MPPFRAME_BUFINDEX];
    }
    return NULL;
}

void rkmpp_transfer_mpp_props(MppFrame inframe, MppFrame outframe){
    mpp_frame_set_mode(outframe, mpp_frame_get_mode(inframe));
    mpp_frame_set_color_range(outframe, mpp_frame_get_color_range(inframe));
    mpp_frame_set_color_primaries(outframe, mpp_frame_get_color_primaries(inframe));
    mpp_frame_set_color_trc(outframe, mpp_frame_get_color_trc(inframe));
    mpp_frame_set_colorspace(outframe, mpp_frame_get_colorspace(inframe));
    mpp_frame_set_pts(outframe, mpp_frame_get_pts(inframe));
}
