#include "network.h"

#include <libavutil/error.h>
#include <libavutil/log.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct DemuxContext {
    AVFormatContext *format;
} DemuxContext;

#ifdef DEBUG
static FILE *debug_log_file;

static void debug_log_begin(void)
{
#ifdef __SWITCH__
    mkdir("sdmc:/switch", 0777);
    debug_log_file = fopen("sdmc:/switch/sdp.log", "w");
    if (!debug_log_file)
        fprintf(stderr, "Could not open sdmc:/switch/sdp.log\n");
#else
    debug_log_file = stderr;
#endif
}

static void debug_log(const char *format, ...)
{
    if (!debug_log_file)
        debug_log_begin();
    if (!debug_log_file)
        return;

    va_list args;
    va_start(args, format);
    vfprintf(debug_log_file, format, args);
    va_end(args);
    fflush(debug_log_file);
}

static void debug_ffmpeg_log(void *avcl, int level,
                                const char *format, va_list args)
{
    if (!debug_log_file)
        debug_log_begin();
    if (!debug_log_file)
        return;

    char line[2048];
    int print_prefix = 1;
    av_log_format_line(avcl, level, format, args,
                       line, sizeof(line), &print_prefix);
    fprintf(debug_log_file, "%s", line);
    fflush(debug_log_file);
}
#else
static inline void debug_log_begin(void) {}
#define debug_log(...) ((void)0)
#endif

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

typedef struct FallbackSdpReader {
    const uint8_t *data;
    size_t size;
    size_t position;
} FallbackSdpReader;

static int fallback_sdp_read(void *opaque, uint8_t *buffer, int buffer_size)
{
    FallbackSdpReader *reader = opaque;
    if (reader->position >= reader->size)
        return AVERROR_EOF;
    size_t remaining = reader->size - reader->position;
    size_t copy_size = remaining < (size_t)buffer_size
                           ? remaining
                           : (size_t)buffer_size;
    memcpy(buffer, reader->data + reader->position, copy_size);
    reader->position += copy_size;
    return (int)copy_size;
}

static int64_t fallback_sdp_seek(void *opaque, int64_t offset, int whence)
{
    FallbackSdpReader *reader = opaque;
    int64_t position;
    if (whence == SEEK_SET)
        position = offset;
    else if (whence == SEEK_CUR)
        position = (int64_t)reader->position + offset;
    else if (whence == SEEK_END)
        position = (int64_t)reader->size + offset;
    else
        return -1;
    if (position < 0)
        return -1;
    reader->position = (size_t)position;
    return position;
}

