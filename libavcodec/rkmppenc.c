/*
 * RockChip MPP Video Decoder
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
#include "rkmpp.h"
#include "rkplane.h"

int rkmpp_init_encoder(AVCodecContext *avctx){
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;

    RK_U8 enc_hdr_buf[HDR_SIZE];
    MppPacket packet = NULL;
    void *packetpos;
    size_t packetlen;
    int ret;
    int input_timeout = 500;
    // ENCODER SETUP
    ret = mpp_enc_cfg_init(&codec->enccfg);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "Codec failed to initialize encoder config (code = %d)\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    ret = codec->mpi->control(codec->ctx, MPP_ENC_GET_CFG, codec->enccfg);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "Codec failed to get encoder config (code = %d)\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    // set extradata
    memset(enc_hdr_buf, 0 , HDR_SIZE);

    ret = mpp_packet_init(&packet, (void *)enc_hdr_buf, HDR_SIZE);
    if (!packet) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init extra info packet (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    mpp_packet_set_length(packet, 0);
    ret = codec->mpi->control(codec->ctx, MPP_ENC_GET_HDR_SYNC, packet);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get extra info on MPI (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    /* get and write sps/pps for H.264 */
    packetpos = mpp_packet_get_pos(packet);
    packetlen  = mpp_packet_get_length(packet);
    if (avctx->extradata != NULL && avctx->extradata_size != packetlen) {
        av_free(avctx->extradata);
        avctx->extradata = NULL;
    }
    if (!avctx->extradata)
        avctx->extradata = av_malloc(packetlen);
    if (avctx->extradata == NULL) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    avctx->extradata_size = packetlen;
    memcpy(avctx->extradata, packetpos, packetlen);
    mpp_packet_deinit(&packet);

    codec->mpi->control(codec->ctx, MPP_SET_INPUT_TIMEOUT, &input_timeout);
    avctx->profile = rk_context->profile;
    avctx->level = rk_context->level;
    return 0;

fail:
    av_log(avctx, AV_LOG_ERROR, "Failed to initialize RKMPP Codec.\n");
    if(packet)
        mpp_packet_deinit(&packet);
    return ret;
}

