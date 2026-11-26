#ifndef MEDIA_H
#define MEDIA_H

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <SDL.h>
#include "gui.h"

typedef struct Media {
    AVCodecContext *video_decoder;
    AVCodecContext *audio_decoder;
    AVFrame *frame;
    SwsContext *sws;
    SwrContext *swr;
    int sws_width;
    int sws_height;
    enum AVPixelFormat sws_format;
    SDL_AudioDeviceID audio_device;
} Media;

int media_init(Media *media, AVFormatContext *format,
               int video_index, int audio_index);
int media_decode_packet(Media *media, AVPacket *packet, enum AVMediaType type,
                        Gui *gui);
void media_shutdown(Media *media);

#endif
