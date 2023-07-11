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
#include "rkplane.h"


int rkmpp_init_decoder(AVCodecContext *avctx){
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;

    int ret;

    ret = codec->mpi->control(codec->ctx, MPP_DEC_SET_EXT_BUF_GROUP, codec->buffer_group);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "Failed to assign buffer group (code = %d)\n", ret);
        return AVERROR_UNKNOWN;
    }

    codec->mpi->control(codec->ctx, MPP_DEC_SET_DISABLE_ERROR, NULL);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to prepare Codec (code = %d)\n", ret);
        return AVERROR_UNKNOWN;
    }

    avctx->coded_width = FFALIGN(avctx->width, 64);
    avctx->coded_height = FFALIGN(avctx->height, 64);

    codec->hwdevice_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_DRM);
    if (!codec->hwdevice_ref) {
        return AVERROR(ENOMEM);
    }

    ret = av_hwdevice_ctx_init(codec->hwdevice_ref);
    if (ret < 0)
        return ret;

    av_buffer_unref(&codec->hwframes_ref);
    codec->hwframes_ref = av_hwframe_ctx_alloc(codec->hwdevice_ref);
    if (!codec->hwframes_ref) {
        return AVERROR(ENOMEM);
    }

    return 0;
}

static int rkmpp_get_frame(AVCodecContext *avctx, AVFrame *frame, int timeout)
{
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;
    MppFrameFormat mpp_format;
    MppFrame mppframe = NULL;
    MppBuffer buffer = NULL;
    rkformat format;
    int ret, mode, latency;

    codec->mpi->control(codec->ctx, MPP_SET_OUTPUT_TIMEOUT, (MppParam)&timeout);

    ret = codec->mpi->decode_get_frame(codec->ctx, &mppframe);
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
        goto clean;
    }

    if (mpp_frame_get_discard(mppframe)) {
        av_log(avctx, AV_LOG_DEBUG, "Received a discard frame.\n");
        ret = AVERROR(EAGAIN);
        goto clean;
    }

    if (mpp_frame_get_errinfo(mppframe)) {
        av_log(avctx, AV_LOG_ERROR, "Received a errinfo frame.\n");
        ret = AVERROR_UNKNOWN;
        goto clean;
    }

     mpp_format = mpp_frame_get_fmt(mppframe) & MPP_FRAME_FMT_MASK;

    if (mpp_frame_get_info_change(mppframe)) {
        av_log(avctx, AV_LOG_INFO, "Decoder noticed an info change\n");

        if (avctx->pix_fmt == AV_PIX_FMT_DRM_PRIME){
            char drmname[4];
            AVHWFramesContext *hwframes;
            rkmpp_get_mpp_format(&format, mpp_format);
            DRMFORMATNAME(drmname, format.drm)

            hwframes = (AVHWFramesContext*)codec->hwframes_ref->data;
            hwframes->format    = AV_PIX_FMT_DRM_PRIME;
            hwframes->sw_format = format.av;
            hwframes->width     = avctx->width;
            hwframes->height    = avctx->height;
            ret = av_hwframe_ctx_init(codec->hwframes_ref);
            // FIXME: handle error
            av_log(avctx, AV_LOG_INFO, "Decoder is set to DRM Prime with format %s.\n", drmname);
        }
        codec->mpi->control(codec->ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
        if(mpp_format == MPP_FMT_YUV420SP_10BIT)
            av_log(avctx, AV_LOG_WARNING, "10bit NV15 plane will be downgraded to 8bit %s.\n", av_get_pix_fmt_name(avctx->pix_fmt));
        goto clean;
    }

    // here we should have a valid frame
    av_log(avctx, AV_LOG_DEBUG, "Received a frame.\n");

    // now setup the frame buffer info
    buffer = mpp_frame_get_buffer(mppframe);
    if (!buffer) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get the frame buffer, frame is dropped (code = %d)\n", ret);
        ret = AVERROR(EAGAIN);
        goto clean;
    }

    latency = rkmpp_update_latency(avctx, -1);

    if (avctx->pix_fmt == AV_PIX_FMT_DRM_PRIME){
        ret = import_mpp_to_drm(avctx, mppframe, frame);
    } else if (mpp_format == MPP_FMT_YUV420SP_10BIT && avctx->pix_fmt == AV_PIX_FMT_NV12){
        ret = mpp_nv15_av_nv12(avctx, mppframe, frame);
    } else if (mpp_format == MPP_FMT_YUV420SP_10BIT && avctx->pix_fmt == AV_PIX_FMT_YUV420P){
        ret = mpp_nv15_av_yuv420p(avctx, mppframe, frame);
    } else if (mpp_format == MPP_FMT_YUV420SP && avctx->pix_fmt == AV_PIX_FMT_NV12){
        ret = mpp_nv12_av_nv12(avctx, mppframe, frame);
    } else {
        rkmpp_get_mpp_format(&format, mpp_format);
        ret = convert_mpp_to_av(avctx, mppframe, frame, format.av, avctx->pix_fmt);
    }

    if(ret < 0){
        av_log(avctx, AV_LOG_ERROR, "Failed set frame buffer (code = %d)\n", ret);
        return ret;
    }



    // setup general frame fields
    frame->format           = avctx->pix_fmt;
    frame->width            = mpp_frame_get_width(mppframe);
    frame->height           = mpp_frame_get_height(mppframe);
    frame->color_range      = mpp_frame_get_color_range(mppframe);
    frame->color_primaries  = mpp_frame_get_color_primaries(mppframe);
    frame->color_trc        = mpp_frame_get_color_trc(mppframe);
    frame->colorspace       = mpp_frame_get_colorspace(mppframe);
    frame->pts              = mpp_frame_get_pts(mppframe);

    // when mpp can not determine the color space, it returns reserved (0) value
    // firefox does not understand this and instead expect unspecified (2) values
    frame->color_primaries  = frame->color_primaries == AVCOL_PRI_RESERVED0 ? AVCOL_PRI_UNSPECIFIED : frame->color_primaries;
    frame->color_trc        = frame->color_trc == AVCOL_TRC_RESERVED0 ? AVCOL_TRC_UNSPECIFIED : frame->color_trc;
    frame->colorspace        = frame->colorspace == AVCOL_SPC_RGB ? AVCOL_SPC_UNSPECIFIED: frame->color_trc;

    mode = mpp_frame_get_mode(mppframe);
    frame->interlaced_frame = ((mode & MPP_FRAME_FLAG_FIELD_ORDER_MASK) == MPP_FRAME_FLAG_DEINTERLACED);
    frame->top_field_first  = ((mode & MPP_FRAME_FLAG_FIELD_ORDER_MASK) == MPP_FRAME_FLAG_TOP_FIRST);

    codec->frames++;
    rkmpp_update_latency(avctx, latency);
    return 0;