static int rkmpp_config(AVCodecContext *avctx, MppFrame mppframe){
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;
    MppEncCfg cfg = codec->enccfg;
    RK_U32 rc_mode, rc_qpinit, split_mode, split_arg, split_out, fps_num, fps_den;
    MppCodingType coding_type = rkmpp_get_codingtype(avctx);
    MppEncHeaderMode header_mode;
    MppEncSeiMode sei_mode;
    MppFrameFormat mpp_format = mpp_frame_get_fmt(mppframe) & MPP_FRAME_FMT_MASK;
    int ret, max_bps, min_bps, qmin, qmax;

    //prep config
    mpp_enc_cfg_set_s32(cfg, "prep:width", mpp_frame_get_width(mppframe));
    mpp_enc_cfg_set_s32(cfg, "prep:height", mpp_frame_get_height(mppframe));
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", mpp_frame_get_hor_stride(mppframe));
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", mpp_frame_get_ver_stride(mppframe));
    mpp_enc_cfg_set_s32(cfg, "prep:format", mpp_format);
    mpp_enc_cfg_set_s32(cfg, "prep:mirroring", 0);
    mpp_enc_cfg_set_s32(cfg, "prep:rotation", 0);
    mpp_enc_cfg_set_s32(cfg, "prep:flip", 0);

    //rc config
    // make sure time base of avctx is synced to input frames
    av_reduce(&fps_num, &fps_den, avctx->time_base.den, avctx->time_base.num, 65535);

    /* fix input / output frame rate */
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fps_num);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denorm", fps_den);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num",fps_num);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denorm", fps_den);

    mpp_enc_cfg_set_s32(cfg, "rc:gop", FFMAX(avctx->gop_size, 1));

    // config rc: mode
    rc_mode = rk_context->rc_mode;
    if(rc_mode == MPP_ENC_RC_MODE_BUTT)
        rc_mode = MPP_ENC_RC_MODE_CBR;

    switch(rc_mode){
        case MPP_ENC_RC_MODE_VBR:
            av_log(avctx, AV_LOG_INFO, "Rate Control mode is set to VBR\n"); break;
        case MPP_ENC_RC_MODE_CBR:
            av_log(avctx, AV_LOG_INFO, "Rate Control mode is set to CBR\n"); break;
        case MPP_ENC_RC_MODE_FIXQP:
            av_log(avctx, AV_LOG_INFO, "Rate Control mode is set to CQP\n"); break;
        case MPP_ENC_RC_MODE_AVBR:
            av_log(avctx, AV_LOG_INFO, "Rate Control mode is set to AVBR\n"); break;
    }

    mpp_enc_cfg_set_u32(cfg, "rc:mode", rc_mode);

    // config rc: bps
    mpp_enc_cfg_set_u32(cfg, "rc:bps_target", avctx->bit_rate);

    switch (rc_mode) {
        case MPP_ENC_RC_MODE_FIXQP : {
            /* do not setup bitrate on FIXQP mode */
            min_bps =  max_bps = avctx->bit_rate;
            break;
        }
        case MPP_ENC_RC_MODE_CBR : {
            /* CBR mode has narrow bound */
            max_bps =  avctx->bit_rate * 17 / 16;
            min_bps =  avctx->bit_rate * 15 / 16;
            break;
        }
        case MPP_ENC_RC_MODE_VBR :
        case MPP_ENC_RC_MODE_AVBR : {
            /* VBR mode has wide bound */
            max_bps =  avctx->bit_rate * 17 / 16;
            min_bps =  avctx->bit_rate * 1 / 16;
            break;
        }
        default : {
            /* default use CBR mode */
            max_bps =  avctx->bit_rate * 17 / 16;
            min_bps =  avctx->bit_rate * 15 / 16;
            break;
        }
    }

    mpp_enc_cfg_set_s32(cfg, "rc:bps_max", max_bps);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_min", min_bps);

    av_log(avctx, AV_LOG_INFO, "Bitrate Target/Min/Max is set to %ld/%d/%d\n", avctx->bit_rate, min_bps, max_bps);

    // config rc: drop behaviour
    mpp_enc_cfg_set_u32(cfg, "rc:drop_mode", MPP_ENC_RC_DROP_FRM_DISABLED);
    mpp_enc_cfg_set_u32(cfg, "rc:drop_thd", 20); // 20% of max bps
    mpp_enc_cfg_set_u32(cfg, "rc:drop_gap", 1); // Do not continuous drop frame

    // config rc: qp
    switch (coding_type) {
        case MPP_VIDEO_CodingAVC :
        case MPP_VIDEO_CodingHEVC : {
            qmax = QMIN_H26x + (100 - rk_context->qmin) * (QMAX_H26x - QMIN_H26x) / 100;
            qmin = QMIN_H26x + (100 - rk_context->qmax) * (QMAX_H26x - QMIN_H26x) / 100;
            switch (rc_mode) {
                case MPP_ENC_RC_MODE_FIXQP : {
                    mpp_enc_cfg_set_s32(cfg, "rc:qp_init", qmin);
                    mpp_enc_cfg_set_s32(cfg, "rc:qp_max", qmin);
                    mpp_enc_cfg_set_s32(cfg, "rc:qp_min", qmin);
                    mpp_enc_cfg_set_s32(cfg, "rc:qp_max_i", qmin);
                    mpp_enc_cfg_set_s32(cfg, "rc:qp_min_i", qmin);
                    mpp_enc_cfg_set_s32(cfg, "rc:qp_ip", 0);
                    break;
                }
                case MPP_ENC_RC_MODE_CBR :
                case MPP_ENC_RC_MODE_VBR :
                case MPP_ENC_RC_MODE_AVBR : {
                    mpp_enc_cfg_set_s32(cfg, "rc:qp_init", qmin);
                    mpp_enc_cfg_set_s32(cfg, "rc:qp_max", qmax);
                    mpp_enc_cfg_set_s32(cfg, "rc:qp_min", qmin);
                    mpp_enc_cfg_set_s32(cfg, "rc:qp_max_i",qmax);
                    mpp_enc_cfg_set_s32(cfg, "rc:qp_min_i", qmin);
                    mpp_enc_cfg_set_s32(cfg, "rc:qp_ip", 2);
                    break;
                }
                default : {
                    av_log(avctx, AV_LOG_ERROR, "Unsupported Encoder Mode %d.\n", rc_mode);
                    break;
                }
            }
            break;
        }
        case MPP_VIDEO_CodingVP8 : {
            // vp8 only setup base qp range
            qmax = QMIN_VPx + (100 - rk_context->qmin) * (QMAX_VPx - QMIN_VPx) / 100;
            qmin = QMIN_VPx + (100 - rk_context->qmax) * (QMAX_VPx - QMIN_VPx) / 100;
            mpp_enc_cfg_set_s32(cfg, "rc:qp_init", qmin);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_max", qmax);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_min", qmin);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_max_i", qmax);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_min_i", qmin);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_ip", 6);
            break;
        }
        case MPP_VIDEO_CodingMJPEG : {
            qmax = QMIN_JPEG + (100 - rk_context->qmin) * (QMAX_JPEG - QMIN_JPEG) / 100;
            qmin = QMIN_JPEG + (100 - rk_context->qmax) * (QMAX_JPEG- QMIN_JPEG) / 100;
            // jpeg use special codec config to control qtable
            mpp_enc_cfg_set_s32(cfg, "jpeg:q_factor", 80);
            mpp_enc_cfg_set_s32(cfg, "jpeg:qf_max", qmax);
            mpp_enc_cfg_set_s32(cfg, "jpeg:qf_min", qmin);
            break;
        }
        default : {
            break;
        }
    }

    // setup codec
     mpp_enc_cfg_set_s32(cfg, "codec:type", coding_type);
     switch (coding_type) {
         case MPP_VIDEO_CodingAVC : {
             mpp_enc_cfg_set_s32(cfg, "h264:profile", avctx->profile);
             mpp_enc_cfg_set_s32(cfg, "h264:level", avctx->level);
             mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", rk_context->coder);
             mpp_enc_cfg_set_s32(cfg, "h264:cabac_idc", 0);
             mpp_enc_cfg_set_s32(cfg, "h264:trans8x8", rk_context->dct8x8 && avctx->profile == FF_PROFILE_H264_HIGH ? 1 : 0);
             switch(avctx->profile){
                 case FF_PROFILE_H264_BASELINE: av_log(avctx, AV_LOG_INFO, "Profile is set to BASELINE\n"); break;
                 case FF_PROFILE_H264_MAIN: av_log(avctx, AV_LOG_INFO, "Profile is set to MAIN\n"); break;
                 case FF_PROFILE_H264_HIGH:
                     av_log(avctx, AV_LOG_INFO, "Profile is set to HIGH\n");
                     if(rk_context->dct8x8)
                         av_log(avctx, AV_LOG_INFO, "8x8 Transform is enabled\n");
                     break;
             }
             av_log(avctx, AV_LOG_INFO, "Level is set to %d\n", avctx->level);
             if(rk_context->coder)
                 av_log(avctx, AV_LOG_INFO, "Coder is set to CABAC\n");
             else
                 av_log(avctx, AV_LOG_INFO, "Coder is set to CAVLC\n");
             break;
         }
         case MPP_VIDEO_CodingHEVC :
         case MPP_VIDEO_CodingMJPEG :
         case MPP_VIDEO_CodingVP8 :
             break;
         default : {
             av_log(avctx, AV_LOG_ERROR, "Unsupported coding type for config (code = %d).\n", coding_type);
             break;
         }
     }

     av_log(avctx, AV_LOG_INFO, "Quality Min/Max is set to %d%(Quant=%d) / %d%(Quant=%d)\n",
             rk_context->qmin, qmax, rk_context->qmax, qmin);

     split_mode = 0;
     split_arg = 0;
     split_out = 0;

     if (split_mode) {
         mpp_enc_cfg_set_s32(cfg, "split:mode", split_mode);
         mpp_enc_cfg_set_s32(cfg, "split:arg", split_arg);
         mpp_enc_cfg_set_s32(cfg, "split:out", split_out);
     }

     ret = codec->mpi->control(codec->ctx, MPP_ENC_SET_CFG, cfg);
     if (ret != MPP_OK) {
         av_log(avctx, AV_LOG_ERROR, "Failed to set cfg on MPI (code = %d).\n", ret);
         return AVERROR_UNKNOWN;
     }

     sei_mode = MPP_ENC_SEI_MODE_ONE_FRAME;
     ret = codec->mpi->control(codec->ctx, MPP_ENC_SET_SEI_CFG, &sei_mode);
     if (ret != MPP_OK) {
         av_log(avctx, AV_LOG_ERROR, "Failed to set sei cfg on MPI (code = %d).\n", ret);
         return AVERROR_UNKNOWN;
     }

     header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
     if (coding_type == MPP_VIDEO_CodingAVC || coding_type == MPP_VIDEO_CodingHEVC) {
         ret = codec->mpi->control(codec->ctx, MPP_ENC_SET_HEADER_MODE, &header_mode);
         if (ret) {
             av_log(avctx, AV_LOG_ERROR, "Failed header mode on MPI (code = %d).\n", ret);
             return ret;
         }
     }

    return 0;
}

