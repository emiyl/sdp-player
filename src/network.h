#ifndef NETWORK_H
#define NETWORK_H

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <SDL.h>

typedef struct PacketQueueEntry {
    AVPacket *packet;
    struct PacketQueueEntry *next;
} PacketQueueEntry;

typedef struct PacketQueue {
    SDL_mutex *mutex;
    PacketQueueEntry *head;
    PacketQueueEntry *tail;
    size_t count;
} PacketQueue;

typedef struct Network {
    AVFormatContext *format;
    PacketQueue queue;
    SDL_atomic_t stop;
    SDL_Thread *thread;
    int video_index;
    int audio_index;
    int has_video;
    int has_audio;
} Network;

#ifdef __cplusplus
extern "C" {
#endif
int network_open(Network *network, const char *sdp_path);
int network_start(Network *network);
int network_pop_packet(Network *network, AVPacket *packet);
void network_stop(Network *network);
void network_close(Network *network);
AVPacket *network_packet_alloc(void);
void network_packet_free(AVPacket **packet);
void network_packet_unref(AVPacket *packet);
int network_packet_new(AVPacket *packet, int size);
void network_packet_move_ref(AVPacket *dst, AVPacket *src);
#ifdef __cplusplus
}
#endif

#endif
