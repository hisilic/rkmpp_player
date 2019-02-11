#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "tools.h"
#include "mppdecoder.h"
#include "rkdrm.h"

AVPacket avpkt;
AVFormatContext *ic;

static void rkmpp_ctx_init(RKMPPCodecContext *mpp_ctx)
{
	mpp_ctx->codectype = MPP_VIDEO_CodingAVC;
}

static int rtsp_open(const char *rtspUrl, RKMPPCodecContext *mpp_ctx)
{
	int err = 0;
	AVDictionary *dict = NULL;
	
	mpp_ctx->ic = avformat_alloc_context();
	av_dict_set(&dict, "rtsp_transport", "tcp", 0);
	err = avformat_open_input(&mpp_ctx->ic, rtspUrl, NULL, &dict);

	return err;
}

#if 0
static void *thread_rtsp_mpp(void *arg)
{
	RKMPPCodecContext *mpp_ctx = (RKMPPCodecContext *)arg;
	AVPacket avPacket;
	int ret = 0;
	
	if(!mpp_ctx->ic)	
		return NULL;

	while (1)
	{
		ret = av_read_frame(mpp_ctx->ic, &avPacket);
		if (ret == AVERROR(EAGAIN)) 
			continue;

		mpp_ctx->avpkt = &avPacket;
		decode_one_pkt(mpp_ctx);
	}

	return NULL;
}
#else
static void rkmpp_play(RKMPPCodecContext *mpp_ctx)
{
	int ret = 0;
	AVPacket avPacket;
	
	if(!mpp_ctx->ic)	
		return;

	while (1)
	{
		ret = av_read_frame(mpp_ctx->ic, &avPacket);
		if (ret == AVERROR(EAGAIN)) 
			continue;

		mpp_ctx->avpkt = &avPacket;
		decode_one_pkt(mpp_ctx);
	}

	return;
}

#endif

int main(int argc, char *argv[])
{
	char url[256] = {0};
	RKMPPCodecContext mpp_ctx;
	//pthread_t thread;

	if (2 != argc)
	{
		printf("Usage: ./rkmpp_player [RTSP_URL]\n");
		printf("       ./rkmpp_player [*.h264]\n");
		exit(-1);
	}
	
	rkmpp_ctx_init(&mpp_ctx);
	
	strcpy(url, argv[1]);
	if (rtsp_open((const char*)url, &mpp_ctx)) {
		fprintf(stderr, "RTSP open error. \n");
		exit(-1);
	}

    rkmpp_decoder_init(&mpp_ctx);

    rkdrm_init();

    //pthread_create(&thread, NULL, &thread_rtsp_mpp, &mpp_ctx);
    rkmpp_play(&mpp_ctx);
    //unsigned long long int espc = 0, espd = 0, disp = 0;
    while (1) {
        msleep(1000);
    }

    rkdrm_fini();

    return 0;
}

