#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include "SDL.h"
#include "SDL_thread.h"

#include <android/log.h>
#define LOGI(FORMAT,...) __android_log_print(ANDROID_LOG_INFO,"ywl5320",FORMAT,##__VA_ARGS__);
#define LOGE(FORMAT,...) __android_log_print(ANDROID_LOG_ERROR,"ywl5320",FORMAT,##__VA_ARGS__);

#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"
#include "libavutil/mathematics.h"
#include "libavutil/samplefmt.h"

static const int SDL_AUDIO_BUFFER_SIZE = 1024;
static const int MAX_AUDIO_FRAME_SIZE = 192000;

/*
���а�
*/
typedef struct PackeQueue
{
	AVPacketList *first_pkt, *last_pkt;

	int data_size;
	int nb_pkts;

	SDL_mutex *mutex;
	SDL_cond *cond;

}PackeQueue;

void init_Queue(PackeQueue *queue)
{
	queue = (PackeQueue *)malloc(sizeof(PackeQueue));
	queue->mutex = SDL_CreateMutex();
	queue->cond = SDL_CreateCond();
}

int push_Queue(PackeQueue *queue, AVPacket *pkt)
{
	AVPacketList *pkt1;
	if (av_dup_packet(pkt) < 0) {
		return -1;
	}
	pkt1 = (AVPacketList *)av_malloc(sizeof(AVPacketList));
	if (!pkt1)
		return -1;
	pkt1->pkt = *pkt;
	pkt1->next = NULL;

	SDL_LockMutex(queue->mutex);

	if (!queue->last_pkt)//��һ��Ϊ�գ���������û����
	{
		queue->first_pkt = pkt1;
	}
	else
	{
		queue->last_pkt->next = pkt1;
	}

	queue->last_pkt = pkt1;
	queue->nb_pkts++;
	queue->data_size += pkt1->pkt.size;

	SDL_CondSignal(queue->cond);
	SDL_UnlockMutex(queue->mutex);
	return 0;
}

int pop_Queue(PackeQueue *queue, AVPacket *pkt)
{
	AVPacketList *pkt1;
	SDL_LockMutex(queue->mutex);
	pkt1 = queue->first_pkt;
	int ret = -1;
	for (;;)
	{
		if (pkt1)
		{
			queue->first_pkt = pkt1->next;
			queue->data_size -= pkt1->pkt.size;
			queue->nb_pkts--;
			*pkt = pkt1->pkt;
			av_free(pkt1);
			ret = 0;
			break;
		}
		else
		{
			SDL_CondWait(queue->cond, queue->mutex);
		}
	}
	SDL_UnlockMutex(queue->mutex);
	return ret;

}



