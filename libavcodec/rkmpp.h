#include <drm_fourcc.h>
#include <rockchip/rk_mpi.h>
#include <unistd.h>
#include <stdint.h>

#include "internal.h"
#include "codec_internal.h"
#include "avcodec.h"
#include "hwconfig.h"
#include "decode.h"
#include "encode.h"
#include "rga/RgaApi.h"
#include "libavutil/macros.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"
#include "libavutil/buffer.h"
#include "libavutil/pixfmt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/hwcontext_drm.h"

// HACK: Older BSP kernel use NA12 for NV15.
#ifndef DRM_FORMAT_NV15 // fourcc_code('N', 'V', '1', '5')
#define DRM_FORMAT_NV15 fourcc_code('N', 'A', '1', '2')
#endif

#define RKMPP_FPS_FRAME_MACD 30
#define RKMPP_STRIDE_ALIGN 16
#define RKMPP_RGA_MIN_SIZE 128
#define RKMPP_RGA_MAX_SIZE 4096
#define RKMPP_MPPFRAME_BUFINDEX 7
#define RKMPP_DMABUF_COUNT 16
#define RKMPP_DMABUF_RGA_COUNT 4
#define HDR_SIZE 1024
#define QMAX_H26x 51
#define QMIN_H26x 10
#define QMAX_VPx 127
#define QMIN_VPx 40
#define QMAX_JPEG 99
#define QMIN_JPEG 1
#define KEEP 0
#define SHR 0
#define SHL 1
#define MUL 2
#define DIV 3

#define DRMFORMATNAME(buf, format) \
    buf[0] = format & 0xff; \
    buf[1] = (format >> 8) & 0xff; \
    buf[2] = (format >> 16) & 0xff; \
    buf[3] = (format >> 24) & 0x7f; \

typedef struct {
    MppCtx ctx;
    MppApi *mpi;
    MppBufferGroup buffer_group;
    MppBufferGroup buffer_group_rga;
    MppCtxType mppctxtype;
    MppEncCfg enccfg;
    int hascfg;
    int64_t ptsstep;
    int64_t pts;

    AVPacket lastpacket;
    AVFrame lastframe;
    AVBufferRef *hwframes_ref;
    AVBufferRef *hwdevice_ref;
    int dma_fd;

    char print_fps;
    uint64_t last_frame_time;
    uint64_t frames;
    uint64_t latencies[RKMPP_FPS_FRAME_MACD];

    int8_t norga;
    int (*init_callback)(struct AVCodecContext *avctx);
} RKMPPCodec;

typedef struct {
    int offset;
    int hstride;
    int width;
    int height;
    int size;
} plane;

typedef struct {
    plane plane[3];
    int hstride;
    int vstride;
    int size;
    int width;
    int height;
    enum AVPixelFormat avformat;
} planedata;

typedef struct {
    enum AVPixelFormat av;
    MppFrameFormat mpp;
    uint32_t drm;
    enum _Rga_SURF_FORMAT rga;
    int numplanes;
    planedata planedata;
    int mode;
} rkformat;

typedef struct {
    AVClass *av_class;
    AVBufferRef *codec_ref;
    int rc_mode;
    int profile;
    int qmin;
    int qmax;
    int level;
    int coder;
    int dct8x8;
    int postrga_width;
    int postrga_height;
    rkformat rgaformat;
    rkformat rkformat;
    rkformat nv12format;
    planedata avplanes;
    planedata nv12planes;
    planedata rgaplanes;
} RKMPPCodecContext;

