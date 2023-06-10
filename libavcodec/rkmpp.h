#include <drm_fourcc.h>
#include <rockchip/rk_mpi.h>
#include <unistd.h>

#include "internal.h"
#include "codec_internal.h"
#include "avcodec.h"
#include "hwconfig.h"
#include "decode.h"
#include "encode.h"
#include "libavutil/log.h"
#include "libavutil/buffer.h"
#include "libavutil/pixfmt.h"


// HACK: Older BSP kernel use NA12 for NV15.
#ifndef DRM_FORMAT_NV15 // fourcc_code('N', 'V', '1', '5')
#define DRM_FORMAT_NV15 fourcc_code('N', 'A', '1', '2')
#endif

#define RKMPP_FPS_FRAME_MACD 30
#define RKMPP_STRIDE_ALIGN 16
#define HDR_SIZE (1024)

typedef struct {
    AVClass *av_class;
    AVBufferRef *codec_ref;
} RKMPPCodecContext;

typedef struct {
    MppCtx ctx;
    MppApi *mpi;
    MppBufferGroup buffer_group;
    MppCtxType mppctxtype;
    MppEncCfg enccfg;
    uint32_t hasconfig;

    AVPacket lastpacket;
    AVFrame lastframe;
    AVBufferRef *hwframes_ref;
    AVBufferRef *hwdevice_ref;

    char print_fps;

    uint64_t last_frame_time;
    uint64_t frames;
    uint64_t latencies[RKMPP_FPS_FRAME_MACD];

    uint32_t drm_format;
    int rga_fd;
    int8_t norga;
    int (*buffer_callback)(struct AVCodecContext *avctx, MppFrame mppframe, struct AVFrame *frame);
    int (*init_callback)(struct AVCodecContext *avctx);

} RKMPPCodec;

MppCodingType rkmpp_get_codingtype(AVCodecContext *avctx);
int rkmpp_init_encoder(AVCodecContext *avctx);
int rkmpp_init_decoder(AVCodecContext *avctx);
AVBufferRef *set_mppframe_to_avbuff(MppFrame mppframe);
int rkmpp_close_codec(AVCodecContext *avctx);
void rkmpp_release_codec(void *opaque, uint8_t *data);
int rkmpp_init_codec(AVCodecContext *avctx);
void rkmpp_flush(AVCodecContext *avctx);
uint64_t rkmpp_update_latency(AVCodecContext *avctx, uint64_t latency);

int rkmpp_receive_frame(AVCodecContext *avctx, AVFrame *frame);
int rkmpp_encode(AVCodecContext *avctx, AVPacket *packet, const AVFrame *frame, int *got_packet);

#define RKMPP_CODEC(NAME, ID, BSFS, TYPE) \
        static const AVClass rkmpp_##NAME##_##TYPE##_class = { \
            .class_name = "rkmpp_" #NAME "_" #TYPE, \
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
            .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_HARDWARE, \
            .caps_internal  = FF_CODEC_CAP_NOT_INIT_THREADSAFE | FF_CODEC_CAP_CONTIGUOUS_BUFFERS, \
            .bsfs           = BSFS, \
            .p.wrapper_name = "rkmpp",


#define RKMPP_DEC(NAME, ID, BSFS) \
        RKMPP_CODEC(NAME, ID, BSFS, decoder) \
        FF_CODEC_RECEIVE_FRAME_CB(rkmpp_receive_frame), \
        .p.pix_fmts     = (const enum AVPixelFormat[]) { AV_PIX_FMT_DRM_PRIME, \
                                                         AV_PIX_FMT_NV12, \
                                                         AV_PIX_FMT_YUV420P, \
                                                         AV_PIX_FMT_NONE}, \
        .hw_configs     = (const AVCodecHWConfigInternal *const []) { HW_CONFIG_INTERNAL(DRM_PRIME), \
                                                                      HW_CONFIG_INTERNAL(NV12), \
                                                                      NULL}, \
    };

#define RKMPP_ENC(NAME, ID, BSFS) \
        RKMPP_CODEC(NAME, ID, BSFS, encoder) \
        FF_CODEC_ENCODE_CB(rkmpp_encode), \
        .p.pix_fmts     = (const enum AVPixelFormat[]) { AV_PIX_FMT_NV12, \
                                                         AV_PIX_FMT_NONE}, \
        .hw_configs     = (const AVCodecHWConfigInternal *const []) { HW_CONFIG_INTERNAL(NV12), \
                                                                      NULL}, \
    };
