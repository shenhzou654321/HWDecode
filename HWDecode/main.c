#include <stdio.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/avassert.h>
#include <libavutil/imgutils.h>
#include "libswscale/swscale.h"

#include "SDL.h"

static AVBufferRef *hw_device_ctx = NULL;
static enum AVPixelFormat hw_pix_fmt;
static FILE *output_file = NULL;

unsigned char* out_buffer;
AVFrame* pFrame, *pFrameYUV;

SDL_Window *screen;
SDL_Renderer* sdlRenderer;
SDL_Texture* sdlTexture;
SDL_Rect sdlRect;
struct SwsContext* img_convert_ctx;

static int hw_decoder_init(AVCodecContext *ctx, const enum AVHWDeviceType type)
{
	int err = 0;

	if ((err = av_hwdevice_ctx_create(&hw_device_ctx, type,
		NULL, NULL, 0)) < 0) {
		fprintf(stderr, "Failed to create specified HW device.\n");
		return err;
	}
	ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

	return err;
}

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
	const enum AVPixelFormat *pix_fmts)
{
	const enum AVPixelFormat *p;

	for (p = pix_fmts; *p != -1; p++) {
		if (*p == hw_pix_fmt)
			return *p;
	}

	fprintf(stderr, "Failed to get HW surface format.\n");
	return AV_PIX_FMT_NONE;
}

static int decode_write(AVCodecContext *avctx, AVPacket *packet)
{
	AVFrame *frame = NULL, *sw_frame = NULL;
	AVFrame *tmp_frame = NULL;
	uint8_t *buffer = NULL;
	int size;
	int ret = 0;

	ret = avcodec_send_packet(avctx, packet);
	if (ret < 0) {
		fprintf(stderr, "Error during decoding\n");
		return ret;
	}

	while (1) {
		if (!(frame = av_frame_alloc()) || !(sw_frame = av_frame_alloc())) {
			fprintf(stderr, "Can not alloc frame\n");
			ret = AVERROR(ENOMEM);
			goto fail;
		}

		ret = avcodec_receive_frame(avctx, frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			av_frame_free(&frame);
			av_frame_free(&sw_frame);
			return 0;
		}
		else if (ret < 0) {
			fprintf(stderr, "Error while decoding\n");
			goto fail;
		}

		if (frame->format == hw_pix_fmt) 
		{
			/* retrieve data from GPU to CPU */
			if ((ret = av_hwframe_transfer_data(sw_frame, frame, 0)) < 0) {
				fprintf(stderr, "Error transferring the data to system memory\n");
				goto fail;
			}
			tmp_frame = sw_frame;
		}
		else
		{
			tmp_frame = frame;
		}
			
		size = av_image_get_buffer_size(tmp_frame->format, tmp_frame->width,
			tmp_frame->height, 1);


		//buffer = av_malloc(size);
		//if (!buffer) {
		//	fprintf(stderr, "Can not alloc buffer\n");
		//	ret = AVERROR(ENOMEM);
		//	goto fail;
		//}
		//ret = av_image_copy_to_buffer(buffer, size,
		//	(const uint8_t * const *)tmp_frame->data,
		//	(const int *)tmp_frame->linesize, tmp_frame->format,
		//	tmp_frame->width, tmp_frame->height, 1);
		//if (ret < 0) {
		//	fprintf(stderr, "Can not copy image to buffer\n");
		//	goto fail;
		//}

		//if ((ret = fwrite(buffer, 1, size, output_file)) < 0) {
		//	fprintf(stderr, "Failed to dump raw data.\n");
		//	goto fail;
		//}


		sws_scale(img_convert_ctx, (const unsigned char* const*)tmp_frame->data, tmp_frame->linesize, 0, tmp_frame->height,
			pFrameYUV->data, pFrameYUV->linesize);

		SDL_UpdateYUVTexture(sdlTexture, &sdlRect,
			pFrameYUV->data[0], pFrameYUV->linesize[0],
			pFrameYUV->data[1], pFrameYUV->linesize[1],
			pFrameYUV->data[2], pFrameYUV->linesize[2]);
		//SDL_UpdateTexture(sdlTexture, &sdlRect, buffer, size);
		SDL_RenderClear(sdlRenderer);
		SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, &sdlRect);
		SDL_RenderPresent(sdlRenderer);
		//SDL End-----------------------
		//Delay 40ms
		SDL_Delay(40);

	fail:
		av_frame_free(&frame);
		av_frame_free(&sw_frame);
		av_freep(&buffer);
		if (ret < 0)
			return ret;
	}
}

