#include "network.h"
#include "logging.h"
#include "logging.h"

#include <libavutil/error.h>
#include <libavutil/base64.h>
#include <libavutil/log.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef __SWITCH__
#include <switch.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#endif
#include <sys/stat.h>
#include <stdio.h>
#include <stdarg.h>
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
    char sprop_parameter_sets[2048] = {0};

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
        } else if (strstr(line, "sprop-parameter-sets=")) {
            const char *sets =
                strstr(line, "sprop-parameter-sets=");
            if (sets) {
                sets += strlen("sprop-parameter-sets=");
                char *comma = strchr(sets, ',');
                if (comma)
                    comma = comma;
                strncpy(sprop_parameter_sets, sets,
                        sizeof(sprop_parameter_sets) - 1);
                sprop_parameter_sets[sizeof(sprop_parameter_sets) - 1] =
                    '\0';
                char *end = sprop_parameter_sets;
                while (*end && *end != '\r' && *end != '\n')
                    end++;
                *end = '\0';
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
    if (network->video_index >= 0 &&
        sprop_parameter_sets[0]) {
        AVStream *video_stream =
            network->format->streams[network->video_index];
        char *sps = sprop_parameter_sets;
        char *pps = strchr(sps, ',');
        size_t sps_b64_size = pps ? (size_t)(pps - sps)
                                  : strlen(sps);
        if (pps) {
            *pps = '\0';
            pps++;
        }
        uint8_t *sps_decoded =
            av_malloc(sps_b64_size);
        uint8_t *pps_decoded =
            pps ? av_malloc(strlen(pps)) : NULL;
        int sps_decoded_size =
            sps_decoded
                ? av_base64_decode(sps_decoded, sps,
                                   (int)sps_b64_size)
                : 0;
        int pps_decoded_size =
            pps_decoded
                ? av_base64_decode(pps_decoded, pps,
                                   (int)strlen(pps))
                : 0;
        size_t sps_size =
            sps_decoded_size > 0
                ? (size_t)sps_decoded_size : 0;
        size_t pps_size =
            pps_decoded_size > 0
                ? (size_t)pps_decoded_size : 0;
        size_t extradata_size =
            sps_size + pps_size + 8;
        uint8_t *extradata =
            av_malloc(extradata_size +
                      AV_INPUT_BUFFER_PADDING_SIZE);
        if (extradata) {
            size_t position = 0;
            extradata[position++] = 0;
            extradata[position++] = 0;
            extradata[position++] = 0;
            extradata[position++] = 1;
            if (sps_decoded_size > 0) {
                memcpy(extradata + position,
                       sps_decoded,
                       (size_t)sps_decoded_size);
                position += (size_t)sps_decoded_size;
            }
            if (pps_decoded_size > 0) {
                extradata[position++] = 0;
                extradata[position++] = 0;
                extradata[position++] = 0;
                extradata[position++] = 1;
                memcpy(extradata + position,
                       pps_decoded,
                       (size_t)pps_decoded_size);
                position += (size_t)pps_decoded_size;
            }
            av_freep(&video_stream->codecpar->extradata);
            video_stream->codecpar->extradata =
                av_malloc(position +
                          AV_INPUT_BUFFER_PADDING_SIZE);
            if (video_stream->codecpar->extradata) {
                memcpy(video_stream->codecpar->extradata,
                       extradata, position);
                video_stream->codecpar->extradata_size =
                    (int)position;
                free(network->h264_parameter_sets);
                network->h264_parameter_sets =
                    malloc(position);
                if (network->h264_parameter_sets) {
                    memcpy(network->h264_parameter_sets,
                           extradata, position);
                    network->h264_parameter_sets_size = position;
                }
            }
            av_free(extradata);
        }
        av_free(sps_decoded);
        av_free(pps_decoded);
#ifdef DEBUG
        debug_log(
            "H264 sprop-parameter-sets decoded: SPS=%zu bytes, PPS=%zu bytes\n",
            sps_size,
            pps_size
        );
#else
        (void)sps_size;
        (void)pps_size;
#endif
    }
#ifdef DEBUG
    if (!sprop_parameter_sets[0]) {
        debug_log(
            "H264 SDP has no sprop-parameter-sets; SPS/PPS must arrive in RTP NAL units\n"
        );
    }
#endif
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

static int rtp_strip_header(AVPacket *packet, int payload_type,
                            uint8_t **payload, size_t *payload_size)
{
    if (packet->size < 12)
        return 0;
    uint8_t first = packet->data[0];
    uint8_t second = packet->data[1];
    if ((first & 0xc0) != 0x80)
        return 0;
    if ((second & 0x7f) != payload_type)
        return 0;

    size_t header_size = 12 + ((first & 0x0f) * 4);
    if (packet->size < (int)header_size)
        return 0;
    if (first & 0x10) {
        if (header_size + 4 > (size_t)packet->size)
            return 0;
        uint16_t extension_words =
            ((uint16_t)packet->data[header_size + 2] << 8) |
            packet->data[header_size + 3];
        size_t extension_size =
            4 + (size_t)extension_words * 4;
        if (header_size + extension_size >
            (size_t)packet->size)
            return 0;
        header_size += extension_size;
    }
    if (first & 0x20) {
        size_t padding = packet->data[packet->size - 1];
        if (padding > packet->size - (int)header_size)
            return 0;
        packet->size -= (int)padding;
    }
    *payload = packet->data + header_size;
    *payload_size = (size_t)(packet->size - (int)header_size);
    return 1;
}

static int append_annexb(Network *network,
                         uint8_t nal_header,
                         const uint8_t *nal_data,
                         size_t nal_data_size)
{
    size_t required =
        network->h264_fragment_size + 4 + 1 + nal_data_size;
    if (required > network->h264_fragment_capacity) {
        size_t capacity =
            required > 131072 ? required : 131072;
        uint8_t *buffer =
            realloc(network->h264_fragment, capacity);
        if (!buffer)
            return 0;
        network->h264_fragment = buffer;
        network->h264_fragment_capacity = capacity;
    }
    uint8_t *out =
        network->h264_fragment + network->h264_fragment_size;
    out[0] = 0;
    out[1] = 0;
    out[2] = 0;
    out[3] = 1;
    out[4] = nal_header;
    if (nal_data_size)
        memcpy(out + 5, nal_data, nal_data_size);
    network->h264_fragment_size = required;
    return 1;
}

static int append_annexb_fragment(Network *network,
                                  const uint8_t *data,
                                  size_t data_size)
{
    size_t required = network->h264_fragment_size + data_size;
    if (required > network->h264_fragment_capacity) {
        size_t capacity =
            required > 131072 ? required : 131072;
        uint8_t *buffer =
            realloc(network->h264_fragment, capacity);
        if (!buffer)
            return 0;
        network->h264_fragment = buffer;
        network->h264_fragment_capacity = capacity;
    }
    memcpy(network->h264_fragment +
               network->h264_fragment_size,
           data, data_size);
    network->h264_fragment_size = required;
    return 1;
}

static int append_h264_parameter_sets(Network *network)
{
    if (!network->h264_parameter_sets ||
        network->h264_parameter_sets_size == 0)
        return 1;
    size_t required =
        network->h264_fragment_size +
        network->h264_parameter_sets_size;
    if (required > network->h264_fragment_capacity) {
        size_t capacity =
            required > 131072 ? required : 131072;
        uint8_t *buffer =
            realloc(network->h264_fragment, capacity);
        if (!buffer)
            return 0;
        network->h264_fragment = buffer;
        network->h264_fragment_capacity = capacity;
    }
    memcpy(network->h264_fragment +
               network->h264_fragment_size,
           network->h264_parameter_sets,
           network->h264_parameter_sets_size);
    network->h264_fragment_size = required;
    return 1;
}

static int depacketize_h264(Network *network, AVPacket *packet)
{
    uint8_t *payload = NULL;
    size_t payload_size = 0;
    if (!rtp_strip_header(packet, 96, &payload, &payload_size)) {
        network->h264_invalid_rtp_packets++;
#ifdef DEBUG
        if (network->h264_invalid_rtp_packets <= 64) {
            uint16_t sequence = packet->size >= 4
                                    ? ((uint16_t)packet->data[2] << 8) |
                                          packet->data[3]
                                    : 0;
            uint32_t ssrc = packet->size >= 12
                                ? ((uint32_t)packet->data[8] << 24) |
                                      ((uint32_t)packet->data[9] << 16) |
                                      ((uint32_t)packet->data[10] << 8) |
                                      packet->data[11]
                                : 0;
            unsigned payload_type = packet->size >= 2
                                        ? packet->data[1] & 0x7f
                                        : 255;
            debug_log(
                "Rejected video RTP datagram: bytes=%d version=%u payload_type=%u sequence=%u ssrc=%08x\n",
                packet->size,
                packet->size >= 1 ? packet->data[0] >> 6 : 0,
                payload_type, sequence, (unsigned)ssrc);
        }
#endif
        return 0;
    }
    if (!payload_size)
        return 0;

    uint32_t rtp_ssrc =
        ((uint32_t)packet->data[8] << 24) |
        ((uint32_t)packet->data[9] << 16) |
        ((uint32_t)packet->data[10] << 8) |
        packet->data[11];
    uint32_t rtp_timestamp =
        ((uint32_t)packet->data[4] << 24) |
        ((uint32_t)packet->data[5] << 16) |
        ((uint32_t)packet->data[6] << 8) |
        packet->data[7];
    if (network->h264_last_timestamp &&
        rtp_timestamp != network->h264_last_timestamp)
        network->h264_timestamp_changes++;
    network->h264_last_timestamp = rtp_timestamp;
    uint16_t rtp_sequence =
        ((uint16_t)packet->data[2] << 8) |
        packet->data[3];
    if (network->h264_have_last_rtp_sequence &&
        rtp_ssrc != network->h264_last_rtp_ssrc) {
        network->h264_have_last_rtp_sequence = 0;
    }
    int sequence_gap =
        network->h264_have_last_rtp_sequence &&
        rtp_sequence !=
            (uint16_t)(network->h264_last_rtp_sequence + 1);
    if (sequence_gap) {
        network->h264_sequence_gaps++;
        network->h264_rejected_packets++;
#ifdef DEBUG
        debug_log(
            "H264 RTP sequence discontinuity: previous=%u expected=%u received=%u timestamp=%u ssrc=%08x\n",
            (unsigned)network->h264_last_rtp_sequence,
            (unsigned)(uint16_t)(network->h264_last_rtp_sequence + 1),
            (unsigned)rtp_sequence, (unsigned)rtp_timestamp,
            (unsigned)rtp_ssrc);
#endif
        if (network->h264_access_unit_active &&
            rtp_timestamp == network->h264_access_unit_timestamp) {
            network->h264_access_unit_damaged = 1;
            network->h264_fragment_active = 0;
        }
    }
    network->h264_last_rtp_sequence = rtp_sequence;
    network->h264_last_rtp_ssrc = rtp_ssrc;
    network->h264_have_last_rtp_sequence = 1;
    int marker = (packet->data[1] & 0x80) != 0;
    if (marker)
        network->h264_marker_packets++;

    if (network->h264_access_unit_active &&
        rtp_timestamp != network->h264_access_unit_timestamp) {
        int lost_idr = network->h264_access_unit_has_idr;
        network->h264_fragment_size = 0;
        network->h264_fragment_active = 0;
        network->h264_access_unit_active = 0;
        network->h264_access_unit_damaged = 0;
        network->h264_access_unit_has_vcl = 0;
        network->h264_access_unit_has_idr = 0;
        if (lost_idr)
            network->h264_seen_idr = 0;
    }
    if (!network->h264_access_unit_active) {
        network->h264_access_unit_active = 1;
        network->h264_access_unit_timestamp = rtp_timestamp;
        network->h264_fragment_first_sequence = rtp_sequence;
        network->h264_access_unit_has_idr = 0;
    }

    uint8_t nal_type = payload[0] & 0x1f;
    if (nal_type >= 1 && nal_type <= 23) {
        if (nal_type == 1 && !network->h264_seen_idr) {
            network->h264_access_unit_damaged = 1;
        } else {
            if (network->h264_fragment_active) {
                network->h264_rejected_packets++;
                network->h264_access_unit_damaged = 1;
            }
            network->h264_fragment_active = 0;
            if (nal_type == 5 &&
                !append_h264_parameter_sets(network))
                network->h264_access_unit_damaged = 1;
            if (!append_annexb(network, payload[0], payload + 1,
                               payload_size - 1))
                network->h264_access_unit_damaged = 1;
            if (nal_type >= 1 && nal_type <= 5)
                network->h264_access_unit_has_vcl = 1;
            if (nal_type == 5) {
                network->h264_seen_idr = 1;
                network->h264_access_unit_has_idr = 1;
                network->h264_complete_idr_units++;
            } else if (nal_type == 1) {
                network->h264_complete_slice_units++;
            }
            if (nal_type == 7)
                network->h264_nal_type7_count++;
            else if (nal_type == 8)
                network->h264_nal_type8_count++;
        }
    } else if (nal_type == 24) {
        if (network->h264_fragment_active) {
            network->h264_rejected_packets++;
            network->h264_access_unit_damaged = 1;
        }
        network->h264_fragment_active = 0;
        size_t offset = 1;
        while (offset < payload_size) {
            if (offset + 2 > payload_size) {
                network->h264_access_unit_damaged = 1;
                break;
            }
            uint16_t nal_size =
                ((uint16_t)payload[offset] << 8) |
                payload[offset + 1];
            offset += 2;
            if (!nal_size || offset + nal_size > payload_size) {
                network->h264_access_unit_damaged = 1;
                break;
            }
            uint8_t stap_nal_type =
                payload[offset] & 0x1f;
            if (stap_nal_type == 5 &&
                !append_h264_parameter_sets(network))
                network->h264_access_unit_damaged = 1;
            if (!append_annexb(network, payload[offset],
                               payload + offset + 1, nal_size - 1))
                network->h264_access_unit_damaged = 1;
            if (stap_nal_type >= 1 && stap_nal_type <= 5)
                network->h264_access_unit_has_vcl = 1;
            if (stap_nal_type == 7)
                network->h264_nal_type7_count++;
            else if (stap_nal_type == 8)
                network->h264_nal_type8_count++;
            else if (stap_nal_type == 5) {
                network->h264_seen_idr = 1;
                network->h264_access_unit_has_idr = 1;
                network->h264_complete_idr_units++;
            } else if (stap_nal_type == 1) {
                network->h264_complete_slice_units++;
            }
            offset += nal_size;
        }
    } else if (nal_type == 28 && payload_size >= 2) {
        uint8_t fu_header = payload[1];
        uint8_t fu_nal_type = fu_header & 0x1f;
        uint16_t sequence =
            ((uint16_t)packet->data[2] << 8) |
            packet->data[3];
        int start = (fu_header & 0x80) != 0;
        int end = (fu_header & 0x40) != 0;
        if (fu_nal_type == 1 && !network->h264_seen_idr) {
            network->h264_access_unit_damaged = 1;
        } else if (start) {
            if (network->h264_fragment_active) {
                network->h264_rejected_packets++;
                network->h264_access_unit_damaged = 1;
            }
            network->h264_fragment_sequence = sequence;
            network->h264_fragment_active = 1;
            network->h264_fu_start_count++;
            network->h264_fragment_timestamp = rtp_timestamp;
            if (fu_nal_type == 5)
                network->h264_access_unit_has_idr = 1;
            if (fu_nal_type == 5 &&
                !append_h264_parameter_sets(network))
                network->h264_access_unit_damaged = 1;
            uint8_t nal_header =
                (payload[0] & 0xe0) | fu_nal_type;
            if (!append_annexb(network, nal_header,
                               payload + 2, payload_size - 2))
                network->h264_access_unit_damaged = 1;
            if (fu_nal_type >= 1 && fu_nal_type <= 5)
                network->h264_access_unit_has_vcl = 1;
        } else {
            if (!network->h264_fragment_active)
                network->h264_access_unit_damaged = 1;
            else if (network->h264_fragment_timestamp != rtp_timestamp ||
                sequence !=
                    (uint16_t)(network->h264_fragment_sequence + 1)) {
                if (network->h264_fragment_timestamp != rtp_timestamp)
                    network->h264_fu_lost_packets++;
                else if (sequence ==
                         network->h264_fragment_sequence)
                    network->h264_fu_duplicate_packets++;
                else
                    network->h264_fu_out_of_order_packets++;
                network->h264_fragment_active = 0;
                network->h264_rejected_packets++;
                network->h264_access_unit_damaged = 1;
            } else {
                network->h264_fu_continuation_count++;
                if (!append_annexb_fragment(network, payload + 2,
                                            payload_size - 2))
                    network->h264_access_unit_damaged = 1;
            }
        }
        if (network->h264_fragment_active) {
            network->h264_fragment_sequence = sequence;
            if (end) {
                network->h264_fu_end_count++;
                network->h264_fragment_active = 0;
                if (fu_nal_type == 5) {
                    network->h264_seen_idr = 1;
                    network->h264_complete_idr_units++;
                } else if (fu_nal_type == 1) {
                    network->h264_complete_slice_units++;
                }
            }
        }
    } else {
        network->h264_access_unit_damaged = 1;
        network->h264_rejected_packets++;
    }

    if (!marker)
        return 0;
    if (network->h264_fragment_active ||
        network->h264_access_unit_damaged ||
        !network->h264_fragment_size ||
        !network->h264_access_unit_has_vcl ||
        !network->h264_seen_idr) {
        int damaged = network->h264_fragment_active ||
                      network->h264_access_unit_damaged;
        int lost_idr = network->h264_access_unit_has_idr;
        network->h264_fragment_size = 0;
        network->h264_fragment_active = 0;
        network->h264_access_unit_active = 0;
        network->h264_access_unit_damaged = 0;
        network->h264_access_unit_has_vcl = 0;
        network->h264_access_unit_has_idr = 0;
        if (damaged && lost_idr)
            network->h264_seen_idr = 0;
        return 0;
    }
    network->h264_assembled_units++;
    av_packet_unref(packet);
    uint8_t *data = av_malloc(network->h264_fragment_size);
    if (!data)
        return 0;
    memcpy(data, network->h264_fragment,
           network->h264_fragment_size);
    network->h264_last_reconstructed_size =
        network->h264_fragment_size;
    uint32_t reconstructed_hash = 2166136261u;
    for (size_t i = 0; i < network->h264_fragment_size; i++)
        reconstructed_hash =
            (reconstructed_hash ^ network->h264_fragment[i]) *
            16777619u;
#ifdef DEBUG
    static Uint64 h264_access_units_logged = 0;
    h264_access_units_logged++;
    if (h264_access_units_logged <= 10 ||
        h264_access_units_logged % 50 == 0) {
        size_t nal_count = 0;
        size_t nal_types[16] = {0};
        size_t nal_sizes[16] = {0};
        for (size_t i = 0; i < network->h264_fragment_size;) {
            size_t start_code = 0;
            if (i + 3 < network->h264_fragment_size &&
                network->h264_fragment[i] == 0 &&
                network->h264_fragment[i + 1] == 0 &&
                network->h264_fragment[i + 2] == 1)
                start_code = 3;
            else if (i + 4 < network->h264_fragment_size &&
                     network->h264_fragment[i] == 0 &&
                     network->h264_fragment[i + 1] == 0 &&
                     network->h264_fragment[i + 2] == 0 &&
                     network->h264_fragment[i + 3] == 1)
                start_code = 4;
            if (!start_code)
                break;
            size_t nal_start = i + start_code;
            size_t nal_end = network->h264_fragment_size;
            for (size_t j = nal_start + 1;
                 j + 3 < network->h264_fragment_size;
                 j++) {
                if (network->h264_fragment[j] == 0 &&
                    network->h264_fragment[j + 1] == 0 &&
                    (network->h264_fragment[j + 2] == 1 ||
                     (network->h264_fragment[j + 2] == 0 &&
                      j + 3 < network->h264_fragment_size &&
                      network->h264_fragment[j + 3] == 1))) {
                    nal_end = j;
                    break;
                }
            }
            if (nal_count < 16) {
                nal_types[nal_count] =
                    network->h264_fragment[nal_start] & 0x1f;
                nal_sizes[nal_count] = nal_end - nal_start;
            }
            nal_count++;
            i = nal_end;
        }
        debug_log(
            "H264 access unit %llu: timestamp=%u seq=%u-%u size=%zu hash=%08x nal_count=%zu nal_types=",
            (unsigned long long)h264_access_units_logged,
            (unsigned)rtp_timestamp,
            (unsigned)network->h264_fragment_first_sequence,
            (unsigned)rtp_sequence,
            network->h264_fragment_size,
            (unsigned)reconstructed_hash,
            nal_count
        );
        for (size_t i = 0; i < nal_count && i < 16; i++)
            debug_log("%zu:%zu,", nal_types[i], nal_sizes[i]);
        debug_log("\n");
    }
#endif
    network->h264_last_reconstructed_hash = reconstructed_hash;
    network->h264_last_reconstructed_first =
        network->h264_fragment_size > 0
            ? network->h264_fragment[0] : 0;
    network->h264_last_reconstructed_second =
        network->h264_fragment_size > 1
            ? network->h264_fragment[1] : 0;
    network->h264_last_reconstructed_third =
        network->h264_fragment_size > 2
            ? network->h264_fragment[2] : 0;
    network->h264_last_reconstructed_fourth =
        network->h264_fragment_size > 3
            ? network->h264_fragment[3] : 0;
    network->h264_last_reconstructed_size =
        network->h264_fragment_size;
    av_packet_from_data(packet, data,
                        (int)network->h264_fragment_size);
    packet->pts = (int64_t)rtp_timestamp;
    packet->dts = (int64_t)rtp_timestamp;
    network->h264_fragment_size = 0;
    network->h264_access_unit_active = 0;
    network->h264_access_unit_damaged = 0;
    network->h264_access_unit_has_vcl = 0;
    network->h264_access_unit_has_idr = 0;
    return 1;
}

#ifdef __SWITCH__
static void configure_udp_receive_buffer(int socket_fd,
                                         const char *stream_name)
{
    int requested_size = 1024 * 1024;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF,
                   &requested_size, sizeof(requested_size)) < 0) {
        debug_log("%s UDP receive buffer setup failed: %d (%s)\n",
                  stream_name, errno, strerror(errno));
        fprintf(stderr,
                "%s UDP receive buffer setup failed: %d (%s)\n",
                stream_name, errno, strerror(errno));
        return;
    }

    int actual_size = 0;
    socklen_t actual_size_length = sizeof(actual_size);
    if (getsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF,
                   &actual_size, &actual_size_length) == 0) {
        debug_log("%s UDP receive buffer: requested=%d actual=%d\n",
                  stream_name, requested_size, actual_size);
    }
}
#endif