static void rkmpp_release_packet_buf(void *opaque, uint8_t *data){
    MppPacket mpppacket = opaque;
    mpp_packet_deinit(&mpppacket);
}

static int rkmpp_send_frame(AVCodecContext *avctx, const AVFrame *frame){
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;
    MppFrame mppframe = NULL;
    int ret=0, keepframe=0;

    // EOS frame, avframe=NULL
    if (!frame) {
        av_log(avctx, AV_LOG_DEBUG, "End of stream.\n");
        mpp_frame_init(&mppframe);
        mpp_frame_set_eos(mppframe, 1);
    } else {
        if (avctx->pix_fmt == AV_PIX_FMT_DRM_PRIME){
            mppframe = import_drm_to_mpp(avctx, frame);
        } else {
            mppframe = get_mppframe_from_av(frame);
            if(mppframe)
                keepframe = 1;
            else
                mppframe = create_mpp_frame(frame->width, frame->height, avctx->pix_fmt, codec->buffer_group, NULL, frame);
        }

        //FIXME: handle exception if mppframe is still NULL

        //FIXME: ugly, those stuff to be done on init
        if(frame->time_base.num && frame->time_base.den){
            avctx->time_base.num = frame->time_base.num;
            avctx->time_base.den = frame->time_base.den;
        } else {
            avctx->time_base.num = avctx->framerate.den;
            avctx->time_base.den = avctx->framerate.num;
        }

        mpp_frame_set_pts(mppframe, frame->pts);
    }

    // there coould be better ways to config the encoder, but lets do it this way atm.
    // FIXME: this is ugly, better to do this on init
    if(!codec->hasconfig && !rkmpp_config(avctx, mppframe))
        codec->hasconfig = 1;

    // put the frame in encoder
    ret = codec->mpi->encode_put_frame(codec->ctx, mppframe);

    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_DEBUG, "Encoder buffer full\n");
        ret = AVERROR(EAGAIN);
    } else
        av_log(avctx, AV_LOG_DEBUG, "Wrote %ld bytes to encoder\n", mpp_frame_get_buf_size(mppframe));

    if(!keepframe)
        mpp_frame_deinit(&mppframe);
    return ret;
}


