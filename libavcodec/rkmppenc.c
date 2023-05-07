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
    int input_timeout = 100;
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
    return 0;

fail:
    av_log(avctx, AV_LOG_ERROR, "Failed to initialize RKMPP Codec.\n");
    if(packet)
        mpp_packet_deinit(&packet);
    return ret;
}

static int rkmpp_config(AVCodecContext *avctx, RKMPPCodec *codec, MppFrame mppframe){
    MppEncCfg cfg = codec->enccfg;
    RK_U32 rc_mode, rc_qpinit, split_mode, split_arg, split_out, fps_in_num, fps_in_den, fps_out_num, fps_out_den;
    MppCodingType coding_type = rkmpp_get_codingtype(avctx);
    MppEncHeaderMode header_mode;
    MppFrameFormat mpp_format = mpp_frame_get_fmt(mppframe) & MPP_FRAME_FMT_MASK;
    int ret;

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
    av_reduce(&fps_in_num, &fps_in_den, avctx->time_base.den, avctx->time_base.num, 65535);
    if (avctx->framerate.num > 0 && avctx->framerate.den > 0)
        av_reduce(&fps_out_num, &fps_out_den, avctx->framerate.num, avctx->framerate.den, 65535);
    else
        av_reduce(&fps_out_num, &fps_out_den, avctx->time_base.den, avctx->time_base.num, 65535);

    /* fix input / output frame rate */
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fps_in_num);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denorm", fps_in_den);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num",fps_out_num);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denorm", fps_out_den);

    mpp_enc_cfg_set_s32(cfg, "rc:gop", FFMAX(avctx->gop_size, 1));

    // config rc: mode
    rc_mode = MPP_ENC_RC_MODE_VBR;
    if (avctx->flags & AV_CODEC_FLAG_QSCALE)
        rc_mode = MPP_ENC_RC_MODE_FIXQP;
    else if (avctx->bit_rate > 0 && avctx->rc_max_rate == avctx->bit_rate)
        rc_mode = MPP_ENC_RC_MODE_CBR;
    else if (avctx->bit_rate > 0)
        rc_mode = MPP_ENC_RC_MODE_AVBR;

    mpp_enc_cfg_set_u32(cfg, "rc:mode", rc_mode);

    // config rc: bps
    mpp_enc_cfg_set_u32(cfg, "rc:bps_target", avctx->bit_rate);
    switch (rc_mode) {
        case MPP_ENC_RC_MODE_FIXQP : {
            /* do not setup bitrate on FIXQP mode */
            break;
        }
        case MPP_ENC_RC_MODE_CBR : {
            /* CBR mode has narrow bound */
            mpp_enc_cfg_set_s32(cfg, "rc:bps_max", avctx->bit_rate * 17 / 16);
            mpp_enc_cfg_set_s32(cfg, "rc:bps_min", avctx->bit_rate * 15 / 16);
            break;
        }
        case MPP_ENC_RC_MODE_VBR :
        case MPP_ENC_RC_MODE_AVBR : {
            /* VBR mode has wide bound */
            mpp_enc_cfg_set_s32(cfg, "rc:bps_max", avctx->bit_rate * 17 / 16);
            mpp_enc_cfg_set_s32(cfg, "rc:bps_min", avctx->bit_rate * 1 / 16);
            break;
        }
        default : {
            /* default use CBR mode */
            mpp_enc_cfg_set_s32(cfg, "rc:bps_max", avctx->bit_rate * 17 / 16);
            mpp_enc_cfg_set_s32(cfg, "rc:bps_min", avctx->bit_rate * 15 / 16);
            break;
        }
    }

    // config rc: drop behaviour
    mpp_enc_cfg_set_u32(cfg, "rc:drop_mode", MPP_ENC_RC_DROP_FRM_DISABLED);
    mpp_enc_cfg_set_u32(cfg, "rc:drop_thd", 20); // 20% of max bps
    mpp_enc_cfg_set_u32(cfg, "rc:drop_gap", 1); // Do not continuous drop frame

    // config rc: qp
    switch (coding_type) {
        case MPP_VIDEO_CodingAVC :
        case MPP_VIDEO_CodingHEVC : {
            switch (rc_mode) {
                case MPP_ENC_RC_MODE_FIXQP : {
                    rc_qpinit = 10 + avctx->global_quality / (FF_QP2LAMBDA << 2);
                    mpp_enc_cfg_set_s32(cfg, "rc:qp_init", rc_qpinit);
                    mpp_enc_cfg_set_s32(cfg, "rc:qp_max", rc_qpinit);
                    mpp_enc_cfg_set_s32(cfg, "rc:qp_min", rc_qpinit);
                    mpp_enc_cfg_set_s32(cfg, "rc:qp_max_i", rc_qpinit);
                    mpp_enc_cfg_set_s32(cfg, "rc:qp_min_i", rc_qpinit);
                    mpp_enc_cfg_set_s32(cfg, "rc:qp_ip", 0);
                    break;
                }
                case MPP_ENC_RC_MODE_CBR :
                case MPP_ENC_RC_MODE_VBR :
                case MPP_ENC_RC_MODE_AVBR : {
                    mpp_enc_cfg_set_s32(cfg, "rc:qp_init", -1);
                    mpp_enc_cfg_set_s32(cfg, "rc:qp_max", 51);
                    mpp_enc_cfg_set_s32(cfg, "rc:qp_min", 10);
                    mpp_enc_cfg_set_s32(cfg, "rc:qp_max_i",51);
                    mpp_enc_cfg_set_s32(cfg, "rc:qp_min_i", 10);
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
            mpp_enc_cfg_set_s32(cfg, "rc:qp_init", 40);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_max", 127);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_min", 0);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_max_i", 127);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_min_i", 0);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_ip", 6);
            break;
        }
        case MPP_VIDEO_CodingMJPEG : {
            // jpeg use special codec config to control qtable
            mpp_enc_cfg_set_s32(cfg, "jpeg:q_factor", 80);
            mpp_enc_cfg_set_s32(cfg, "jpeg:qf_max", 99);
            mpp_enc_cfg_set_s32(cfg, "jpeg:qf_min", 1);
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
             mpp_enc_cfg_set_s32(cfg, "h264:profile", avctx->profile > 0 ? avctx->profile : FF_PROFILE_H264_HIGH);
             mpp_enc_cfg_set_s32(cfg, "h264:level", avctx->level != FF_LEVEL_UNKNOWN ? avctx->level : 40);
             mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", 1);
             mpp_enc_cfg_set_s32(cfg, "h264:cabac_idc", 0);
             mpp_enc_cfg_set_s32(cfg, "h264:trans8x8", 1);
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

static int rkmpp_send_frame(AVCodecContext *avctx, AVFrame *frame){
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;
    MppFrame mppframe = NULL;
    int ret=0, clean=0;

    // EOS frame, avframe=NULL
    if (!frame) {
        av_log(avctx, AV_LOG_DEBUG, "End of stream.\n");
        mpp_frame_init(&mppframe);
        mpp_frame_set_eos(mppframe, 1);
    } else {
        //MPPFrame from rkmppdec.c
        if (mpp_frame_get_buffer(frame->data[3])){
            mppframe = (MppFrame)frame->data[3];
        // any other ffmpeg buffer
        } else {
            mppframe = av_nv12_mpp_nv12(avctx, frame);
            clean = 1;
        }

        if (frame->pict_type == AV_PICTURE_TYPE_I) {
            av_log(avctx, AV_LOG_ERROR, "TYPE_I for testing.\n");
        }
        mpp_frame_set_pts(mppframe, frame->pts);
        mpp_frame_set_dts(mppframe, frame->pkt_dts);
    }

    // there coould be better ways to config the encoder, but lets do it this way atm.
    // FIXME: this is ugly
    if(!codec->hasconfig && !rkmpp_config(avctx, codec, mppframe))
        codec->hasconfig = 1;

    // put the frame in encoder
    ret = codec->mpi->encode_put_frame(codec->ctx, mppframe);

    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_DEBUG, "Encoder buffer full\n");
        ret = AVERROR(EAGAIN);
    } else
        av_log(avctx, AV_LOG_DEBUG, "Wrote %ld bytes to encoder\n", mpp_frame_get_buf_size(mppframe));

    if (clean)
        mpp_frame_deinit(&mppframe);
    return ret;
}


static int rkmpp_get_packet(AVCodecContext *avctx, AVPacket *packet, int timeout){
    RKMPPCodecContext *rk_context = avctx->priv_data;
    RKMPPCodec *codec = (RKMPPCodec *)rk_context->codec_ref->data;
    MppPacket mpppacket = NULL;
    MppMeta meta = NULL;
    int ret, keyframe;

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
    packet->pts = mpp_packet_get_pts(mpppacket);
    packet->dts = mpp_packet_get_dts(mpppacket);
    if (packet->pts <= 0)
        packet->pts = packet->dts;
    if (packet->dts <= 0)
        packet->dts = packet->pts;
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
RKMPP_ENC(vp8, AV_CODEC_ID_VP8, NULL)
