#include "mppdecoder.h"
#include <string.h>
#include <pthread.h>
#include "tools.h"
#include "rkdrm.h"


#if DEBUG
#define mpp_log printf
#define mpp_err printf
void mpp_dump(uint8_t* data, int sz)
{
   int i = 0;
   for (i = 0; i < sz; i++) {
       printf("%02x ", data[i]);
       if ((i & 0x0f) == 0x0f) printf("\n");
   }
   if ((sz & 0x0f) != 0) printf("\n");
}
#else
#define mpp_log
#define mpp_err
#define mpp_dump
#endif

/* end of stream flag when set quit the loop */
RK_U32          eos;
/* input and output */
MppBufferGroup  frm_grp;
MppBufferGroup  pkt_grp;
RK_U64          frame_count = 0;
RK_U64          frame_discards = 0;
RK_U64          frame_err = 0;
/* FPS calculator */
RK_U64 fps_ms;
RK_U32 fps_counter;
float fps = 0.0;

MppApi *mpi;
MppCtx ctx;

//RK_U32          pkt_eos;

int rkmpp_decoder_init(RKMPPCodecContext *mpp_ctx)
{
    MPP_RET ret = MPP_OK;
    MpiCmd mpi_cmd = MPP_CMD_BASE;
    MppParam param = NULL;
    RK_U32 need_split = 1;

	mpp_ctx->codectype = MPP_VIDEO_CodingAVC;

    ret = mpp_create(&mpp_ctx->ctx, &mpp_ctx->mpi);
    if (MPP_OK != ret) {
        mpp_err("mpi->control failed\n");
        return -1;
    }

    mpi_cmd = MPP_DEC_SET_PARSER_SPLIT_MODE;
    param = &need_split;
    ret = mpp_ctx->mpi->control(mpp_ctx->ctx, mpi_cmd, param);
    if (MPP_OK != ret) {
        mpp_err("mpi->control failed\n");
        return -1;
    }

	/* 
		MPP_VIDEO_CodingAVC;
		MPP_VIDEO_CodingHEVC;
		MPP_VIDEO_CodingVP8;
		MPP_VIDEO_CodingVP9;
		MPP_VIDEO_CodingUnused;
	*/
    ret = mpp_init(mpp_ctx->ctx, MPP_CTX_DEC, mpp_ctx->codectype);
    if (MPP_OK != ret) {
        mpp_err("mpp_init failed\n");
        return -1;
    }

    fps_ms = current_ms();
    fps_counter = 0;

    return 0;
}