static int rkmpp_get_packet(AVCodecContext *avctx, AVPacket *packet, int timeout){
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;
    MppPacket mpppacket = NULL;
    MppMeta meta = NULL;
    int ret, keyframe, latency;

    codec->mpi->control(codec->ctx, MPP_SET_OUTPUT_TIMEOUT, (MppParam)&timeout);

    ret = codec->mpi->encode_get_packet(codec->ctx, &mpppacket);

    // rest of above code is never tested most likely broken
    if (ret != MPP_OK && ret != MPP_ERR_TIMEOUT) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get packet (code = %d)\n", ret);
        return AVERROR(EAGAIN);
    }

    if (!mpppacket) {
        av_log(avctx, AV_LOG_DEBUG, "Timeout getting encoded packet.\n");
        return AVERROR(EAGAIN);
    }

    // TO-DO: Handle EOS
    if (mpp_packet_get_eos(mpppacket)) {
        av_log(avctx, AV_LOG_DEBUG, "Received an EOS packet.\n");
        ret = AVERROR_EOF;
        goto fail;
    }

    av_log(avctx, AV_LOG_DEBUG, "Received a packet.\n");

    packet->data = mpp_packet_get_data(mpppacket);
    packet->size = mpp_packet_get_length(mpppacket);
    packet->buf = av_buffer_create(packet->data, packet->size, rkmpp_release_packet_buf,
            mpppacket, AV_BUFFER_FLAG_READONLY);
    if (!packet->buf) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    //FIXME: This is low-res and does not cover b-frames.
	packet->time_base.num = avctx->time_base.num;
    packet->time_base.den = avctx->time_base.den;
    packet->pts = mpp_packet_get_pts(mpppacket);
    packet->dts = mpp_packet_get_pts(mpppacket);
    codec->frames++;

    meta = mpp_packet_get_meta(mpppacket);
    if (meta)
        mpp_meta_get_s32(meta, KEY_OUTPUT_INTRA, &keyframe);
    if (keyframe)
        packet->flags |= AV_PKT_FLAG_KEY;

    return 0;
fail:
    if (mpppacket)
        mpp_packet_deinit(&mpppacket);
    return ret;
}


int rkmpp_encode(AVCodecContext *avctx, AVPacket *packet, const AVFrame *frame, int *got_packet){
    int ret;

    ret = rkmpp_send_frame(avctx, frame);
    if (ret)
        return ret;

    ret = rkmpp_get_packet(avctx, packet, MPP_TIMEOUT_BLOCK);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        *got_packet = 0;
    } else if (ret) {
        return ret;
    } else {
        *got_packet = 1;
    }
    return 0;
}

RKMPP_ENC(h264, AV_CODEC_ID_H264, "h264_mp4toannexb")
RKMPP_ENC(hevc, AV_CODEC_ID_HEVC, "hevc_mp4toannexb")