int main(int argc, char *argv[])
{
	AVFormatContext *input_ctx = NULL;
	AVDictionaryEntry *tag = NULL;
	int video_stream, audio_stream, ret;
	AVStream *video = NULL;
	AVStream* audio = NULL;

	AVCodecContext *decoder_ctx = NULL;
	AVCodec *decoder = NULL;
	
	AVCodecContext *audio_decoder_ctx = NULL;
	AVCodec* audioDecoder = NULL;

	AVPacket packet;
	enum AVHWDeviceType type;
	int i;


	int screen_w = 0;
	int screen_h = 0;


	if (argc < 4) {
		//fprintf(stderr, "Usage: %s <device type> <input file> <output file>\n", argv[0]);
		//return -1;
	}

	char* name = av_hwdevice_get_type_name(AV_HWDEVICE_TYPE_CUDA);
	type = av_hwdevice_find_type_by_name(name);
	if (type == AV_HWDEVICE_TYPE_NONE) {
		fprintf(stderr, "Device type %s is not supported.\n", argv[1]);
		fprintf(stderr, "Available device types:");
		while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE)
			fprintf(stderr, " %s", av_hwdevice_get_type_name(type));
		fprintf(stderr, "\n");
		return -1;
	}

	/* open the input file */
	if (avformat_open_input(&input_ctx, "h:/test2.mp4", NULL, NULL) != 0) {
		fprintf(stderr, "Cannot open input file '%s'\n", argv[2]);
		return -1;
	}

	while ((tag = av_dict_get(input_ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
		printf("%s=%s\n", tag->key, tag->value);


	if (avformat_find_stream_info(input_ctx, NULL) < 0) {
		fprintf(stderr, "Cannot find input stream information.\n");
		return -1;
	}

	/* find the video stream information */
	ret = av_find_best_stream(input_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
	if (ret < 0) {
		fprintf(stderr, "Cannot find a video stream in the input file\n");
		return -1;
	}
	video_stream = ret;

	for (i = 0;; i++) {
		const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, i);
		if (!config) {
			fprintf(stderr, "Decoder %s does not support device type %s.\n",
				decoder->name, av_hwdevice_get_type_name(type));
			return -1;
		}
		if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX && config->device_type == type) {
			hw_pix_fmt = config->pix_fmt;
			break;
		}
	}

	/* find the audio stream information */
	//ret = av_find_best_stream(input_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &audioDecoder, 0);
	//if (ret < 0)
	//{
	//	fprintf(stderr, "Cannot find a audio stream in the input file\n");
	//	return -1;
	//}
	//audio_stream = ret;

	//for (i = 0;; i++) {
	//	const AVCodecHWConfig *config = avcodec_get_hw_config(audioDecoder, i);
	//	if (!config) {
	//		fprintf(stderr, "Decoder %s does not support device type %s.\n", audioDecoder->name, av_hwdevice_get_type_name(type));
	//		return -1;
	//	}
	//	if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX && config->device_type == type) {
	//		hw_pix_fmt = config->pix_fmt;
	//		break;
	//	}
	//}


	/* initialize video decoder context */
	if (!(decoder_ctx = avcodec_alloc_context3(decoder)))
		return AVERROR(ENOMEM);

	video = input_ctx->streams[video_stream];
	if (avcodec_parameters_to_context(decoder_ctx, video->codecpar) < 0)
		return -1;

	decoder_ctx->get_format = get_hw_format;
	av_opt_set_int(decoder_ctx, "refcounted_frames", 1, 0);

	if (hw_decoder_init(decoder_ctx, type) < 0)
		return -1;

	if ((ret = avcodec_open2(decoder_ctx, decoder, NULL)) < 0) {
		fprintf(stderr, "Failed to open codec for stream #%u\n", video_stream);
		return -1;
	}

	double duration = input_ctx->duration;

	/* open the file to dump raw data */
	output_file = fopen("output", "w+");


	pFrame = av_frame_alloc();
	pFrameYUV = av_frame_alloc();


	out_buffer = (unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P, decoder_ctx->width, decoder_ctx->height, 1));
	av_image_fill_arrays(pFrameYUV->data, pFrameYUV->linesize, out_buffer, AV_PIX_FMT_YUV420P, decoder_ctx->width, decoder_ctx->height, 1);

	img_convert_ctx = sws_getContext(decoder_ctx->width, decoder_ctx->height, AV_PIX_FMT_NV12,
		decoder_ctx->width, decoder_ctx->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);


	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
	{
		printf("Could not initialize SDL - %s\n", SDL_GetError());
		return -1;
	}

	screen_w = decoder_ctx->width;
	screen_h = decoder_ctx->height;
	//SDL 2.0 Support for multiple windows
	screen = SDL_CreateWindow("Simplest ffmpeg player's Window", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		screen_w, screen_h, SDL_WINDOW_OPENGL);

	if (!screen) 
	{
		printf("SDL: could not create window - exiting:%s\n", SDL_GetError());
		return -1;
	}

	sdlRenderer = SDL_CreateRenderer(screen, -1, 0);

	sdlTexture = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, decoder_ctx->width, decoder_ctx->height);

	sdlRect.x = 0;
	sdlRect.y = 0;
	sdlRect.w = screen_w;
	sdlRect.h = screen_h;



	/* actual decoding and dump the raw data */
	while (ret >= 0) {
		if ((ret = av_read_frame(input_ctx, &packet)) < 0)
			break;

		if (video_stream == packet.stream_index)
			ret = decode_write(decoder_ctx, &packet);

		av_packet_unref(&packet);
	}

	/* flush the decoder */
	packet.data = NULL;
	packet.size = 0;
	ret = decode_write(decoder_ctx, &packet);
	av_packet_unref(&packet);

	if (output_file)
		fclose(output_file);
	avcodec_free_context(&decoder_ctx);
	avformat_close_input(&input_ctx);
	av_buffer_unref(&hw_device_ctx);

	return 0;
}