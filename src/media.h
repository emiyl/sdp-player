#ifndef MEDIA_H
#define MEDIA_H

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <SDL.h>
#include "gui.h"
#include "profile.h"

typedef struct Media {
    AVCodecContext *video_decoder;
    AVCodecContext *audio_decoder;
    AVFrame *frame;
    struct SwsContext *sws;
    struct SwrContext *swr;
    int sws_width;
    int sws_height;
    enum AVPixelFormat sws_format;
    SDL_AudioDeviceID audio_device;
    SDL_mutex *audio_queue_mutex;
    SDL_atomic_t audio_stop;
    SDL_Thread *audio_thread;
    struct AudioPacketEntry *audio_packets_head;
    struct AudioPacketEntry *audio_packets_tail;
    size_t audio_packet_count;
    uint8_t *h264_parameter_sets;
    size_t h264_parameter_sets_size;
    uint8_t *h264_sps;
    size_t h264_sps_size;
    uint8_t *h264_pps;
    size_t h264_pps_size;
    int h264_ready;
    Profile *profile;
} Media;

int media_init(Media *media, AVFormatContext *format,
               int video_index, int audio_index,
               int process_audio, int process_video);
int media_decode_packet(Media *media, AVPacket *packet, enum AVMediaType type,
                        Gui *gui, Profile *profile);
void media_shutdown(Media *media);

#endif