static int demux_thread(void *opaque)
{
    Network *network = opaque;
#ifdef DEBUG
    debug_log("RTP demux thread body started\n");
    debug_log("video format pointer: %p\n", (void *)network->video_format);
    debug_log("audio format pointer: %p\n", (void *)network->audio_format);
#endif
#ifdef __SWITCH__
    network->video_socket = -1;
    network->audio_socket = -1;
    if (R_FAILED(socketInitializeDefault())) {
#ifdef DEBUG
        debug_log("socketInitializeDefault failed\n");
#endif
    }

    int video_port = 5004;
    int audio_port = 5006;
    char *video_port_text = strrchr(network->video_address, ':');
    char *audio_port_text = strrchr(network->audio_address, ':');
    if (video_port_text)
        video_port = atoi(video_port_text + 1);
    if (audio_port_text)
        audio_port = atoi(audio_port_text + 1);

    network->video_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    network->audio_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (network->video_socket >= 0)
        configure_udp_receive_buffer(network->video_socket, "video");
    if (network->audio_socket >= 0)
        configure_udp_receive_buffer(network->audio_socket, "audio");

    struct sockaddr_in video_addr;
    memset(&video_addr, 0, sizeof(video_addr));
    video_addr.sin_family = AF_INET;
    video_addr.sin_port = htons((uint16_t)video_port);
    video_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(network->video_socket, (struct sockaddr *)&video_addr,
             sizeof(video_addr)) < 0) {
#ifdef DEBUG
        debug_log("video UDP bind failed: %d (%s)\n",
                  errno, strerror(errno));
#endif
    } else {
#ifdef DEBUG
        debug_log("video UDP bind succeeded on port %d\n", video_port);
#endif
    }

    struct sockaddr_in audio_addr;
    memset(&audio_addr, 0, sizeof(audio_addr));
    audio_addr.sin_family = AF_INET;
    audio_addr.sin_port = htons((uint16_t)audio_port);
    audio_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(network->audio_socket, (struct sockaddr *)&audio_addr,
             sizeof(audio_addr)) < 0) {
#ifdef DEBUG
        debug_log("audio UDP bind failed: %d (%s)\n",
                  errno, strerror(errno));
#endif
    } else {
#ifdef DEBUG
        debug_log("audio UDP bind succeeded on port %d\n", audio_port);
#endif
    }

    struct pollfd sockets[2] = {
        { network->video_socket, POLLIN, 0 },
        { network->audio_socket, POLLIN, 0 },
    };
    AVPacket *packet = av_packet_alloc();
    Uint32 packet_summary_ms = SDL_GetTicks();

    while (!SDL_AtomicGet(&network->stop)) {
        int poll_result = poll(sockets, 2, 100);
        if (poll_result > 0) {
            uint8_t datagram[65536];
            if (sockets[0].revents & POLLIN) {
                ssize_t length = recvfrom(network->video_socket,
                                          datagram, sizeof(datagram),
                                          0, NULL, NULL);
                if (length > 0) {
                    network->video_rtp_datagrams_received++;
                    network->video_rtp_bytes_received +=
                        (Uint64)length;
                    av_packet_unref(packet);
                    uint8_t *data = av_malloc((size_t)length);
                    if (data) {
                        memcpy(data, datagram, (size_t)length);
                        av_packet_from_data(packet, data, (int)length);
                        if (depacketize_h264(network, packet)) {
                            packet->stream_index = network->video_index;
                            network->video_packets_received++;
                            network->video_bytes_received += packet->size;
                            packet_queue_push(&network->queue, packet);
                        }
                    }
                }
            }
            if (sockets[1].revents & POLLIN) {
                ssize_t length = recvfrom(network->audio_socket,
                                          datagram, sizeof(datagram),
                                          0, NULL, NULL);
                if (length > 0) {
                    av_packet_unref(packet);
                    uint8_t *data = av_malloc((size_t)length);
                    if (data) {
                        memcpy(data, datagram, (size_t)length);
                        av_packet_from_data(packet, data, (int)length);
                        uint8_t *payload = NULL;
                        size_t payload_size = 0;
                        if (rtp_strip_header(packet, 97, &payload,
                                             &payload_size)) {
                            uint8_t *audio_data =
                                av_malloc(payload_size);
                            if (audio_data) {
                                memcpy(audio_data, payload,
                                       payload_size);
                                av_packet_unref(packet);
                                av_packet_from_data(packet, audio_data,
                                                    (int)payload_size);
                                packet->stream_index =
                                    network->audio_index;
                                network->audio_packets_received++;
                                network->audio_bytes_received +=
                                    packet->size;
                                packet_queue_push(&network->queue,
                                                  packet);
                            }
                        }
                    }
                }
            }
        }
#ifdef DEBUG
        Uint32 now = SDL_GetTicks();
        if (now - packet_summary_ms >= 1000) {
            debug_log("RTP packet summary\n");
            debug_log("video RTP datagrams received: %llu, bytes: %llu\n",
                        (unsigned long long)network->video_rtp_datagrams_received,
                        (unsigned long long)network->video_rtp_bytes_received);
            debug_log("video access units queued: %llu, bytes: %llu\n",
                        (unsigned long long)network->video_packets_received,
                        (unsigned long long)network->video_bytes_received);
            debug_log("audio packets: %llu, audio bytes: %llu\n",
                        (unsigned long long)network->audio_packets_received,
                        (unsigned long long)network->audio_bytes_received);
            debug_log("H264 assembled units: %llu, rejected RTP packets: %llu\n",
                        (unsigned long long)network->h264_assembled_units,
                        (unsigned long long)network->h264_rejected_packets);
            debug_log("H264 SPS count: %llu, PPS count: %llu\n",
                        (unsigned long long)network->h264_nal_type7_count,
                        (unsigned long long)network->h264_nal_type8_count);
            debug_log("H264 FU-A start: %llu, continuation: %llu, end: %llu\n",
                        (unsigned long long)network->h264_fu_start_count,
                        (unsigned long long)network->h264_fu_continuation_count,
                        (unsigned long long)network->h264_fu_end_count);
            debug_log("H264 RTP markers: %llu, sequence gaps: %llu\n",
                        (unsigned long long)network->h264_marker_packets,
                        (unsigned long long)network->h264_sequence_gaps);
            debug_log("H264 invalid RTP datagrams: %llu\n",
                        (unsigned long long)network->h264_invalid_rtp_packets);
            debug_log("H264 timestamp changes: %llu\n",
                        (unsigned long long)network->h264_timestamp_changes);
            debug_log(
                "H264 FU-A lost: %llu, duplicate: %llu, out-of-order: %llu, complete IDR: %llu, complete slices: %llu\n",
                (unsigned long long)network->h264_fu_lost_packets,
                (unsigned long long)network->h264_fu_duplicate_packets,
                (unsigned long long)network->h264_fu_out_of_order_packets,
                (unsigned long long)network->h264_complete_idr_units,
                (unsigned long long)network->h264_complete_slice_units);
            debug_log("H264 last reconstructed size: %zu bytes, prefix=%02x%02x%02x%02x, hash=%08x\n",
                        network->h264_last_reconstructed_size,
                        network->h264_last_reconstructed_first,
                        network->h264_last_reconstructed_second,
                        network->h264_last_reconstructed_third,
                        network->h264_last_reconstructed_fourth,
                        (unsigned)network->h264_last_reconstructed_hash);
            packet_summary_ms = now;
        }
#endif
    }
    av_packet_free(&packet);
    if (network->video_socket >= 0)
        close(network->video_socket);
    if (network->audio_socket >= 0)
        close(network->audio_socket);
    socketExit();
    return 0;
