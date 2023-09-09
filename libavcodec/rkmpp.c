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

    av_log(avctx, AV_LOG_VERBOSE, "Closing Codec.\n");
    rkmpp_flush(avctx);

    if (rk_context->buffer_group){
        mpp_buffer_group_clear(rk_context->buffer_group);
        mpp_buffer_group_put(rk_context->buffer_group);
        rk_context->buffer_group = NULL;
    }

    if (rk_context->buffer_group_swap){
        mpp_buffer_group_clear(rk_context->buffer_group_swap);
        mpp_buffer_group_put(rk_context->buffer_group_swap);
        rk_context->buffer_group_swap = NULL;
    }

    if (rk_context->buffer_group_rga){
        mpp_buffer_group_clear(rk_context->buffer_group_rga);
        mpp_buffer_group_put(rk_context->buffer_group_rga);
        rk_context->buffer_group_rga = NULL;
    }

    if(rk_context->dma_fd > 0)
        close(rk_context->dma_fd);

    if (rk_context->mpi) {
        rk_context->mpi->reset(rk_context->ctx);
        mpp_destroy(rk_context->ctx);
        rk_context->ctx = NULL;
    }

    if(rk_context->hwframes_ref)
        av_buffer_unref(&rk_context->hwframes_ref);
    if(rk_context->hwdevice_ref)
        av_buffer_unref(&rk_context->hwdevice_ref);

    av_packet_unref(&rk_context->lastpacket);

    return 0;
}

