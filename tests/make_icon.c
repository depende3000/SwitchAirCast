/*
 * Generates icon.jpg (256x256) for the homebrew menu from the UI's logo:
 *   SDL_VIDEODRIVER=offscreen build/linux/make_icon icon.jpg
 */

#include <stdio.h>
#include <stdlib.h>

#include <SDL.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

#include "platform.h"
#include "ui.h"

#define SIZE 256

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "icon.jpg";
    platform_init();
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window *win = SDL_CreateWindow("icon", 0, 0, SIZE, SIZE, SDL_WINDOW_HIDDEN);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren || !ui_init(ren)) return 1;
    SDL_Texture *target = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, SIZE, SIZE);
    SDL_SetRenderTarget(ren, target);
    ui_draw_app_icon(SIZE);
    static uint8_t rgb[SIZE * SIZE * 4];
    SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_BGRA32, rgb, SIZE * 4);

    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->width = ctx->height = SIZE;
    ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
    ctx->time_base = (AVRational) { 1, 25 };
    ctx->flags |= AV_CODEC_FLAG_QSCALE;
    ctx->global_quality = FF_QP2LAMBDA * 2;
    if (!codec || avcodec_open2(ctx, codec, NULL) < 0) return 1;
    AVFrame *frame = av_frame_alloc();
    frame->format = ctx->pix_fmt;
    frame->width = frame->height = SIZE;
    frame->quality = ctx->global_quality;
    av_frame_get_buffer(frame, 0);
    struct SwsContext *sws = sws_getContext(SIZE, SIZE, AV_PIX_FMT_BGRA, SIZE, SIZE, AV_PIX_FMT_YUVJ420P,
                                            SWS_BICUBIC, NULL, NULL, NULL);
    const uint8_t *src[1] = { rgb };
    int stride[1] = { SIZE * 4 };
    sws_scale(sws, src, stride, 0, SIZE, frame->data, frame->linesize);
    AVPacket *pkt = av_packet_alloc();
    if (avcodec_send_frame(ctx, frame) < 0 || avcodec_receive_packet(ctx, pkt) < 0) return 1;
    FILE *f = fopen(out, "wb");
    fwrite(pkt->data, 1, pkt->size, f);
    fclose(f);
    printf("wrote %s (%d bytes)\n", out, pkt->size);
    return 0;
}