MppCodingType rkmpp_get_codingtype(AVCodecContext *avctx);
int rkmpp_get_drm_format(rkformat *format, uint32_t informat);
int rkmpp_get_mpp_format(rkformat *format, MppFrameFormat informat);
int rkmpp_get_rga_format(rkformat *format, enum _Rga_SURF_FORMAT informat);
int rkmpp_get_av_format(rkformat *format, enum AVPixelFormat informat);
int rkmpp_init_encoder(AVCodecContext *avctx);
int rkmpp_encode(AVCodecContext *avctx, AVPacket *packet, const AVFrame *frame, int *got_packet);
int rkmpp_init_decoder(AVCodecContext *avctx);
int rkmpp_receive_frame(AVCodecContext *avctx, AVFrame *frame);
int rkmpp_init_codec(AVCodecContext *avctx);
int rkmpp_close_codec(AVCodecContext *avctx);
void rkmpp_release_codec(void *opaque, uint8_t *data);
void rkmpp_flush(AVCodecContext *avctx);
uint64_t rkmpp_update_latency(AVCodecContext *avctx, int latency);
int rkmpp_planedata(rkformat *format, planedata *planes, int width, int height, int align);
void rkmpp_buffer_free(MppBufferInfo *dma_info);
MPP_RET rkmpp_buffer_set(AVCodecContext *avctx, size_t size, MppBufferGroup buffer_group, int count);

#define OFFSET(x) offsetof(RKMPPCodecContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM

#define ENCODEROPTS() \
    { "rc_mode", "Set rate control mode", OFFSET(rc_mode), AV_OPT_TYPE_INT, \
            { .i64 = MPP_ENC_RC_MODE_CBR }, MPP_ENC_RC_MODE_VBR, MPP_ENC_RC_MODE_BUTT, VE, "rc_mode"}, \
        {"VBR", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MPP_ENC_RC_MODE_VBR }, 0, 0, VE, "rc_mode" }, \
        {"CBR", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MPP_ENC_RC_MODE_CBR }, 0, 0, VE, "rc_mode" }, \
        {"CQP", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MPP_ENC_RC_MODE_FIXQP }, 0, 0, VE, "rc_mode" }, \
        {"AVBR", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MPP_ENC_RC_MODE_AVBR }, 0, 0, VE, "rc_mode" }, \
    { "quality_min", "Minimum Quality", OFFSET(qmin), AV_OPT_TYPE_INT, \
        { .i64=50 }, 0, 100, VE, "qmin"}, \
    { "quality_max", "Maximum Quality", OFFSET(qmax), AV_OPT_TYPE_INT, \
            { .i64=100 }, 0, 100, VE, "qmax"}, \
    { "width", "scale to Width", OFFSET(postrga_width), AV_OPT_TYPE_INT, \
             { .i64=0 }, 0, RKMPP_RGA_MAX_SIZE, VE, "width"}, \
    { "height", "scale to Height", OFFSET(postrga_height), AV_OPT_TYPE_INT, \
             { .i64=0 }, 0, RKMPP_RGA_MAX_SIZE, VE, "height"},

