#include "network.h"

#include <libavutil/error.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct DemuxContext {
    AVFormatContext *format;
} DemuxContext;

static const char fallback_sdp[] =
    "v=0\r\n"
    "o=- 0 0 IN IP4 127.0.0.1\r\n"
    "s=Cemu Gamepad\r\n"
    "c=IN IP4 239.255.0.1\r\n"
    "t=0\r\n"
    "m=video 5004 RTP/AVP 96\r\n"
    "a=rtpmap:96 H264/90000\r\n"
    "a=fmtp:96 packetization-mode=1\r\n"
    "a=framerate:60\r\n"
    "a=recvonly\r\n"
    "m=audio 5006 RTP/AVP 97\r\n"
    "a=rtpmap:97 opus/48000/2\r\n"
    "a=fmtp:97 useinbandfec=1\r\n"
    "a=recvonly\r\n";

static int is_default_sdp_path(const char *sdp_path)
{
#ifdef __SWITCH__
    return strcmp(sdp_path, "sdmc:/switch/sdp-player/stream.sdp") == 0;
#else
    return strcmp(sdp_path, "stream.sdp") == 0;
#endif
}

static void av_error(const char *operation, int error)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(error, buffer, sizeof(buffer));
    fprintf(stderr, "%s: %s\n", operation, buffer);
}

static void network_parse_address(Network *network, const char *sdp_path)
{
    strncpy(network->address, "Unknown", sizeof(network->address) - 1);
    network->address[sizeof(network->address) - 1] = '\0';

    FILE *file = fopen(sdp_path, "rb");
    if (!file)
        return;

    char line[512];
    while (fgets(line, sizeof(line), file)) {
        char *connection = strstr(line, "c=IN IP4 ");
        if (!connection)
            connection = strstr(line, "c=IN IP6 ");
        if (!connection)
            continue;

        char *address = connection + 9;
        char *end = address;
        while (*end && *end != '\r' && *end != '\n' &&
               *end != ' ' && *end != '/' && *end != '\t')
            end++;
        size_t length = (size_t)(end - address);
        if (length > 0 && length < sizeof(network->address)) {
            memcpy(network->address, address, length);
            network->address[length] = '\0';
        }
        break;
    }
    fclose(file);
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
    network_parse_address(network, sdp_path);
    avformat_network_init();
    network->queue.mutex = SDL_CreateMutex();

    const char *open_sdp_path = sdp_path;
    int remove_fallback = 0;
    FILE *source = fopen(sdp_path, "rb");
    if (!source && is_default_sdp_path(sdp_path)) {
        open_sdp_path = w();
        FILE *fallback = fopen(open_sdp_path, "wb");
        if (fallback) {
            fwrite(fallback_sdp, 1, sizeof(fallback_sdp) - 1, fallback);
            fclose(fallback);
            remove_fallback = 1;
            strncpy(network->address, "239.255.0.1",
                    sizeof(network->address) - 1);
            network->address[sizeof(network->address) - 1] = '\0';
        } else {
            open_sdp_path = sdp_path;
        }
    } else if (source) {
        fclose(source);
    }

    AVDictionary *options = NULL;
    av_dict_set(&options, "protocol_whitelist", "file,udp,rtp,rtcp", 0);
    av_dict_set(&options, "buffer_size", "4194304", 0);
    av_dict_set(&options, "reorder_queue_size", "0", 0);
    av_dict_set(&options, "timeout", "100000", 0);
    av_dict_set(&options, "rw_timeout", "100000", 0);
    av_dict_set(&options, "max_delay", "50000", 0);

    int error = avformat_open_input(&network->format, open_sdp_path,
                                    NULL, &options);
    if (remove_fallback)
        remove(open_sdp_path);
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

const char *network_address(const Network *network)
{
    return network->address[0] ? network->address : "Unknown";
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
}

void network_close(Network *network)
{
    packet_queue_destroy(&network->queue);
    avformat_close_input(&network->format);
    avformat_network_deinit();
}