static int open_rtp_sdp_context(AVFormatContext **format,
                                AVIOContext **sdp_io,
                                void **sdp_reader,
                                const char *name,
                                const char *sdp)
{
    size_t sdp_size = strlen(sdp);
    FallbackSdpReader *reader = calloc(1, sizeof(*reader));
    uint8_t *buffer = av_malloc(sdp_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!reader || !buffer) {
        free(reader);
        av_free(buffer);
        return AVERROR(ENOMEM);
    }
    memcpy(buffer, sdp, sdp_size);
    memset(buffer + sdp_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    reader->data = buffer;
    reader->size = sdp_size;
    reader->position = 0;

    AVIOContext *io = avio_alloc_context(
        buffer,
        (int)sdp_size,
        0,
        reader,
        fallback_sdp_read,
        NULL,
        fallback_sdp_seek
    );
    if (!io) {
        free(reader);
        av_free(buffer);
        return AVERROR(ENOMEM);
    }

    AVFormatContext *context = avformat_alloc_context();
    if (!context) {
        avio_closep(&io);
        free(reader);
        return AVERROR(ENOMEM);
    }
    context->pb = io;
    context->iformat = av_find_input_format("sdp");

    int error = avformat_open_input(&context, name,
                                    context->iformat, NULL);
    if (error < 0) {
        avformat_close_input(&context);
        free(reader);
        return error;
    }

    *format = context;
    *sdp_io = io;
    *sdp_reader = reader;
    return 0;
}

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

static int network_parse_media(Network *network, const char *sdp_path)
{
    char *data = NULL;
    size_t size = 0;
    char receive_address[128] = "0.0.0.0";
    FILE *file = fopen(sdp_path, "rb");
    if (file) {
        fseek(file, 0, SEEK_END);
        long file_size = ftell(file);
        rewind(file);
        if (file_size > 0) {
            data = malloc((size_t)file_size + 1);
            if (data) {
                size = fread(data, 1, (size_t)file_size, file);
                data[size] = '\0';
            }
        }
        fclose(file);
    } else if (is_default_sdp_path(sdp_path)) {
        size = sizeof(fallback_sdp) - 1;
        data = malloc(size + 1);
        if (data) {
            memcpy(data, fallback_sdp, size);
            data[size] = '\0';
        }
    }
    if (!data)
        return -1;

    network->format = avformat_alloc_context();
    if (!network->format) {
        free(data);
        return -1;
    }
    network->video_index = -1;
    network->audio_index = -1;

    char *line = data;
    while (*line) {
        char *end = strchr(line, '\n');
        if (end)
            *end = '\0';

        if (strncmp(line, "c=IN IP4 ", 9) == 0) {
            char *address = line + 9;
            char *address_end = address;
            while (*address_end && *address_end != '\r' &&
                   *address_end != ' ' && *address_end != '/')
                address_end++;
            size_t address_size =
                (size_t)(address_end - address);
            if (address_size > 0 &&
                address_size < sizeof(receive_address)) {
                memcpy(receive_address, address, address_size);
                receive_address[address_size] = '\0';
                strncpy(network->address, receive_address,
                        sizeof(network->address) - 1);
                network->address[sizeof(network->address) - 1] = '\0';
            }
        } else if (strncmp(line, "m=video ", 8) == 0) {
            int port = 0;
            if (sscanf(line + 8, "%d", &port) == 1) {
                AVStream *stream = avformat_new_stream(network->format, NULL);
                if (stream) {
                    stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
                    stream->codecpar->codec_id = AV_CODEC_ID_H264;
                    network->video_index = (int)stream->index;
                }
                snprintf(network->video_address,
                         sizeof(network->video_address),
                         "rtp://%s:%d", receive_address, port);
                snprintf(network->video_sdp,
                         sizeof(network->video_sdp),
                         "v=0\r\n"
                         "o=- 0 0 IN IP4 %s\r\n"
                         "s=RTP Video\r\n"
                         "c=IN IP4 %s\r\n"
                         "t=0 0\r\n"
                         "m=video %d RTP/AVP 96\r\n"
                         "a=rtpmap:96 H264/90000\r\n"
                         "a=fmtp:96 packetization-mode=1\r\n"
                         "a=recvonly\r\n",
                         receive_address, receive_address, port);
            }
        } else if (strncmp(line, "m=audio ", 8) == 0) {
            int port = 0;
            if (sscanf(line + 8, "%d", &port) == 1) {
                AVStream *stream = avformat_new_stream(network->format, NULL);
                if (stream) {
                    stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
                    stream->codecpar->codec_id = AV_CODEC_ID_OPUS;
                    network->audio_index = (int)stream->index;
                }
                snprintf(network->audio_address,
                         sizeof(network->audio_address),
                         "rtp://%s:%d", receive_address, port);
                snprintf(network->audio_sdp,
                         sizeof(network->audio_sdp),
                         "v=0\r\n"
                         "o=- 0 0 IN IP4 %s\r\n"
                         "s=RTP Audio\r\n"
                         "c=IN IP4 %s\r\n"
                         "t=0 0\r\n"
                         "m=audio %d RTP/AVP 97\r\n"
                         "a=rtpmap:97 opus/48000/2\r\n"
                         "a=fmtp:97 useinbandfec=1\r\n"
                         "a=recvonly\r\n",
                         receive_address, receive_address, port);
            }
        }

        if (!end)
            break;
        line = end + 1;
    }
    free(data);
    return network->video_index >= 0 || network->audio_index >= 0 ? 0 : -1;
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
    avformat_network_init();
    if (network->video_address[0]) {
        int video_error = open_rtp_sdp_context(
            &network->video_format,
            &network->video_sdp_io,
            &network->video_sdp_reader,
            "memory://video.sdp",
            network->video_sdp
        );
#ifdef DEBUG
        if (video_error < 0) {
            char av_error_text[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(video_error, av_error_text,
                        sizeof(av_error_text));
            debug_log("video RTP open failed\n");
            debug_log("video receive URL: %s\n",
                        network->video_address);
            debug_log("video RTP AV error: %d (0x%x): %s\n",
                        video_error, (unsigned)video_error,
                        av_error_text);
        } else {
            debug_log("video RTP open succeeded\n");
        }
#endif
    }
    if (network->audio_address[0]) {
        int audio_error = open_rtp_sdp_context(
            &network->audio_format,
            &network->audio_sdp_io,
            &network->audio_sdp_reader,
            "memory://audio.sdp",
            network->audio_sdp
        );
#ifdef DEBUG
        if (audio_error < 0) {
            char av_error_text[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(audio_error, av_error_text,
                        sizeof(av_error_text));
            debug_log("audio RTP open failed\n");
            debug_log("audio receive URL: %s\n",
                        network->audio_address);
            debug_log("audio RTP AV error: %d (0x%x): %s\n",
                        audio_error, (unsigned)audio_error,
                        av_error_text);
        } else {
            debug_log("audio RTP open succeeded\n");
        }
#endif
    }
    AVPacket *packet = av_packet_alloc();
    Uint32 packet_summary_ms = SDL_GetTicks();
    while (!SDL_AtomicGet(&network->stop)) {
        av_packet_unref(packet);
        int read_error = AVERROR(EAGAIN);
        int packet_stream_index = -1;
        if (network->video_format) {
            read_error = av_read_frame(network->video_format, packet);
            if (read_error >= 0) {
                packet_stream_index = network->video_index;
                network->video_packets_received++;
                network->video_bytes_received += packet->size;
            }
        }
        if (read_error < 0 && network->audio_format) {
            av_packet_unref(packet);
            read_error = av_read_frame(network->audio_format, packet);
            if (read_error >= 0) {
                packet_stream_index = network->audio_index;
                network->audio_packets_received++;
                network->audio_bytes_received += packet->size;
            }
        }
        if (read_error < 0) {
#ifdef DEBUG
            Uint32 now = SDL_GetTicks();
            if (now - network->last_read_error_ms >= 1000) {
                char av_error_text[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(read_error, av_error_text,
                            sizeof(av_error_text));
                debug_log("demux av_read_frame failed\n");
                debug_log("AV error: %d (0x%x): %s\n",
                            read_error, (unsigned)read_error,
                            av_error_text);
                debug_log("errno: %d (%s)\n", errno, strerror(errno));
                network->last_read_error_ms = now;
            }
#endif
            av_packet_unref(packet);
            SDL_Delay(25);
        } else {
            packet->stream_index = packet_stream_index;
            packet_queue_push(&network->queue, packet);
        }
#ifdef DEBUG
        Uint32 now = SDL_GetTicks();
        if (now - packet_summary_ms >= 5000) {
            debug_log("RTP packet summary\n");
            debug_log("video packets: %llu, video bytes: %llu\n",
                        (unsigned long long)network->video_packets_received,
                        (unsigned long long)network->video_bytes_received);
            debug_log("audio packets: %llu, audio bytes: %llu\n",
                        (unsigned long long)network->audio_packets_received,
                        (unsigned long long)network->audio_bytes_received);
            packet_summary_ms = now;
        }
#endif
    }
    av_packet_free(&packet);
    return 0;
}

int network_open(Network *network, const char *sdp_path)
{
    memset(network, 0, sizeof(*network));
    network_parse_address(network, sdp_path);
#ifdef DEBUG
    debug_log_begin();
    debug_log("SDP network open attempt\n");
    debug_log("requested SDP path: %s\n", sdp_path);
#ifdef DEBUG
    av_log_set_level(AV_LOG_DEBUG);
    av_log_set_callback(debug_ffmpeg_log);
#endif
#endif
    avformat_network_init();
    network->queue.mutex = SDL_CreateMutex();
    if (network_parse_media(network, sdp_path) == 0) {
#ifdef DEBUG
        debug_log("parsed SDP media locally\n");
        debug_log("video receive URL: %s\n",
                    network->video_address);
        debug_log("audio receive URL: %s\n",
                    network->audio_address);
#endif
        return 0;
    }

    const char *open_sdp_path = sdp_path;
    FallbackSdpReader *memory_reader = NULL;
    AVIOContext *memory_io = NULL;
    AVFormatContext *memory_format = NULL;
    const uint8_t *memory_sdp = NULL;
    size_t memory_sdp_size = 0;

    FILE *source = fopen(sdp_path, "rb");
    if (source) {
        if (fseek(source, 0, SEEK_END) == 0) {
            long size = ftell(source);
            if (size > 0) {
                rewind(source);
                memory_sdp = av_malloc((size_t)size);
                if (memory_sdp &&
                    fread((void *)memory_sdp, 1, (size_t)size, source) ==
                        (size_t)size) {
                    memory_sdp_size = (size_t)size;
                } else if (memory_sdp) {
                    av_free((void *)memory_sdp);
                    memory_sdp = NULL;
                }
            }
        }
        fclose(source);
#ifdef DEBUG
        debug_log("opened default/explicit SDP file: %s\n", sdp_path);
#endif
    } else if (is_default_sdp_path(sdp_path)) {
        memory_sdp = (const uint8_t *)fallback_sdp;
        memory_sdp_size = sizeof(fallback_sdp) - 1;
        strncpy(network->address, "0.0.0.0",
                sizeof(network->address) - 1);
        network->address[sizeof(network->address) - 1] = '\0';
#ifdef DEBUG
        debug_log("default SDP file missing; using built-in fallback SDP\n");
#endif
    }

    if (memory_sdp && memory_sdp_size > 0) {
        open_sdp_path = "memory://stream.sdp";
        memory_reader = calloc(1, sizeof(*memory_reader));
        uint8_t *memory_buffer =
            av_malloc(memory_sdp_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (memory_reader && memory_buffer) {
            memset(memory_buffer + memory_sdp_size, 0,
                   AV_INPUT_BUFFER_PADDING_SIZE);
            memcpy(memory_buffer, memory_sdp, memory_sdp_size);
            memory_reader->data = memory_buffer;
            memory_reader->size = memory_sdp_size;
            memory_reader->position = 0;
            memory_io = avio_alloc_context(
                memory_buffer,
                (int)memory_sdp_size,
                0,
                memory_reader,
                fallback_sdp_read,
                NULL,
                fallback_sdp_seek
            );
            if (memory_io) {
                memory_format = avformat_alloc_context();
                if (memory_format) {
                    memory_format->pb = memory_io;
                    memory_format->iformat = av_find_input_format("sdp");
                    network->sdp_io = memory_io;
                    network->sdp_reader = memory_reader;
                }
            }
        }
        if (memory_sdp != (const uint8_t *)fallback_sdp)
            av_free((void *)memory_sdp);
    }

    AVDictionary *options = NULL;
    av_dict_set(&options, "protocol_whitelist", "file,udp,rtp,rtcp", 0);
    av_dict_set(&options, "buffer_size", "4194304", 0);
    av_dict_set(&options, "reorder_queue_size", "0", 0);
    av_dict_set(&options, "timeout", "100000", 0);
    av_dict_set(&options, "rw_timeout", "100000", 0);
    av_dict_set(&options, "max_delay", "50000", 0);
    av_dict_set(&options, "analyzeduration", "0", 0);
    av_dict_set(&options, "probesize", "32", 0);
    av_dict_set(&options, "fpsprobesize", "0", 0);

    int error;
    if (memory_format) {
        network->format = memory_format;
        error = avformat_open_input(&network->format, open_sdp_path,
                                    memory_format->iformat, &options);
    } else {
        error = avformat_open_input(&network->format, open_sdp_path,
                                    NULL, &options);
    }
    av_dict_free(&options);
    if (error < 0) {
#ifdef DEBUG
        debug_log("initial avformat_open_input stream count: %u\n",
                    (unsigned)(network->format
                                    ? network->format->nb_streams
                                    : 0));
#endif
        if (memory_format &&
            memory_format->nb_streams >= 1) {
            network->format = memory_format;
#ifdef DEBUG
            debug_log("avformat_open_input returned invalid data but SDP streams were parsed; continuing\n");
#endif
            error = 0;
        }
    }
    if (error < 0) {
        av_error("Could not open SDP", error);
#ifdef DEBUG
        char av_error_text[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(error, av_error_text, sizeof(av_error_text));
        debug_log("avformat_open_input failed\n");
        debug_log("requested path: %s\n", sdp_path);
        debug_log("effective path: %s\n", open_sdp_path);
        debug_log("AV error: %d (0x%x): %s\n",
                    error, (unsigned)error, av_error_text);
        debug_log("errno: %d (%s)\n", errno, strerror(errno));
        debug_log("parsed connection address: %s\n", network_address(network));
        debug_log("protocol whitelist: file,udp,rtp,rtcp\n");
#endif
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
#ifdef DEBUG
    debug_log("avformat_open_input succeeded\n");
    debug_log("effective SDP path: %s\n", open_sdp_path);
    debug_log("format name: %s\n",
                network->format->iformat
                    ? network->format->iformat->name
                    : "unknown");
    debug_log("format URL: %s\n",
                network->format->url ? network->format->url : "none");
    debug_log("stream count: %u\n",
                (unsigned)network->format->nb_streams);
    debug_log("video stream index: %d\n", network->video_index);
    debug_log("audio stream index: %d\n", network->audio_index);
    debug_log("parsed connection address: %s\n",
                network_address(network));
#endif
    return 0;
}

int network_start(Network *network)
{
#ifdef DEBUG
    debug_log_begin();
#endif
    network->thread =
        SDL_CreateThread(demux_thread, "RTP demux", network);
#ifdef DEBUG
    if (!network->thread) {
        debug_log("SDL_CreateThread(demux_thread) failed: %s\n",
                    SDL_GetError());
    } else {
        debug_log("RTP demux thread started\n");
    }
#endif
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
#ifdef DEBUG
    debug_log("stopping RTP demux thread\n");
#endif
    SDL_AtomicSet(&network->stop, 1);
    if (network->thread)
        SDL_WaitThread(network->thread, NULL);
}

void network_close(Network *network)
{
#ifdef DEBUG
    debug_log("closing RTP network and SDP context\n");
#endif
    packet_queue_destroy(&network->queue);
    avformat_close_input(&network->format);
    avformat_close_input(&network->video_format);
    avformat_close_input(&network->audio_format);
    free(network->video_sdp_reader);
    free(network->audio_sdp_reader);
    network->video_sdp_reader = NULL;
    network->audio_sdp_reader = NULL;
    free(network->sdp_reader);
    network->sdp_reader = NULL;
    network->sdp_io = NULL;
    avformat_network_deinit();
#ifdef DEBUG
    av_log_set_callback(av_log_default_callback);
    if (debug_log_file && debug_log_file != stderr) {
        debug_log("network closed\n");
        fclose(debug_log_file);
        debug_log_file = NULL;
    }
#endif
}