static const AVOption options_h264_encoder[] = {
    ENCODEROPTS()
    { "profile", "Set profile restrictions", OFFSET(profile), AV_OPT_TYPE_INT,
            { .i64=FF_PROFILE_H264_HIGH }, -1, FF_PROFILE_H264_HIGH, VE, "profile"},
        { "baseline",   NULL, 0, AV_OPT_TYPE_CONST, {.i64 = FF_PROFILE_H264_BASELINE},  INT_MIN, INT_MAX, VE, "profile" },
        { "main",       NULL, 0, AV_OPT_TYPE_CONST, {.i64 = FF_PROFILE_H264_MAIN},      INT_MIN, INT_MAX, VE, "profile" },
        { "high",       NULL, 0, AV_OPT_TYPE_CONST, {.i64 = FF_PROFILE_H264_HIGH},      INT_MIN, INT_MAX, VE, "profile" },
    { "level", "Compression Level", OFFSET(level), AV_OPT_TYPE_INT,
            { .i64 = 0 }, FF_LEVEL_UNKNOWN, 0xff, VE, "level"},
        { "1",          NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 10 }, 0, 0, VE, "level"},
        { "1.1",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 11 }, 0, 0, VE, "level"},
        { "1.2",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 12 }, 0, 0, VE, "level"},
        { "1.3",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 13 }, 0, 0, VE, "level"},
        { "2",          NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 20 }, 0, 0, VE, "level"},
        { "2.1",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 21 }, 0, 0, VE, "level"},
        { "2.2",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 22 }, 0, 0, VE, "level"},
        { "3",          NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 30 }, 0, 0, VE, "level"},
        { "3.1",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 31 }, 0, 0, VE, "level"},
        { "3.2",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 32 }, 0, 0, VE, "level"},
        { "4",          NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 40 }, 0, 0, VE, "level"},
        { "4.1",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 41 }, 0, 0, VE, "level"},
        { "4.2",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 42 }, 0, 0, VE, "level"},
        { "5",          NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 50 }, 0, 0, VE, "level"},
        { "5.1",           NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 51 }, 0, 0, VE, "level"},
        { "5.2",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 52 }, 0, 0, VE, "level"},
        { "6",          NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 60 }, 0, 0, VE, "level"},
        { "6.1",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 61 }, 0, 0, VE, "level"},
        { "6.2",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 62 }, 0, 0, VE, "level"},
    { "coder", "Entropy coder type (from 0 to 1) (default cabac)", OFFSET(coder), AV_OPT_TYPE_INT,
            { .i64 = 1 }, 0, 1, VE, "coder"},
        { "cavlc", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 0 }, INT_MIN, INT_MAX, VE, "coder" },
        { "cabac", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 1 }, INT_MIN, INT_MAX, VE, "coder" },
    { "8x8dct", "High profile 8x8 transform.",  OFFSET(dct8x8), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, VE},
    { NULL }
};

static const AVOption options_hevc_encoder[] = {
    ENCODEROPTS()
    { "level", "Compression Level", OFFSET(level), AV_OPT_TYPE_INT,
            { .i64 = 0 }, FF_LEVEL_UNKNOWN, 0xff, VE, "level"},
        { "1",          NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 30 }, 0, 0, VE, "level"},
        { "2",          NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 60 }, 0, 0, VE, "level"},
        { "2.1",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 63 }, 0, 0, VE, "level"},
        { "3",          NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 90 }, 0, 0, VE, "level"},
        { "3.1",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 93 }, 0, 0, VE, "level"},
        { "4",          NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 120 }, 0, 0, VE, "level"},
        { "4.1",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 123 }, 0, 0, VE, "level"},
        { "5",          NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 150 }, 0, 0, VE, "level"},
        { "5.1",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 153 }, 0, 0, VE, "level"},
        { "5.2",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 156 }, 0, 0, VE, "level"},
        { "6",          NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 180 }, 0, 0, VE, "level"},
        { "6.1",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 183 }, 0, 0, VE, "level"},
        { "6.2",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 186 }, 0, 0, VE, "level"},
        { "8.5",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 255 }, 0, 0, VE, "level"},
    { NULL }
};

static const AVOption options_vp8_encoder[] = {
    ENCODEROPTS()
    { NULL }
};

#define DECODEROPTIONS(NAME, TYPE) \
static const AVOption options_##NAME##_##TYPE[] = { \
            { NULL } \
        };

DECODEROPTIONS(h263, decoder);
DECODEROPTIONS(h264, decoder);
DECODEROPTIONS(hevc, decoder);
DECODEROPTIONS(av1, decoder);
DECODEROPTIONS(vp8, decoder);
DECODEROPTIONS(vp9, decoder);
DECODEROPTIONS(mpeg1, decoder);
DECODEROPTIONS(mpeg2, decoder);
DECODEROPTIONS(mpeg4, decoder);

static const FFCodecDefault rkmpp_enc_defaults[] = {
    { "b",  "6M" },
    { "g",  "60" },
    { NULL }
};

