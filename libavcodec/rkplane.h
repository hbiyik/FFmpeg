#include <rockchip/rk_mpi.h>
#include "avcodec.h"

int mpp_nv15_av_yuv420p(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame);
int mpp_nv15_av_nv12(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame);
int mpp_nv12_av_nv12(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame);
int convert_mpp_to_av(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame,
        enum AVPixelFormat informat, enum AVPixelFormat outformat);
MppFrame copy_av_to_mpp(const AVFrame *frame, enum AVPixelFormat avformat, MppBufferGroup buffer_group);
MppFrame import_drm_to_mpp(AVCodecContext *avctx, const AVFrame *frame);
int import_mpp_to_drm(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame);
