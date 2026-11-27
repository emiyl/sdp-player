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
    Uint64 dropped_packets;
} PacketQueue;

typedef struct Network {
    AVFormatContext *format;
    AVFormatContext *video_format;
    AVFormatContext *audio_format;
    AVIOContext *sdp_io;
    void *sdp_reader;
    AVIOContext *video_sdp_io;
    AVIOContext *audio_sdp_io;
    void *video_sdp_reader;
    void *audio_sdp_reader;
    char address[128];
    char video_address[128];
    char audio_address[128];
    char video_sdp[1024];
    char audio_sdp[1024];
    PacketQueue queue;
    SDL_atomic_t stop;
    SDL_Thread *thread;
    int video_index;
    int audio_index;
    int has_video;
    int has_audio;
    Uint64 dropped_packets;
    Uint32 last_read_error_ms;
    Uint64 video_rtp_datagrams_received;
    Uint64 video_rtp_bytes_received;
    Uint64 video_packets_received;
    Uint64 audio_packets_received;
    Uint64 video_bytes_received;
    Uint64 audio_bytes_received;
    Uint64 h264_assembled_units;
    Uint64 h264_rejected_packets;
    uint8_t *h264_fragment;
    size_t h264_fragment_size;
    size_t h264_fragment_capacity;
    uint16_t h264_fragment_sequence;
    uint16_t h264_fragment_first_sequence;
    int h264_fragment_active;
    uint32_t h264_access_unit_timestamp;
    int h264_access_unit_active;
    int h264_access_unit_damaged;
    int h264_access_unit_has_vcl;
    int h264_access_unit_has_idr;
    uint64_t h264_nal_type7_count;
    uint64_t h264_nal_type8_count;
    uint64_t h264_fu_start_count;
    uint64_t h264_fu_end_count;
    uint64_t h264_fu_continuation_count;
    uint64_t h264_marker_packets;
    uint64_t h264_sequence_gaps;
    uint64_t h264_timestamp_changes;
    uint16_t h264_last_rtp_sequence;
    uint32_t h264_last_rtp_ssrc;
    int h264_have_last_rtp_sequence;
    uint64_t h264_invalid_rtp_packets;
    uint32_t h264_fragment_timestamp;
    uint32_t h264_last_reconstructed_hash;
    size_t h264_last_reconstructed_size;
    uint8_t h264_last_reconstructed_first;
    uint8_t h264_last_reconstructed_second;
    uint8_t h264_last_reconstructed_third;
    uint8_t h264_last_reconstructed_fourth;
    uint64_t h264_fu_lost_packets;
    uint64_t h264_fu_duplicate_packets;
    uint64_t h264_fu_out_of_order_packets;
    uint64_t h264_complete_idr_units;
    uint64_t h264_complete_slice_units;
    uint8_t *h264_parameter_sets;
    size_t h264_parameter_sets_size;
    int h264_seen_idr;
    uint32_t h264_last_timestamp;
#ifdef __SWITCH__
    int video_socket;
    int audio_socket;
#endif
} Network;

#ifdef __cplusplus
extern "C" {
#endif
int network_open(Network *network, const char *sdp_path);
int network_start(Network *network);
void network_disable_audio(Network *network);
void network_disable_video(Network *network);
int network_pop_packet(Network *network, AVPacket *packet);
Uint64 network_dropped_packets(Network *network);
void network_stop(Network *network);
void network_close(Network *network);
size_t network_queue_depth(Network *network);
const char *network_address(const Network *network);
AVPacket *network_packet_alloc(void);
void network_packet_free(AVPacket **packet);
void network_packet_unref(AVPacket *packet);
int network_packet_new(AVPacket *packet, int size);
void network_packet_move_ref(AVPacket *dst, AVPacket *src);
#ifdef __cplusplus
}
#endif

#endif
