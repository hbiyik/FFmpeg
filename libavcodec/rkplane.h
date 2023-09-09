#include <rockchip/rk_mpi.h>
#include "avcodec.h"

typedef struct {
    char  *name;
} rkmpp_frame_type;

int rga_convert_mpp_mpp(AVCodecContext *avctx, MppFrame in_mppframe, MppFrame out_mppframe);
int mpp_nv15_av_yuv420p(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame);
int mpp_nv15_av_nv12(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame);
int mpp_nv12_av_nv12(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame);
int convert_mpp_to_av(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame,
        enum AVPixelFormat informat, enum AVPixelFormat outformat);
MppFrame create_mpp_frame(int width, int height, rkformat *rkformat, MppBufferGroup buffer_group,
        AVDRMFrameDescriptor *desc, AVFrame *frame, planedata *planedata);
MppFrame import_drm_to_mpp(AVCodecContext *avctx, AVFrame *frame);
int import_mpp_to_drm(AVCodecContext *avctx, MppFrame mppframe, AVFrame *frame);
MppFrame get_mppframe_from_av(AVFrame *frame);