int rkmpp_init_codec(AVCodecContext *avctx)
{
    RKMPPCodecContext *rk_context = avctx->priv_data;
    MppCodingType codectype = MPP_VIDEO_CodingUnused;
    int ret;

    av_log(avctx, AV_LOG_VERBOSE, "Initializing RKMPP Codec.\n");

    codectype = rkmpp_get_codingtype(avctx);
    if (codectype == MPP_VIDEO_CodingUnused) {
        av_log(avctx, AV_LOG_ERROR, "Unknown codec type (%d).\n", avctx->codec_id);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    ret = mpp_check_support_format(rk_context->mppctxtype, codectype);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Codec type (%d) unsupported by MPP\n", avctx->codec_id);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    // Create the MPP context
    ret = mpp_create(&rk_context->ctx, &rk_context->mpi);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create MPP context (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    rk_context->dma_fd = open("/dev/dma_heap/system-dma32", O_RDWR);
    if (rk_context->dma_fd < 0) {
       av_log(avctx, AV_LOG_ERROR, "Failed to open system-dma32 heap\n");
       ret = AVERROR_UNKNOWN;
       goto fail;
    }

    if(ffcodec(avctx->codec)->cb_type == FF_CODEC_CB_TYPE_RECEIVE_FRAME){
        rk_context->init_callback = rkmpp_init_decoder;
        rk_context->mppctxtype = MPP_CTX_DEC;

        ret = 1;
        rk_context->mpi->control(rk_context->ctx, MPP_DEC_SET_PARSER_FAST_MODE, &ret);
    } else if ((ffcodec(avctx->codec)->cb_type == FF_CODEC_CB_TYPE_ENCODE)){
        rk_context->mppctxtype = MPP_CTX_ENC;
        rk_context->init_callback = rkmpp_init_encoder;
    } else {
        ret = AVERROR(ENOMEM);
        av_log(avctx, AV_LOG_ERROR, "RKMPP Codec can not determine if the mode is decoder or encoder\n");
        goto fail;
    }

    // initialize mpp
    ret = mpp_init(rk_context->ctx, rk_context->mppctxtype, codectype);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to initialize MPP context (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    ret = rk_context->init_callback(avctx);

    if(ret){
        av_log(avctx, AV_LOG_ERROR, "Failed to init Codec (code = %d).\n", ret);
        goto fail;
    }

    return 0;

fail:
    av_log(avctx, AV_LOG_ERROR, "Failed to initialize RKMPP Codec.\n");
    rkmpp_close_codec(avctx);
    return ret;
}

void rkmpp_flush(AVCodecContext *avctx)
{
    RKMPPCodecContext *rk_context = avctx->priv_data;
    av_log(avctx, AV_LOG_VERBOSE, "Flush.\n");

    av_packet_unref(&rk_context->lastpacket);

    rk_context->mpi->reset(rk_context->ctx);

    while (rk_context->mpp_decode_fifo.first){
        MppFrameItem* first = rk_context->mpp_decode_fifo.first;
        rkmpp_fifo_pop(&rk_context->mpp_decode_fifo);
        rkmpp_rga_wait(first, MPP_TIMEOUT_BLOCK, NULL);
        rkmpp_release_mppframe_item(first, NULL);
    }

    while (rk_context->mpp_rga_fifo.first){
        MppFrameItem* first = rk_context->mpp_rga_fifo.first;
        rkmpp_fifo_pop(&rk_context->mpp_rga_fifo);
        rkmpp_rga_wait(first, MPP_TIMEOUT_BLOCK, NULL);
        rkmpp_release_mppframe_item(first, NULL);
    }

    rk_context->hascfg = rk_context->frame_num = rk_context->eof = 0;
}

MPP_RET rkmpp_buffer_alloc(AVCodecContext *avctx, size_t size, MppBufferGroup *buffer_group, int count)
{
    MPP_RET ret=MPP_SUCCESS;
    RKMPPCodecContext *rk_context = avctx->priv_data;

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
        void *ptr;
        struct dma_heap_allocation_data alloc = {
               .len = size,
               .fd_flags = O_CLOEXEC | O_RDWR,
           };

        MppBufferInfo buf_info = {
            .index = i,
            .type  = MPP_BUFFER_TYPE_DMA_HEAP,
            .size  = alloc.len,
        };

        if (ioctl(rk_context->dma_fd, DMA_HEAP_IOCTL_ALLOC, &alloc) == -1)
            return MPP_ERR_MALLOC;

        buf_info.fd = alloc.fd;
        buf_info.ptr = mmap(NULL, alloc.len, PROT_READ | PROT_WRITE, MAP_SHARED, alloc.fd, 0);
        ptr = buf_info.ptr;
        ret = mpp_buffer_commit(*buffer_group, &buf_info);
        if (ret) {
            av_log(avctx, AV_LOG_ERROR, "Failed to commit external buffer group: %d\n", ret);
            return ret;
        }
        if(alloc.fd > 0){
            // FDs are duplicated in mpp buffer pool. to prevent leaks, close original map
            munmap(ptr, alloc.len);
            close(alloc.fd);
        }
    }

    av_log(avctx, AV_LOG_VERBOSE, "Allocated %d buffer(s) of %ld kB as total %ld MB\n", count, (size) >> 10, (count*size) >> 20);

    return MPP_SUCCESS;
}

int rkmpp_fifo_isfull(MppFrameFifo* fifo){
    if(fifo->limit != -1 && fifo->used == fifo->limit)
        return 0;
    return -1;
}

int rkmpp_fifo_isempty(MppFrameFifo* fifo){
    if(!fifo->used)
        return 0;
    return -1;
}

int rkmpp_fifo_push(MppFrameFifo* fifo, MppFrameItem* item){
    if(!rkmpp_fifo_isfull(fifo))
        return AVERROR(EAGAIN);

    item->nextitem = NULL;
    if(fifo->first == NULL)
        fifo->first = item;
    else if (fifo->last == NULL){
        fifo->last = item;
        fifo->first->nextitem = item;
    } else {
        fifo->last->nextitem = item;
        fifo->last = item;
    }

    fifo->used++;
    return 0;
}

int rkmpp_fifo_pop(MppFrameFifo* fifo){
    if(!rkmpp_fifo_isempty(fifo))
        return AVERROR(EAGAIN);

    if(fifo->first->nextitem){
        fifo->first = fifo->first->nextitem;
    } else
        fifo->first = NULL;

    if(fifo->first == fifo->last)
        fifo->last = NULL;

    fifo->used--;
    return 0;
}