int main(int argv, char* argc[])
{
	//1.ע��֧�ֵ��ļ���ʽ����Ӧ��codec
	av_register_all();
	avformat_network_init();

	//char* filenName = "http://live.g3proxy.lecloud.com/gslb?stream_id=lb_yxlm_1800&tag=live&ext=m3u8&sign=live_tv&platid=10&splatid=1009";
	//char *filenName = "jxtg3.mkv";


	// 2.��video�ļ�
	AVFormatContext* pFormatCtx = NULL;
	// ��ȡ�ļ�ͷ������ʽ�����Ϣ�����AVFormatContext�ṹ����
	if (avformat_open_input(&pFormatCtx, argc[1], NULL, NULL) != 0)
		return -1; // ��ʧ��

	// ����ļ�������Ϣ
	if (avformat_find_stream_info(pFormatCtx, NULL) < 0)
		return -1; // û�м�⵽����Ϣ stream infomation

	// �ڿ���̨����ļ���Ϣ
	av_dump_format(pFormatCtx, 0, argc[1], 0);

	//���ҵ�һ����Ƶ�� video stream
	int videoStream = -1;
	int i = 0;
	for (; i < pFormatCtx->nb_streams; i++)
	{
		if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			videoStream = i;
			break;
		}
	}

	if (videoStream == -1)
		return -1; // û�в��ҵ���Ƶ��video stream

	AVCodecContext* pCodecCtxOrg = NULL;
	AVCodecContext* pCodecCtx = NULL;

	AVCodec* pCodec = NULL;

	pCodecCtxOrg = pFormatCtx->streams[videoStream]->codec; // codec context

	// �ҵ�video stream�� decoder
	pCodec = avcodec_find_decoder(pCodecCtxOrg->codec_id);

	if (!pCodec)
	{
//		cout << "Unsupported codec!" << endl;
		return -1;
	}

	// ��ֱ��ʹ�ô�AVFormatContext�õ���CodecContext��Ҫ����һ��
	pCodecCtx = avcodec_alloc_context3(pCodec);
	if (avcodec_copy_context(pCodecCtx, pCodecCtxOrg) != 0)
	{
//		cout << "Could not copy codec context!" << endl;
		return -1;
	}

	// open codec
	if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0)
		return -1; // Could open codec

	AVFrame* pFrame = NULL;
	AVFrame* pFrameYUV = NULL;

	pFrame = av_frame_alloc();
	pFrameYUV = av_frame_alloc();

	// ʹ�õĻ������Ĵ�С
	int numBytes = 0;
	uint8_t* buffer = NULL;

	numBytes = avpicture_get_size(AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height);
	buffer = (uint8_t*)av_malloc(numBytes * sizeof(uint8_t));

	avpicture_fill((AVPicture*)pFrameYUV, buffer, AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height);

	struct SwsContext* sws_ctx = NULL;
	sws_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt,
			pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);

	///////////////////////////////////////////////////////////////////////////
	//
	// SDL2.0
	//
	//////////////////////////////////////////////////////////////////////////
	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER);
	SDL_Window* window = SDL_CreateWindow("FFmpeg Decode", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
			pCodecCtx->width, pCodecCtx->height, SDL_WINDOW_OPENGL);
	SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, 0);
	SDL_Texture* bmp = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING,
		pCodecCtx->width, pCodecCtx->height);
	SDL_Rect rect;
	rect.x = 0;
	rect.y = 0;
	rect.w = pCodecCtx->width;
	rect.h = pCodecCtx->height;

	SDL_Event event;

	AVPacket packet;
	while (av_read_frame(pFormatCtx, &packet) >= 0)
	{
		if (packet.stream_index == videoStream)
		{
			int frameFinished = 0;
			avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished, &packet);
			if (frameFinished)
			{
				printf("pts:%d \n", pFrame->pkt_pts / 1000);




				sws_scale(sws_ctx, (uint8_t const * const *)pFrame->data, pFrame->linesize, 0,
					pCodecCtx->height, pFrameYUV->data, pFrameYUV->linesize);

				//SDL_UpdateTexture(bmp, &rect, pFrameYUV->data[0], pFrameYUV->linesize[0]);
				SDL_UpdateYUVTexture(bmp, &rect,
					pFrameYUV->data[0], pFrameYUV->linesize[0],
					pFrameYUV->data[1], pFrameYUV->linesize[2],
					pFrameYUV->data[2], pFrameYUV->linesize[1]);
				SDL_RenderClear(renderer);
				SDL_RenderCopy(renderer, bmp, &rect, &rect);
				SDL_RenderPresent(renderer);
				SDL_Delay(30);

			}
		}
		av_free_packet(&packet);

		SDL_PollEvent(&event);
		switch (event.type)
		{
		case SDL_QUIT:
			SDL_Quit();
			av_free(buffer);
			av_frame_free(&pFrame);
			av_frame_free(&pFrameYUV);

			avcodec_close(pCodecCtx);
			avcodec_close(pCodecCtxOrg);

			avformat_close_input(&pFormatCtx);
			return 0;
		}
	}


	av_free(buffer);
	av_frame_free(&pFrame);
	av_frame_free(&pFrameYUV);

	avcodec_close(pCodecCtx);
	avcodec_close(pCodecCtxOrg);

	avformat_close_input(&pFormatCtx);

	getchar();
	return 0;
}











