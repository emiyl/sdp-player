#ifndef NETWORK_H
#define NETWORK_H

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
} PacketQueue;

typedef struct Network {
    AVFormatContext *format;
    PacketQueue queue;
    SDL_atomic_t stop;
    SDL_Thread *thread;
    int video_index;
    int audio_index;
} Network;

int network_open(Network *network, const char *sdp_path);
int network_start(Network *network);
int network_pop_packet(Network *network, AVPacket *packet);
void network_stop(Network *network);
void network_close(Network *network);

#endif