void decode_one_pkt(RKMPPCodecContext *mpp_ctx)
{
    int ret;
    RK_U32 pkt_done = 0;
	AVPacket *avpkt = mpp_ctx->avpkt;
	MppPacket packet;
	MppFrame frame;

    ret = mpp_packet_init(&packet, avpkt->data, avpkt->size);
    if (MPP_OK != ret) {
        mpp_err("mpp_packet_init failed\n");
        return;
    }

	mpp_packet_set_pts(packet, avpkt->pts);
    if (!(avpkt->data))
        mpp_packet_set_eos(packet);

    do {

        RK_S32 times = 5;
        // send the packet first if packet is not done
        if (!pkt_done) {
            ret = mpp_ctx->mpi->decode_put_packet(mpp_ctx->ctx, packet);
            if (MPP_OK == ret)
                pkt_done = 1;
        }

        // then get all available frame and release
        do {
            RK_S32 get_frm = 0;
            RK_U32 frm_eos = 0;

        try_again:
            ret = mpp_ctx->mpi->decode_get_frame(mpp_ctx->ctx, &frame);
            if (MPP_ERR_TIMEOUT == ret) {
                if (times > 0) {
                    times--;
                    msleep(MPP_H264_DECODE_TIMEOUT);
                    goto try_again;
                }
                mpp_err("decode_get_frame failed too much time\n");
            }
            if (MPP_OK != ret) {
                mpp_err("decode_get_frame failed ret %d\n", ret);
                break;
            }

            if (frame) {
                if (mpp_frame_get_info_change(frame)) {
                    RK_U32 width = mpp_frame_get_width(frame);
                    RK_U32 height = mpp_frame_get_height(frame);
                    RK_U32 hor_stride = mpp_frame_get_hor_stride(frame);
                    RK_U32 ver_stride = mpp_frame_get_ver_stride(frame);

                    mpp_log("decode_get_frame get info changed found\n");
                    mpp_log("decoder require buffer w:h [%d:%d] stride [%d:%d]\n",
                            width, height, hor_stride, ver_stride);

                    /*
                     * NOTE: We can choose decoder's buffer mode here.
                     * There are three mode that decoder can support:
                     *
                     * Mode 1: Pure internal mode
                     * In the mode user will NOT call MPP_DEC_SET_EXT_BUF_GROUP
                     * control to decoder. Only call MPP_DEC_SET_INFO_CHANGE_READY
                     * to let decoder go on. Then decoder will use create buffer
                     * internally and user need to release each frame they get.
                     *
                     * Advantage:
                     * Easy to use and get a demo quickly
                     * Disadvantage:
                     * 1. The buffer from decoder may not be return before
                     * decoder is close. So memroy leak or crash may happen.
                     * 2. The decoder memory usage can not be control. Decoder
                     * is on a free-to-run status and consume all memory it can
                     * get.
                     * 3. Difficult to implement zero-copy display path.
                     *
                     * Mode 2: Half internal mode
                     * This is the mode current test code using. User need to
                     * create MppBufferGroup according to the returned info
                     * change MppFrame. User can use mpp_buffer_group_limit_config
                     * function to limit decoder memory usage.
                     *
                     * Advantage:
                     * 1. Easy to use
                     * 2. User can release MppBufferGroup after decoder is closed.
                     *    So memory can stay longer safely.
                     * 3. Can limit the memory usage by mpp_buffer_group_limit_config
                     * Disadvantage:
                     * 1. The buffer limitation is still not accurate. Memory usage
                     * is 100% fixed.
                     * 2. Also difficult to implement zero-copy display path.
                     *
                     * Mode 3: Pure external mode
                     * In this mode use need to create empty MppBufferGroup and
                     * import memory from external allocator by file handle.
                     * On Android surfaceflinger will create buffer. Then
                     * mediaserver get the file handle from surfaceflinger and
                     * commit to decoder's MppBufferGroup.
                     *
                     * Advantage:
                     * 1. Most efficient way for zero-copy display
                     * Disadvantage:
                     * 1. Difficult to learn and use.
                     * 2. Player work flow may limit this usage.
                     * 3. May need a external parser to get the correct buffer
                     * size for the external allocator.
                     *
                     * The required buffer size caculation:
                     * hor_stride * ver_stride * 3 / 2 for pixel data
                     * hor_stride * ver_stride / 2 for extra info
                     * Total hor_stride * ver_stride * 2 will be enough.
                     *
                     * For H.264/H.265 20+ buffers will be enough.
                     * For other codec 10 buffers will be enough.
                     */
                    ret = mpp_buffer_group_get_internal(&frm_grp, MPP_BUFFER_TYPE_DRM);
                    if (ret) {
                        mpp_err("get mpp buffer group  failed ret %d\n", ret);
                        break;
                    }
                    mpp_ctx->mpi->control(mpp_ctx->ctx, MPP_DEC_SET_EXT_BUF_GROUP, frm_grp);
                    mpp_ctx->mpi->control(mpp_ctx->ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);

                } else {
                    RK_U32 err_info = mpp_frame_get_errinfo(frame) | mpp_frame_get_discard(frame);
                    if (err_info) {
                        frame_err++;
                        mpp_log("decoder_get_frame get err info:%d discard:%d.\n",
                                mpp_frame_get_errinfo(frame), mpp_frame_get_discard(frame));
                    }
                    else {
                        /* FPS calculation */
                        fps_counter++;
                        frame_count++;
                        RK_U64 diff, now = current_ms();
                        if ((diff = now - fps_ms) >= 1000) {
                            fps = fps_counter / (diff / 1000.0);
                            fps_counter = 0;
                            fps_ms = now;
                            mpp_log("decode_get_frame get frame %llu, error %llu, discard %llu, FPS = %3.2f\n", frame_count, frame_err, frame_discards, fps);
                        }

                        /** Got a frame */
                        rkdrm_display(frame);
                    }
                }
                frm_eos = mpp_frame_get_eos(frame);
                mpp_frame_deinit(&frame);
                frame = NULL;
                get_frm = 1;
            }

            /* TBD */
            // if last packet is send but last frame is not found continue
            //if (pkt_eos && pkt_done && !frm_eos) {
            //    msleep(MPP_H264_DECODE_TIMEOUT);
            //    continue;
            //}

            if (frm_eos) {
                mpp_log("found last frame\n");
                break;
            }

            if (!get_frm)
                break;
        } while (1);

        if (pkt_done)
            break;

        /*
         * why sleep here:
         * mpi->decode_put_packet will failed when packet in internal queue is
         * full,waiting the package is consumed .
         */
        msleep(MPP_H264_DECODE_TIMEOUT);

    } while (1);
}

float decoder_fps() {
    return fps;
}