clean:
    if (mppframe)
        mpp_frame_deinit(&mppframe);
    return ret;
}

static int rkmpp_send_packet(AVCodecContext *avctx, AVPacket *packet)
{
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;
    MppPacket mpkt;
    int64_t pts = packet->pts;
    int ret;

    if(pts == AV_NOPTS_VALUE || pts < 0){
        if(!codec->ptsstep){
            int64_t x = avctx->pkt_timebase.den * (int64_t)avctx->framerate.den;
            int64_t y = avctx->pkt_timebase.num * (int64_t)avctx->framerate.num;
            codec->ptsstep = x / y;
        }
        pts = codec->pts;
        codec->pts += codec->ptsstep;
    }

    ret = mpp_packet_init(&mpkt, packet->data, packet->size);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init MPP packet (code = %d)\n", ret);
        return AVERROR_UNKNOWN;
    }

    mpp_packet_set_pts(mpkt, pts);

    ret = codec->mpi->decode_put_packet(codec->ctx, mpkt);
    mpp_packet_deinit(&mpkt);

    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_TRACE, "Decoder buffer full\n");
        return AVERROR(EAGAIN);
    }

    av_log(avctx, AV_LOG_DEBUG, "Wrote %d bytes to decoder\n", packet->size);
    return 0;
}

static int rkmpp_send_eos(AVCodecContext *avctx)
{
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;
    MppPacket mpkt;
    int ret;

    ret = mpp_packet_init(&mpkt, NULL, 0);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init EOS packet (code = %d)\n", ret);
        return AVERROR_UNKNOWN;
    }

    mpp_packet_set_eos(mpkt);

    do {
        ret = codec->mpi->decode_put_packet(codec->ctx, mpkt);
    } while (ret != MPP_OK);
    mpp_packet_deinit(&mpkt);

    return 0;
}

int rkmpp_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    AVCodecInternal *avci = avctx->internal;
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;
    AVPacket *packet = &codec->lastpacket;
    int ret_send, ret_get;

    // get packet if not already available from previous iteration
    if (!avci->draining){
        if(!packet->size){
            switch(ff_decode_get_packet(avctx, packet)){
                case AVERROR_EOF:
                    av_log(avctx, AV_LOG_DEBUG, "Decoder Draining.\n");
                    return rkmpp_send_eos(avctx);
                case AVERROR(EAGAIN):
                    av_log(avctx, AV_LOG_TRACE, "Decoder Can't get packet retrying.\n");
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
            av_log(avctx, AV_LOG_ERROR, "Decoder Failed to send data (code = %d)\n", ret_send);
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
        av_log(avctx, AV_LOG_ERROR, "Decoder Failed to get frame (code = %d)\n", ret_get);

   return ret_get;
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

