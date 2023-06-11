#include <rockchip/rk_mpi.h>
#include "avcodec.h"

int mpp_nv12_av_yuv420p(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame);
int mpp_nv15_av_yuv420p(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame);
int mpp_nv16_av_yuv420p(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame);
int mpp_nv12_av_nv12(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame);
int mpp_nv15_av_nv12(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame);
int mpp_nv16_av_nv12(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame);
MppFrame av_yuv420p_mpp_nv12(AVCodecContext *avctx, AVFrame *frame);
MppFrame av_nv12_mpp_nv12(AVCodecContext *avctx, AVFrame *frame);
