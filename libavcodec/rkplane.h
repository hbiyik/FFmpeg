#include <drm_fourcc.h>
#include <rockchip/rk_mpi.h>
#include "avcodec.h"
#include "libavutil/hwcontext_drm.h"
#include "rga/RgaApi.h"
#include "rga/im2d.h"

#define RKMPP_MPPFRAME_BUFINDEX 7
#define RKMPP_RENDER_DEPTH 8
#define RKMPP_FBC_NONE 0
#define RKMPP_FBC_DECODER 1
#define RKMPP_FBC_DRM 2

typedef struct MppFrameItem MppFrameItem;
typedef struct MppFrameFifo MppFrameFifo;

typedef struct {
    int width;
    int width_div;
    int height;
    int height_div;
    int hstride;
    int hstride_div;
} factor;

typedef struct {
    int offset;
    int hstride;
    int width;
    int height;
    int size;
} plane;

typedef struct {
    int offset;
    int splitw;
    int splith;
    int hstride;
    int size;
    int depth;
} uvplane;

typedef struct {
    int hstride;
    int vstride;
    int hstride_pix;
    int vstride_pix;
    int size;
    int overshoot;
    int width;
    int height;
    plane plane[3];
    uvplane uvplane;
} planedata;

typedef struct {
    enum AVPixelFormat av;
    MppFrameFormat mpp;
    uint32_t drm;
    uint32_t drm_fbc;
    enum _Rga_SURF_FORMAT rga;
    planedata planedata;
    int numplanes;
    int bpp;
    int bpp_div;
    int qual;
    int rga_uncompact;
    int rga_msb_aligned;
    int fbcstride;
    int ypixoffset;
    factor factors[3];
} rkformat;

struct MppFrameItem {
    MppFrame mppframe;
    int rgafence;
    uint64_t framenum;
    MppFrameItem* rga;
    MppFrameItem* nextitem;
    rkformat* format;
    struct timespec timestamp;
    struct timespec old_timestamp;
    char type[6];
};

struct MppFrameFifo {
    MppFrameItem* first;
    MppFrameItem* last;
    int limit;
    int used;
};

typedef struct {
    char  *name;
} rkmpp_frame_type;

enum codec_flow {
    FLOWUNSET,
    SWAPANDCONVERT,
    CONVERT,
    NOCONVERSION
};

typedef struct {
    AVClass *av_class;
    MppCtx ctx;
    MppApi *mpi;
    MppBufferGroup buffer_group;
    MppBufferGroup buffer_group_rga;
    MppBufferGroup buffer_group_swap;
    MppCtxType mppctxtype;
    MppEncCfg enccfg;
    int hascfg;
    int64_t ptsstep;
    int64_t pts;
    AVPacket lastpacket;
    AVBufferRef *hwframes_ref;
    AVBufferRef *hwdevice_ref;
    int dma_fd;
    char timing;
    int drm_hdrbits;
    uint64_t frame_num;
    uint64_t eof;
    int libyuv;
    int (*init_callback)(struct AVCodecContext *avctx);
    int rc_mode;
    int profile;
    int qmin;
    int qmax;
    int level;
    int coder;
    int dct8x8;
    int rga_width;
    int rga_height;
    rkformat informat;
    rkformat swapformat;
    rkformat outformat;
    MppFrameFifo mpp_decode_fifo;
    MppFrameFifo mpp_rga_fifo;
    int fbc;
    int dma_enc_count;
    int dma_dec_count;
    int dma_swap_count;
    int dma_rga_count;
    enum codec_flow codec_flow;
} RKMPPCodecContext;


int rkmpp_get_drm_format(rkformat *format, uint32_t informat, int width, int height, int align,
        int hstride, int vstride, int ypixoffset, int overshoot, int fbcstride, factor* uvfactor);
int rkmpp_get_mpp_format(rkformat *format, MppFrameFormat informat, int width, int height, int align,
        int hstride, int vstride, int ypixoffset, int overshoot, int fbcstride, factor* uvfactor);
int rkmpp_get_rga_format(rkformat *format, enum _Rga_SURF_FORMAT informat, int width, int height, int align,
        int hstride, int vstride, int ypixoffset, int overshoot, int fbcstride, factor* uvfactor);
int rkmpp_get_av_format(rkformat *format, enum AVPixelFormat informat, int width, int height, int align,
        int hstride, int vstride, int ypixoffset, int overshoot, int fbcstride, factor* uvfactor);
void rkmpp_planedata(rkformat *format, int width, int height, int align, int hstride, int vstride,
        int ypixoffset, int overshoot, int fbcstride, factor* uvfactor);
int rkmpp_set_mppframe_to_av(AVCodecContext *avctx, MppFrameItem *cache, AVFrame* frame, int index);
int rkmpp_set_drmdesc_to_av(AVDRMFrameDescriptor *desc, AVFrame *frame);
int rkmpp_rga_convert_mpp_mpp(AVCodecContext *avctx, MppFrame in_mppframe, rkformat* in_format,
        MppFrame out_mppframe, rkformat* out_format, int* outfence);
void rkmpp_semi_to_planar_soft(MppFrame mpp_inframe, rkformat* informat, MppFrame mpp_outframe, rkformat* outformat);
MppFrame rkmpp_create_mpp_frame(int width, int height, rkformat *rkformat, MppBufferGroup buffer_group,
        AVDRMFrameDescriptor *desc, AVFrame *frame);
MppFrame rkmpp_mppframe_from_av(AVFrame *frame);
void rkmpp_transfer_mpp_props(MppFrame inframe, MppFrame outframe);
void rkmpp_release_mppframe_item(void *opaque, uint8_t *data);
int rkmpp_rga_wait(MppFrameItem* item, int timeout, struct timespec* ts);
int rkmpp_fifo_push(MppFrameFifo* fifo, MppFrameItem* item);
int rkmpp_fifo_pop(MppFrameFifo* fifo);
int rkmpp_fifo_isfull(MppFrameFifo* fifo);
int rkmpp_fifo_isempty(MppFrameFifo* fifo);
int rkmpp_check_rga_dimensions(RKMPPCodecContext *rk_context);