static const enum AVPixelFormat rkmpp_vepu1_formats[] = {
        AV_PIX_FMT_NV16,
        AV_PIX_FMT_YUV422P,
        AV_PIX_FMT_YUYV422,
        AV_PIX_FMT_UYVY422,
        AV_PIX_FMT_RGBA,
        AV_PIX_FMT_RGB0,
        AV_PIX_FMT_BGRA,
        AV_PIX_FMT_BGR0,
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_DRM_PRIME,
        AV_PIX_FMT_NONE,
};

static const enum AVPixelFormat rkmpp_vepu5_formats[] = {
        AV_PIX_FMT_NV24,
        AV_PIX_FMT_YUV444P,
        AV_PIX_FMT_NV16,
        AV_PIX_FMT_YUV422P,
        AV_PIX_FMT_BGR24,
        AV_PIX_FMT_YUYV422,
        AV_PIX_FMT_UYVY422,
        AV_PIX_FMT_RGBA,
        AV_PIX_FMT_RGB0,
        AV_PIX_FMT_BGRA,
        AV_PIX_FMT_BGR0,
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_DRM_PRIME,
        AV_PIX_FMT_NONE,
};

static const enum AVPixelFormat rkmpp_vdpu_formats[] = {
        AV_PIX_FMT_DRM_PRIME,
        AV_PIX_FMT_YUYV422,
        AV_PIX_FMT_UYVY422,
        AV_PIX_FMT_BGR24,
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_RGBA,
        AV_PIX_FMT_RGB0,
        AV_PIX_FMT_BGRA,
        AV_PIX_FMT_BGR0,
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_NONE
};

#define RKMPP_CODEC(NAME, ID, BSFS, TYPE) \
        static const AVClass rkmpp_##NAME##_##TYPE##_class = { \
            .class_name = "rkmpp_" #NAME "_" #TYPE, \
            .item_name  = av_default_item_name,\
            .option     = options_##NAME##_##TYPE, \
            .version    = LIBAVUTIL_VERSION_INT, \
        }; \
        const FFCodec ff_##NAME##_rkmpp_##TYPE = { \
            .p.name         = #NAME "_rkmpp_" #TYPE, \
            CODEC_LONG_NAME(#NAME " (rkmpp " #TYPE " )"), \
            .p.type         = AVMEDIA_TYPE_VIDEO, \
            .p.id           = ID, \
            .priv_data_size = sizeof(RKMPPCodecContext), \
            .init           = rkmpp_init_codec, \
            .close          = rkmpp_close_codec, \
            .flush          = rkmpp_flush, \
            .p.priv_class   = &rkmpp_##NAME##_##TYPE##_class, \
            .caps_internal  = FF_CODEC_CAP_NOT_INIT_THREADSAFE, \
            .bsfs           = BSFS, \
            .p.wrapper_name = "rkmpp",


#define RKMPP_DEC(NAME, ID, BSFS) \
        RKMPP_CODEC(NAME, ID, BSFS, decoder) \
        FF_CODEC_RECEIVE_FRAME_CB(rkmpp_receive_frame), \
        .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_HARDWARE, \
        .p.pix_fmts     = rkmpp_vdpu_formats, \
        .hw_configs     = (const AVCodecHWConfigInternal *const []) { HW_CONFIG_INTERNAL(DRM_PRIME), \
                                                                      NULL}, \
    };

#define RKMPP_ENC(NAME, ID, VEPU) \
        RKMPP_CODEC(NAME, ID, NULL, encoder) \
        FF_CODEC_ENCODE_CB(rkmpp_encode), \
        .p.capabilities = AV_CODEC_CAP_HARDWARE, \
        .defaults       = rkmpp_enc_defaults, \
        .p.pix_fmts     = rkmpp_##VEPU##_formats, \
        .hw_configs     = (const AVCodecHWConfigInternal *const []) { HW_CONFIG_INTERNAL(DRM_PRIME), \
                                                                      NULL}, \
    };
