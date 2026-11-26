#include "network.h"

#include <libavutil/error.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct DemuxContext {
    AVFormatContext *format;
} DemuxContext;

static void av_error(const char *operation, int error)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(error, buffer, sizeof(buffer));
    fprintf(stderr, "%s: %s\n", operation, buffer);
}

static void packet_queue_destroy(PacketQueue *queue)
{
    if (queue->mutex)
        SDL_LockMutex(queue->mutex);
    while (queue->head) {
        PacketQueueEntry *entry = queue->head;
        queue->head = entry->next;
        av_packet_free(&entry->packet);
        free(entry);
    }
    queue->tail = NULL;
    queue->count = 0;
    if (queue->mutex) {
        SDL_UnlockMutex(queue->mutex);
        SDL_DestroyMutex(queue->mutex);
        queue->mutex = NULL;
    }
}

static void packet_queue_push(PacketQueue *queue, AVPacket *packet)
{
    PacketQueueEntry *entry = malloc(sizeof(*entry));
    if (!entry)
        return;
    entry->packet = av_packet_alloc();
    av_packet_move_ref(entry->packet, packet);
    entry->next = NULL;

    SDL_LockMutex(queue->mutex);
    if (queue->tail)
        queue->tail->next = entry;
    else
        queue->head = entry;
    queue->tail = entry;
    queue->count++;
    while (queue->count > 64 && queue->head) {
        PacketQueueEntry *overflow = queue->head;
        queue->head = overflow->next;
        if (!queue->head)
            queue->tail = NULL;
        queue->count--;
        queue->dropped_packets++;
        av_packet_free(&overflow->packet);
        free(overflow);
    }
    SDL_UnlockMutex(queue->mutex);
}

static int demux_interrupt_callback(void *opaque)
{
    Network *network = opaque;
    return SDL_AtomicGet(&network->stop);
}

static int demux_thread(void *opaque)
{
    Network *network = opaque;
    AVPacket *packet = av_packet_alloc();
    while (!SDL_AtomicGet(&network->stop)) {
        av_packet_unref(packet);
        if (av_read_frame(network->format, packet) < 0) {
            av_packet_unref(packet);
            SDL_Delay(25);
        } else {
            packet_queue_push(&network->queue, packet);
        }
    }
    av_packet_free(&packet);
    return 0;
}

int network_open(Network *network, const char *sdp_path)
{
    memset(network, 0, sizeof(*network));
    avformat_network_init();
    network->queue.mutex = SDL_CreateMutex();

    AVDictionary *options = NULL;
    av_dict_set(&options, "protocol_whitelist", "file,udp,rtp,rtcp", 0);
    av_dict_set(&options, "buffer_size", "4194304", 0);
    av_dict_set(&options, "reorder_queue_size", "0", 0);
    av_dict_set(&options, "timeout", "100000", 0);
    av_dict_set(&options, "rw_timeout", "100000", 0);
    av_dict_set(&options, "max_delay", "50000", 0);

    int error = avformat_open_input(&network->format, sdp_path, NULL, &options);
    av_dict_free(&options);
    if (error < 0) {
        av_error("Could not open SDP", error);
        packet_queue_destroy(&network->queue);
        avformat_network_deinit();
        return -1;
    }

    network->format->flags |= AVFMT_FLAG_NOBUFFER | AVFMT_FLAG_FLUSH_PACKETS;
    network->format->interrupt_callback.callback = demux_interrupt_callback;
    network->format->interrupt_callback.opaque = network;

    av_dump_format(network->format, 0, sdp_path, 0);
    network->video_index =
        av_find_best_stream(network->format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    network->audio_index =
        av_find_best_stream(network->format, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    network->has_video = network->video_index >= 0;
    network->has_audio = network->audio_index >= 0;
    return 0;
}

int network_start(Network *network)
{
    network->thread =
        SDL_CreateThread(demux_thread, "RTP demux", network);
    return network->thread ? 0 : -1;
}

size_t network_queue_depth(Network *network)
{
    size_t count = 0;
    SDL_LockMutex(network->queue.mutex);
    count = network->queue.count;
    SDL_UnlockMutex(network->queue.mutex);
    return count;
}

Uint64 network_dropped_packets(Network *network)
{
    Uint64 dropped;
    SDL_LockMutex(network->queue.mutex);
    dropped = network->queue.dropped_packets;
    SDL_UnlockMutex(network->queue.mutex);
    return dropped;
}

void network_disable_audio(Network *network)
{
    network->audio_index = -1;
    network->has_audio = 0;
}

void network_disable_video(Network *network)
{
    network->video_index = -1;
    network->has_video = 0;
}

int network_pop_packet(Network *network, AVPacket *packet)
{
    PacketQueue *queue = &network->queue;
    SDL_LockMutex(queue->mutex);
    PacketQueueEntry *entry = queue->head;
    if (!entry) {
        SDL_UnlockMutex(queue->mutex);
        return 0;
    }
    queue->head = entry->next;
    if (!queue->head)
        queue->tail = NULL;
    if (queue->count > 0)
        queue->count--;
    SDL_UnlockMutex(queue->mutex);

    av_packet_unref(packet);
    av_packet_move_ref(packet, entry->packet);
    av_packet_free(&entry->packet);
    free(entry);
    return 1;
}

void network_stop(Network *network)
{
    SDL_AtomicSet(&network->stop, 1);
    if (network->thread)
        SDL_WaitThread(network->thread, NULL);
    network->thread = NULL;
}

void network_close(Network *network)
{
    packet_queue_destroy(&network->queue);
    avformat_close_input(&network->format);
    avformat_network_deinit();
}