#else
    avformat_network_init();
    AVDictionary *udp_options = NULL;
    av_dict_set(&udp_options, "protocol_whitelist", "udp", 0);
    av_dict_set(&udp_options, "listen", "1", 0);
    av_dict_set(&udp_options, "buffer_size", "4194304", 0);
    av_dict_set(&udp_options, "fifo_size", "4194304", 0);
    av_dict_set(&udp_options, "reuse", "1", 0);

    if (network->video_address[0]) {
        char video_udp_address[128];
        char *port = strrchr(network->video_address, ':');
        snprintf(video_udp_address, sizeof(video_udp_address),
                 "udp://0.0.0.0:%s",
                 port ? port + 1 : "5004");
        int video_error = avformat_open_input(
            &network->video_format,
            video_udp_address,
            av_find_input_format("udp"),
            &udp_options
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
        char audio_udp_address[128];
        char *port = strrchr(network->audio_address, ':');
        snprintf(audio_udp_address, sizeof(audio_udp_address),
                 "udp://0.0.0.0:%s",
                 port ? port + 1 : "5006");
        int audio_error = avformat_open_input(
            &network->audio_format,
            audio_udp_address,
            av_find_input_format("udp"),
            &udp_options
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
    av_dict_free(&udp_options);
    AVPacket *packet = av_packet_alloc();
    Uint32 packet_summary_ms = SDL_GetTicks();
    while (!SDL_AtomicGet(&network->stop)) {
        av_packet_unref(packet);
        int read_error = AVERROR(EAGAIN);
        int packet_stream_index = -1;
        if (network->video_format) {
            read_error = av_read_frame(network->video_format, packet);
            if (read_error >= 0) {
                if (depacketize_h264(network, packet)) {
                    packet_stream_index = network->video_index;
                    network->video_packets_received++;
                    network->video_bytes_received += packet->size;
                } else {
                    read_error = AVERROR(EAGAIN);
                }
            }
        }
        if (read_error < 0 && network->audio_format) {
            av_packet_unref(packet);
            read_error = av_read_frame(network->audio_format, packet);
            if (read_error >= 0) {
                uint8_t *payload = NULL;
                size_t payload_size = 0;
                if (rtp_strip_header(packet, 97, &payload,
                                     &payload_size)) {
                    uint8_t *audio_data = av_malloc(payload_size);
                    if (audio_data) {
                        memcpy(audio_data, payload, payload_size);
                        av_packet_unref(packet);
                        av_packet_from_data(packet, audio_data,
                                            (int)payload_size);
                        packet_stream_index = network->audio_index;
                        network->audio_packets_received++;
                        network->audio_bytes_received += packet->size;
                    } else {
                        read_error = AVERROR(ENOMEM);
                    }
                } else {
                    read_error = AVERROR(EAGAIN);
                }
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
        if (now - packet_summary_ms >= 1000) {
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
#endif
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
    debug_log("RTP network start\n");
    debug_log("video receive URL: %s\n", network->video_address);
    debug_log("audio receive URL: %s\n", network->audio_address);
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
    free(network->h264_fragment);
    network->h264_fragment = NULL;
    free(network->h264_parameter_sets);
    network->h264_parameter_sets = NULL;
    network->h264_parameter_sets_size = 0;
    free(network->video_sdp_reader);
    free(network->audio_sdp_reader);
    network->video_sdp_reader = NULL;
    network->audio_sdp_reader = NULL;
    free(network->sdp_reader);
    network->sdp_reader = NULL;
    network->sdp_io = NULL;
    avformat_network_deinit();
#ifdef DEBUG
    debug_log_shutdown();
#endif
}
