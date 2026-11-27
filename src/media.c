#include "media.h"
#include "network.h"
#include "logging.h"

#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define AUDIO_RING_SIZE (64 * 1024)
#define AUDIO_PREBUFFER_BYTES (16 * 1024)

typedef struct AudioRing {
    unsigned char data[AUDIO_RING_SIZE];
    size_t read_pos;
    size_t write_pos;
} AudioRing;

struct AudioPacketEntry {
    AVPacket *packet;
    struct AudioPacketEntry *next;
};

static AudioRing audio_ring;
static SDL_mutex *audio_ring_mutex;

static size_t ring_available(void)
{
    if (audio_ring.write_pos >= audio_ring.read_pos)
        return audio_ring.write_pos - audio_ring.read_pos;
    return AUDIO_RING_SIZE - audio_ring.read_pos + audio_ring.write_pos;
}

static void ring_write(const unsigned char *src, size_t len)
{
    if (len >= AUDIO_RING_SIZE) {
        src += len - (AUDIO_RING_SIZE - 1);
        len = AUDIO_RING_SIZE - 1;
        audio_ring.read_pos = audio_ring.write_pos;
    }

    size_t free = AUDIO_RING_SIZE - 1 - ring_available();
    if (len > free) {
        size_t overflow = len - free;
        audio_ring.read_pos =
            (audio_ring.read_pos + overflow) % AUDIO_RING_SIZE;
    }
    size_t first = AUDIO_RING_SIZE - audio_ring.write_pos;
    if (first > len)
        first = len;
    memcpy(audio_ring.data + audio_ring.write_pos, src, first);
    if (len > first)
        memcpy(audio_ring.data, src + first, len - first);
    audio_ring.write_pos =
        (audio_ring.write_pos + len) % AUDIO_RING_SIZE;
}

static void ring_read(unsigned char *dst, size_t len)
{
    size_t available = ring_available();
    if (len > available)
        len = available;
    size_t first = AUDIO_RING_SIZE - audio_ring.read_pos;
    if (first > len)
        first = len;
    memcpy(dst, audio_ring.data + audio_ring.read_pos, first);
    if (len > first)
        memcpy(dst + first, audio_ring.data, len - first);
    audio_ring.read_pos =
        (audio_ring.read_pos + len) % AUDIO_RING_SIZE;
}

static size_t ring_buffered(void)
{
    if (audio_ring.write_pos >= audio_ring.read_pos)
        return audio_ring.write_pos - audio_ring.read_pos;
    return AUDIO_RING_SIZE - audio_ring.read_pos + audio_ring.write_pos;
}

static void audio_callback(void *userdata, Uint8 *stream, int length)
{
    (void)userdata;
    memset(stream, 0, (size_t)length);
    if (!audio_ring_mutex)
        return;
    SDL_LockMutex(audio_ring_mutex);
    if (ring_buffered() >= AUDIO_PREBUFFER_BYTES)
        ring_read(stream, (size_t)length);
    else
        memset(stream, 0, (size_t)length);
    SDL_UnlockMutex(audio_ring_mutex);
}

static void av_error(const char *operation, int error)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(error, buffer, sizeof(buffer));
    fprintf(stderr, "%s: %s\n", operation, buffer);
}

static void audio_queue_push(Media *media, AVPacket *packet)
{
    struct AudioPacketEntry *overflow = NULL;
    struct AudioPacketEntry *entry = malloc(sizeof(*entry));
    if (!entry)
        return;
    entry->packet = av_packet_alloc();
    av_packet_move_ref(entry->packet, packet);
    entry->next = NULL;

    SDL_LockMutex(media->audio_queue_mutex);
    if (media->audio_packet_count >= 16) {
        overflow = media->audio_packets_head;
        media->audio_packets_head = overflow->next;
        if (!media->audio_packets_head)
            media->audio_packets_tail = NULL;
        media->audio_packet_count--;
    }
    if (media->audio_packets_tail)
        media->audio_packets_tail->next = entry;
    else
        media->audio_packets_head = entry;
    media->audio_packets_tail = entry;
    media->audio_packet_count++;
    SDL_UnlockMutex(media->audio_queue_mutex);

    if (overflow) {
        av_packet_free(&overflow->packet);
        free(overflow);
    }
}

static struct AudioPacketEntry *audio_queue_pop(Media *media)
{
    SDL_LockMutex(media->audio_queue_mutex);
    struct AudioPacketEntry *entry = media->audio_packets_head;
    if (!entry) {
        SDL_UnlockMutex(media->audio_queue_mutex);
        return NULL;
    }
    media->audio_packets_head = entry->next;
    if (!media->audio_packets_head)
        media->audio_packets_tail = NULL;
    if (media->audio_packet_count > 0)
        media->audio_packet_count--;
    SDL_UnlockMutex(media->audio_queue_mutex);
    return entry;
}

static int audio_thread(void *opaque)
{
    Media *media = opaque;
    AVFrame *frame = av_frame_alloc();

    while (!SDL_AtomicGet(&media->audio_stop)) {
        struct AudioPacketEntry *entry = audio_queue_pop(media);
        if (!entry) {
            SDL_Delay(1);
            continue;
        }

        int audio_send_result =
            avcodec_send_packet(media->audio_decoder, entry->packet);
#ifdef DEBUG
        static Uint64 audio_packets_logged = 0;
        audio_packets_logged++;
        if (audio_packets_logged == 1 ||
            audio_packets_logged % 100 == 0) {
            debug_log(
                "Opus decoder packets: %llu, send result: %d, packet bytes: %d\n",
                (unsigned long long)audio_packets_logged,
                audio_send_result,
                entry->packet->size
            );
        }
#endif
        if (audio_send_result >= 0) {
            while (avcodec_receive_frame(media->audio_decoder, frame) >= 0) {
                if (media->audio_device) {
                    debug_log(
                        "Opus decoder frame: samples=%d, sample_rate=%d, queued_bytes=%u\n",
                        frame->nb_samples,
                        frame->sample_rate,
                        (unsigned)SDL_GetQueuedAudioSize(media->audio_device));
                }
                if (media->swr) {
                    int capacity = av_rescale_rnd(
                        frame->nb_samples, 48000,
                        frame->sample_rate, AV_ROUND_UP
                    ) + 32;
                    uint8_t *output = NULL;
                    av_samples_alloc(&output, NULL, 2, capacity,
                                     AV_SAMPLE_FMT_S16, 1);
                    int converted = swr_convert(
                        media->swr, &output, capacity,
                        (const uint8_t **)frame->extended_data,
                        frame->nb_samples
                    );
                    if (converted > 0) {
                        size_t bytes =
                            (size_t)converted * 2 * sizeof(int16_t);
                        if (media->audio_device) {
                            if (SDL_GetQueuedAudioSize(media->audio_device) < 32768) {
                                SDL_QueueAudio(media->audio_device, output,
                                               (Uint32)bytes);
                            }
                            if (SDL_GetQueuedAudioSize(media->audio_device) >= 16384)
                                SDL_PauseAudioDevice(media->audio_device, 0);
                        }
                        if (media->profile)
                            profile_audio_frame(media->profile, bytes);
                    }
                    av_freep(&output);
                }
                av_frame_unref(frame);
            }
        }
        av_packet_free(&entry->packet);
        free(entry);
    }

    av_frame_free(&frame);
    return 0;
}

typedef struct H264BitReader {
    const uint8_t *data;
    size_t size;
    size_t bit_position;
} H264BitReader;

static uint32_t h264_read_bit(H264BitReader *reader)
{
    size_t byte_index = reader->bit_position / 8;
    if (byte_index >= reader->size)
        return 0;
    unsigned shift =
        7 - (unsigned)(reader->bit_position % 8);
    reader->bit_position++;
    return (reader->data[byte_index] >> shift) & 1;
}

static uint32_t h264_read_bits(H264BitReader *reader, unsigned count)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < count; i++)
        value = (value << 1) | h264_read_bit(reader);
    return value;
}

static uint32_t h264_read_ue(H264BitReader *reader)
{
    unsigned zero_count = 0;
    while (h264_read_bit(reader) == 0 && zero_count < 32)
        zero_count++;
    uint32_t suffix = h264_read_bits(reader, zero_count);
    return suffix + ((1u << zero_count) - 1);
}

static int h264_skip_scaling_list(H264BitReader *reader,
                                  unsigned size)
{
    int32_t last_scale = 8;
    int32_t next_scale = 8;
    for (unsigned i = 0; i < size; i++) {
        if (next_scale != 0) {
            uint32_t delta = h264_read_ue(reader);
            int32_t signed_delta =
                (int32_t)((delta & 1) ? (delta + 1) / 2
                                      : -(int32_t)(delta / 2));
            next_scale = (last_scale + signed_delta + 256) % 256;
        }
        if (next_scale != 0)
            last_scale = next_scale;
    }
    return 1;
}

static int h264_parse_dimensions(const uint8_t *sps,
                                 size_t sps_size,
                                 int *width,
                                 int *height)
{
    if (sps_size < 4)
        return 0;
    H264BitReader reader = { sps, sps_size, 0 };

    unsigned profile_idc =
        h264_read_bits(&reader, 8);
    h264_read_bits(&reader, 16);
    h264_read_ue(&reader);

    unsigned chroma_format_idc = 1;
    if (profile_idc == 100 || profile_idc == 110 ||
        profile_idc == 122 || profile_idc == 244 ||
        profile_idc == 44 || profile_idc == 83 ||
        profile_idc == 86 || profile_idc == 118 ||
        profile_idc == 128 || profile_idc == 138 ||
        profile_idc == 139 || profile_idc == 134) {
        chroma_format_idc = h264_read_ue(&reader);
        if (chroma_format_idc == 3)
            h264_read_bit(&reader);
        h264_read_ue(&reader);
        h264_read_ue(&reader);
        h264_read_bit(&reader);
        if (h264_read_bit(&reader)) {
            unsigned scaling_count =
                chroma_format_idc != 3 ? 8u : 12u;
            for (unsigned i = 0; i < scaling_count; i++) {
                if (h264_read_bit(&reader))
                    h264_skip_scaling_list(
                        &reader, i < 6 ? 16u : 64u);
                else
                    h264_read_ue(&reader);
            }
        }
    }

    h264_read_ue(&reader);
    unsigned pic_order_cnt_type = h264_read_ue(&reader);
    if (pic_order_cnt_type == 0) {
        h264_read_ue(&reader);
    } else if (pic_order_cnt_type == 1) {
        h264_read_bit(&reader);
        h264_read_ue(&reader);
        h264_read_ue(&reader);
        unsigned cycle_count = h264_read_ue(&reader);
        for (unsigned i = 0; i < cycle_count; i++)
            h264_read_ue(&reader);
    }
    h264_read_ue(&reader);
    h264_read_bit(&reader);
    unsigned pic_width_in_mbs =
        h264_read_ue(&reader) + 1;
    unsigned pic_height_in_map_units =
        h264_read_ue(&reader) + 1;
    unsigned frame_mbs_only =
        h264_read_bit(&reader);
    if (!frame_mbs_only)
        h264_read_bit(&reader);
    h264_read_bit(&reader);

    unsigned frame_crop_left = 0;
    unsigned frame_crop_right = 0;
    unsigned frame_crop_top = 0;
    unsigned frame_crop_bottom = 0;
    if (h264_read_bit(&reader)) {
        frame_crop_left = h264_read_ue(&reader);
        frame_crop_right = h264_read_ue(&reader);
        frame_crop_top = h264_read_ue(&reader);
        frame_crop_bottom = h264_read_ue(&reader);
    }

    unsigned crop_unit_x = 1;
    unsigned crop_unit_y = 2 - frame_mbs_only;
    if (chroma_format_idc == 1) {
        crop_unit_x = 2;
        crop_unit_y = 2 * crop_unit_y;
    } else if (chroma_format_idc == 2) {
        crop_unit_x = 2;
        crop_unit_y = crop_unit_y;
    } else if (chroma_format_idc == 3) {
        crop_unit_x = 1;
        crop_unit_y = crop_unit_y;
    }

    int base_width =
        (int)(pic_width_in_mbs * 16);
    int base_height =
        (int)(pic_height_in_map_units * 16 *
              (2 - frame_mbs_only));
    int cropped_width =
        base_width -
        (int)((frame_crop_left + frame_crop_right) *
              crop_unit_x);
    int cropped_height =
        base_height -
        (int)((frame_crop_top + frame_crop_bottom) *
              crop_unit_y);
    if (cropped_width <= 0 || cropped_height <= 0)
        return 0;
    *width = cropped_width;
    *height = cropped_height;
    return 1;
}

static void cache_h264_extradata(Media *media,
                                 AVCodecContext *decoder)
{
    if (!decoder || !decoder->extradata ||
        decoder->extradata_size <= 0)
        return;

    uint8_t *parameters =
        av_malloc((size_t)decoder->extradata_size +
                  AV_INPUT_BUFFER_PADDING_SIZE);
    if (!parameters)
        return;
    memcpy(parameters, decoder->extradata,
           (size_t)decoder->extradata_size);
    memset(parameters + decoder->extradata_size, 0,
           AV_INPUT_BUFFER_PADDING_SIZE);

    free(media->h264_parameter_sets);
    media->h264_parameter_sets =
        malloc((size_t)decoder->extradata_size);
    if (!media->h264_parameter_sets) {
        av_free(parameters);
        return;
    }
    memcpy(media->h264_parameter_sets, parameters,
           (size_t)decoder->extradata_size);
    media->h264_parameter_sets_size =
        (size_t)decoder->extradata_size;

    size_t position = 0;
    while (position + 4 < (size_t)decoder->extradata_size) {
        if (parameters[position] == 0 &&
            parameters[position + 1] == 0 &&
            parameters[position + 2] == 0 &&
            parameters[position + 3] == 1) {
            size_t nal_start = position + 4;
            size_t next = nal_start;
            while (next + 3 <
                   (size_t)decoder->extradata_size &&
                   !(parameters[next] == 0 &&
                     parameters[next + 1] == 0 &&
                     parameters[next + 2] == 0 &&
                     parameters[next + 3] == 1))
                next++;
            unsigned nal_type =
                next > nal_start
                    ? parameters[nal_start] & 0x1f
                    : 0;
            if (nal_type == 7) {
                free(media->h264_sps);
                media->h264_sps_size =
                    next - position;
                media->h264_sps =
                    malloc(media->h264_sps_size);
                if (media->h264_sps)
                    memcpy(media->h264_sps,
                           parameters + position,
                           media->h264_sps_size);
            } else if (nal_type == 8) {
                free(media->h264_pps);
                media->h264_pps_size =
                    next - position;
                media->h264_pps =
                    malloc(media->h264_pps_size);
                if (media->h264_pps)
                    memcpy(media->h264_pps,
                           parameters + position,
                           media->h264_pps_size);
            }
            position = next;
        } else {
            position++;
        }
    }

    media->h264_ready =
        media->h264_sps && media->h264_pps ? 1 : 0;
    av_free(parameters);
#ifdef DEBUG
    debug_log(
        "H264 extradata cached: SPS=%s, PPS=%s, size=%d\n",
        media->h264_sps ? "yes" : "no",
        media->h264_pps ? "yes" : "no",
        decoder->extradata_size
    );
#endif
}

static void cache_h264_parameters(Media *media, AVPacket *packet)
{
#ifdef DEBUG
    static Uint64 h264_packets_logged = 0;
    h264_packets_logged++;
    if (h264_packets_logged == 1 ||
        h264_packets_logged % 100 == 0) {
        debug_log(
            "H264 decoder input packets: %llu, bytes: %d, SPS=%s, PPS=%s\n",
            (unsigned long long)h264_packets_logged,
            packet->size,
            media->h264_sps ? "yes" : "no",
            media->h264_pps ? "yes" : "no"
        );
    }
#endif
    for (size_t i = 0; i + 4 < (size_t)packet->size;) {
        size_t start_code_size = 0;
        if (packet->data[i] == 0 && packet->data[i + 1] == 0 &&
            packet->data[i + 2] == 1)
            start_code_size = 3;
        else if (packet->data[i] == 0 && packet->data[i + 1] == 0 &&
                 packet->data[i + 2] == 0 &&
                 packet->data[i + 3] == 1)
            start_code_size = 4;
        if (!start_code_size) {
            i++;
            continue;
        }

        size_t nal_start = i + start_code_size;
        size_t next = nal_start;
        while (next + 3 < (size_t)packet->size) {
            if (packet->data[next] == 0 &&
                packet->data[next + 1] == 0 &&
                (packet->data[next + 2] == 1 ||
                 (packet->data[next + 2] == 0 &&
                  packet->data[next + 3] == 1)))
                break;
            next++;
        }
        if (next + 3 >= (size_t)packet->size)
            next = packet->size;

        unsigned type = packet->data[nal_start] & 0x1f;
        if (type == 7) {
            free(media->h264_sps);
            media->h264_sps_size = next - i;
            media->h264_sps = malloc(media->h264_sps_size);
            if (media->h264_sps)
                memcpy(media->h264_sps, packet->data + i,
                       media->h264_sps_size);
            int sps_width = 0;
            int sps_height = 0;
            if (h264_parse_dimensions(
                    packet->data + nal_start + 1,
                    next - nal_start - 1,
                    &sps_width,
                    &sps_height) &&
                sps_width > 0 && sps_height > 0) {
                media->h264_width = sps_width;
                media->h264_height = sps_height;
#ifdef DEBUG
                debug_log(
                    "Decoded H264 dimensions: %dx%d\n",
                    sps_width, sps_height);
#endif
            }
        } else if (type == 8) {
            free(media->h264_pps);
            media->h264_pps_size = next - i;
            media->h264_pps = malloc(media->h264_pps_size);
            if (media->h264_pps)
                memcpy(media->h264_pps, packet->data + i,
                       media->h264_pps_size);
        }
        i = next;
    }

    if (!media->h264_sps || !media->h264_pps)
        return;

    size_t parameter_size =
        media->h264_sps_size + media->h264_pps_size;
    uint8_t *parameters = malloc(parameter_size);
    if (!parameters)
        return;
    memcpy(parameters, media->h264_sps, media->h264_sps_size);
    memcpy(parameters + media->h264_sps_size,
           media->h264_pps, media->h264_pps_size);

    free(media->h264_parameter_sets);
    media->h264_parameter_sets = parameters;
    media->h264_parameter_sets_size = parameter_size;
#ifdef DEBUG
    debug_log(
        "H264 SPS/PPS found: SPS=%zu bytes, PPS=%zu bytes\n",
        media->h264_sps_size,
        media->h264_pps_size
    );
#endif
    if (!media->h264_ready) {
        media->h264_ready = 1;
        if (media->video_decoder)
            avcodec_flush_buffers(media->video_decoder);
    }
}

int media_init(Media *media, AVFormatContext *format,
               int video_index, int audio_index,
               int process_audio, int process_video)
{
    memset(media, 0, sizeof(*media));
    media->frame = av_frame_alloc();

    if (process_video && video_index >= 0) {
        AVCodecParameters *parameters =
            format->streams[video_index]->codecpar;
#ifdef __SWITCH2__
        if (parameters->codec_id == AV_CODEC_ID_H264 ||
            parameters->codec_id == AV_CODEC_ID_VP9 ||
            parameters->codec_id == AV_CODEC_ID_AV1) {
            media->switch2_video_decoder_open = 0;
            media->switch2_codec_id = parameters->codec_id;
            media->switch2_width = parameters->width;
            media->switch2_height = parameters->height;
        }
#endif
        const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
        if (codec) {
            media->video_decoder = avcodec_alloc_context3(codec);
            avcodec_parameters_to_context(media->video_decoder, parameters);
            media->video_decoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
            media->video_decoder->thread_count = 1;
            media->video_decoder->thread_type = 0;
            if (avcodec_open2(media->video_decoder, codec, NULL) < 0) {
                avcodec_free_context(&media->video_decoder);
            } else {
                cache_h264_extradata(
                    media,
                    media->video_decoder
                );
            }
        }
    }

    if (process_audio && audio_index >= 0) {
        AVCodecParameters *parameters =
            format->streams[audio_index]->codecpar;
        const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
        if (codec) {
            media->audio_decoder = avcodec_alloc_context3(codec);
            avcodec_parameters_to_context(media->audio_decoder, parameters);
            media->audio_decoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
            media->audio_decoder->thread_count = 1;
            if (avcodec_open2(media->audio_decoder, codec, NULL) < 0)
                avcodec_free_context(&media->audio_decoder);
        }

        if (media->audio_decoder) {
            SDL_AudioSpec wanted = {0};
            SDL_AudioSpec obtained;
            wanted.freq = 48000;
            wanted.format = AUDIO_S16SYS;
            wanted.channels = 2;
            wanted.samples = 256;
            wanted.callback = NULL;
            media->audio_device =
                SDL_OpenAudioDevice(NULL, 0, &wanted, &obtained, 0);
            if (media->audio_device)
                SDL_PauseAudioDevice(media->audio_device, 1);

            AVChannelLayout out_layout;
            av_channel_layout_default(&out_layout, 2);
            swr_alloc_set_opts2(
                &media->swr,
                &out_layout,
                AV_SAMPLE_FMT_S16,
                48000,
                &media->audio_decoder->ch_layout,
                media->audio_decoder->sample_fmt,
                media->audio_decoder->sample_rate,
                0,
                NULL
            );
            av_channel_layout_uninit(&out_layout);
            if (media->swr)
                swr_init(media->swr);
        }
    }

    if (media->audio_decoder) {
        media->audio_queue_mutex = SDL_CreateMutex();
        media->audio_thread =
            SDL_CreateThread(audio_thread, "RTP audio", media);
    }

    return media->video_decoder || media->audio_decoder
#ifdef __SWITCH2__
           || process_video
#endif
           ? 0 : -1;
}

#ifdef __SWITCH2__
static ViddecCodec switch2_codec(enum AVCodecID codec_id)
{
    if (codec_id == AV_CODEC_ID_VP9)
        return ViddecCodec_Vp9;
    if (codec_id == AV_CODEC_ID_AV1)
        return ViddecCodec_Av1;
    return ViddecCodec_H264;
}

static int switch2_init_video_decoder(Media *media,
                                      enum AVCodecID codec_id,
                                      int width,
                                      int height)
{
    if (media->switch2_video_decoder_open)
        return 0;
    if (codec_id == AV_CODEC_ID_H264 &&
        (!media->h264_ready || !media->h264_parameter_sets))
        return -1;

    VideoDecoderConfig config = {0};
    config.codec = switch2_codec(codec_id);
    config.width = media->h264_width > 0
                       ? (u32)media->h264_width
                       : (u32)width;
    config.height = media->h264_height > 0
                        ? (u32)media->h264_height
                        : (u32)height;
    config.tiling = ViddecTilingMode_PitchLinear;
    config.max_sample_size = (u64)width * (u64)height * 3 / 2;
    if (codec_id == AV_CODEC_ID_H264) {
        config.codec_config = media->h264_parameter_sets;
        config.codec_config_size = media->h264_parameter_sets_size;
    }

    Result result = videoDecoderCreate(
        &media->switch2_video_decoder,
        &config
    );
    if (R_FAILED(result)) {
#ifdef DEBUG
        debug_log(
            "Switch 2 hardware decoder create failed: 0x%x\n",
            (unsigned)result
        );
#endif
        fprintf(stderr, "Switch 2 hardware decoder create failed: 0x%x\n",
                (unsigned)result);
        return -1;
    }
#ifdef DEBUG
    debug_log(
        "Switch 2 hardware decoder created: %ux%u, codec=%d\n",
        config.width, config.height, config.codec
    );
#endif
    media->switch2_video_decoder_open = 1;
    return 0;
}

static int switch2_decode_packet(Media *media, AVPacket *packet,
                                 enum AVCodecID codec_id,
                                 int width,
                                 int height,
                                 Profile *profile)
{
    cache_h264_parameters(media, packet);
    if (switch2_init_video_decoder(media, codec_id, width, height) != 0)
        return 0;

    s64 timestamp_ns = (s64)SDL_GetPerformanceCounter() * 1000000000 /
                       (s64)SDL_GetPerformanceFrequency();
    Result result;
    while (1) {
        result = videoDecoderSubmit(
            &media->switch2_video_decoder,
            packet->data,
            packet->size,
            timestamp_ns,
            false,
            100000000
        );
        if (result != VIDDEC_RESULT_SUBMIT_BUSY &&
            R_VALUE(result) != R_VALUE(KERNELRESULT(TimedOut)))
            break;

#ifdef DEBUG
        static Uint64 hardware_submit_busy_count = 0;
        hardware_submit_busy_count++;
        if (hardware_submit_busy_count == 1 ||
            hardware_submit_busy_count % 100 == 0)
            debug_log(
                "Switch 2 hardware decoder waiting to submit: %llu result=0x%x\n",
                (unsigned long long)hardware_submit_busy_count,
                (unsigned)result
            );
#endif
        Result wait_result = eventWait(
            &media->switch2_video_decoder.decoder.submission_ready,
            100000000
        );
        if (R_FAILED(wait_result) &&
            R_VALUE(wait_result) != R_VALUE(KERNELRESULT(TimedOut))) {
#ifdef DEBUG
            debug_log(
                "Switch 2 hardware decoder readiness wait failed: 0x%x\n",
                (unsigned)wait_result
            );
#else
            fprintf(stderr,
                    "Switch 2 hardware decoder readiness wait failed: 0x%x\n",
                    (unsigned)wait_result);
#endif
            return 0;
        }
    }

    if (R_FAILED(result)) {
#ifdef DEBUG
        debug_log(
            "Switch 2 hardware decoder submit failed: 0x%x\n",
            (unsigned)result
        );
#else
        fprintf(stderr, "Switch 2 hardware decoder submit failed: 0x%x\n",
                (unsigned)result);
#endif
        return 0;
    } else {
#ifdef DEBUG
        static Uint64 hardware_submit_count = 0;
        hardware_submit_count++;
        if (hardware_submit_count == 1 ||
            hardware_submit_count % 100 == 0)
        {
            uint32_t packet_hash = 2166136261u;
            for (int i = 0; i < packet->size; i++)
                packet_hash =
                    (packet_hash ^ packet->data[i]) * 16777619u;
            debug_log(
                "Switch 2 hardware decoder submitted: packet=%llu bytes=%d pts=%lld prefix=%02x%02x%02x%02x hash=%08x\n",
                (unsigned long long)hardware_submit_count,
                packet->size,
                (long long)packet->pts,
                packet->size > 0 ? packet->data[0] : 0,
                packet->size > 1 ? packet->data[1] : 0,
                packet->size > 2 ? packet->data[2] : 0,
                packet->size > 3 ? packet->data[3] : 0,
                (unsigned)packet_hash
            );
        }
#endif
    }

    int received_video = 0;
    for (;;) {
        VideoFrame frame;
        result = videoDecoderGetFrame(
            &media->switch2_video_decoder,
            &frame,
            0
        );
        if (R_FAILED(result)) {
#ifdef DEBUG
            static Uint64 hardware_get_misses = 0;
            hardware_get_misses++;
            if (hardware_get_misses == 1 ||
                hardware_get_misses % 100 == 0)
                debug_log(
                    "Switch 2 hardware decoder get frame: miss=%llu result=0x%x\n",
                    (unsigned long long)hardware_get_misses,
                    (unsigned)result
                );
#endif
            break;
        }
#ifdef DEBUG
        static Uint32 media_frame_summary_ms = 0;
        static Uint64 hardware_frame_count = 0;
        hardware_frame_count++;
        if (!media_frame_summary_ms)
            media_frame_summary_ms = SDL_GetTicks();
        if (hardware_frame_count == 1 ||
            hardware_frame_count % 100 == 0 ||
            SDL_GetTicks() - media_frame_summary_ms >= 1000) {
            debug_log(
                "Switch 2 hardware decoder frame received: %llu %ux%u visible=%ux%u tiling=%u timestamp=%lld slot=%u\n",
                (unsigned long long)hardware_frame_count,
                frame.width,
                frame.height,
                frame.width,
                frame.height,
                (unsigned)frame.tiling,
                (long long)frame.timestamp_ns,
                frame.slot
            );
            media_frame_summary_ms = SDL_GetTicks();
        }
#endif

        uint8_t *data[4] = {0};
        int linesizes[4] = {0};
        av_image_alloc(data, linesizes,
                       frame.width, frame.height,
                       AV_PIX_FMT_YUV420P, 1);
        const uint8_t *nv12_data[2] = {0};
        int nv12_linesizes[2] = {0};
        uint8_t *copied_y = NULL;
        uint8_t *copied_uv = NULL;

        if (frame.tiling == ViddecTilingMode_PitchLinear &&
            frame.planes[0] && frame.planes[1]) {
            nv12_data[0] = frame.planes[0];
            nv12_data[1] = frame.planes[1];
            nv12_linesizes[0] = (int)frame.pitches[0];
            nv12_linesizes[1] = (int)frame.pitches[1];
        } else {
            size_t y_size = (size_t)frame.width * frame.height;
            size_t uv_size = y_size / 2;
            copied_y = malloc(y_size);
            copied_uv = malloc(uv_size);
            if (copied_y && copied_uv) {
                result = videoDecoderCopyFrame(
                    &media->switch2_video_decoder,
                    &frame,
                    copied_y,
                    frame.width,
                    copied_uv,
                    frame.width
                );
                if (R_SUCCEEDED(result)) {
                    nv12_data[0] = copied_y;
                    nv12_data[1] = copied_uv;
                    nv12_linesizes[0] = (int)frame.width;
                    nv12_linesizes[1] = (int)frame.width;
                } else {
#ifdef DEBUG
                    debug_log(
                        "Switch 2 hardware decoder copy frame failed: 0x%x %ux%u\n",
                        (unsigned)result,
                        frame.width,
                        frame.height
                    );
#endif
                }
            }
        }
        if (nv12_data[0] && nv12_data[1]) {
#ifdef DEBUG
            uint32_t y_hash = 2166136261u;
            uint32_t uv_hash = 2166136261u;
            size_t y_probe = frame.width * frame.height < 4096
                ? (size_t)frame.width * frame.height : 4096;
            size_t uv_probe = frame.width * frame.height / 2 < 2048
                ? frame.width * frame.height / 2 : 2048;
            for (size_t i = 0; i < y_probe; i++)
                y_hash = (y_hash ^ nv12_data[0][i]) * 16777619u;
            for (size_t i = 0; i < uv_probe; i++)
                uv_hash = (uv_hash ^ nv12_data[1][i]) * 16777619u;
            debug_log(
                "Switch 2 NV12 planes: y_pitch=%d uv_pitch=%d y_hash=%08x uv_hash=%08x y_ptr=%p uv_ptr=%p\n",
                nv12_linesizes[0],
                nv12_linesizes[1],
                (unsigned)y_hash,
                (unsigned)uv_hash,
                (void *)nv12_data[0],
                (void *)nv12_data[1]
            );
#endif
            sws_freeContext(media->sws);
            media->sws = sws_getContext(
                frame.width, frame.height,
                AV_PIX_FMT_NV12,
                frame.width, frame.height,
                AV_PIX_FMT_YUV420P,
                SWS_FAST_BILINEAR, NULL, NULL, NULL
            );
            sws_scale(media->sws,
                      nv12_data, nv12_linesizes,
                      0, frame.height,
                      data, linesizes);
            if (media->video_callback) {
                SdpPlayerVideoFrame output = {0};
                for (int i = 0; i < 4; i++) {
                    output.planes[i] = data[i];
                    output.linesizes[i] = linesizes[i];
                }
                output.width = frame.width;
                output.height = frame.height;
                media->video_callback(media->video_userdata, &output);
            }
            if (profile)
                profile_video_frame(profile, 0, 0);
            received_video = 1;
        }
        free(copied_y);
        free(copied_uv);
        av_freep(&data[0]);
        videoDecoderReleaseFrame(&media->switch2_video_decoder, &frame);
    }
    return received_video;
}
#endif

int media_decode_packet(Media *media, AVPacket *packet,
                        enum AVMediaType type, Profile *profile)
{
    if (type == AVMEDIA_TYPE_AUDIO) {
        if (media->audio_decoder)
            audio_queue_push(media, packet);
        return 0;
    }

#ifdef __SWITCH2__
    if (type == AVMEDIA_TYPE_VIDEO &&
        media->switch2_codec_id != AV_CODEC_ID_NONE) {
        int hardware_result = switch2_decode_packet(
            media,
            packet,
            media->switch2_codec_id,
            media->switch2_width,
            media->switch2_height,
            profile
        );
        if (hardware_result > 0)
            return hardware_result;
        if (media->switch2_video_decoder_open)
            return 0;
    }
#endif

    AVCodecContext *decoder =
        type == AVMEDIA_TYPE_VIDEO ? media->video_decoder : NULL;
    if (!decoder)
        return 0;

    if (media->video_decoder == decoder) {
        cache_h264_parameters(media, packet);
    }

    int is_idr = 0;
    if (packet->size > 4) {
        for (size_t i = 0; i + 4 < (size_t)packet->size; i++) {
            size_t start = SIZE_MAX;
            if (packet->data[i] == 0 &&
                packet->data[i + 1] == 0 &&
                packet->data[i + 2] == 1)
                start = i + 3;
            else if (i + 4 < (size_t)packet->size &&
                     packet->data[i] == 0 &&
                     packet->data[i + 1] == 0 &&
                     packet->data[i + 2] == 0 &&
                     packet->data[i + 3] == 1)
                start = i + 4;
            if (start != SIZE_MAX &&
                start < (size_t)packet->size &&
                (packet->data[start] & 0x1f) == 5) {
                is_idr = 1;
                break;
            }
        }
    }
    int error = avcodec_send_packet(decoder, packet);
    if (error < 0 && error != AVERROR(EAGAIN) && error != AVERROR(EINVAL))
        av_error("Video/audio decoder rejected packet", error);

    int received_video = 0;
    static Uint64 h264_send_packets = 0;
    static Uint64 h264_receive_frames = 0;
    if (type == AVMEDIA_TYPE_VIDEO) {
        h264_send_packets++;
        if (h264_send_packets == 1 ||
            h264_send_packets % 100 == 0 ||
            error < 0)
            debug_log(
                "H264 decoder send: packet=%llu bytes=%d result=%d NAL=%02x%02x%02x%02x\n",
                (unsigned long long)h264_send_packets,
                packet->size,
                error,
                packet->size > 0 ? packet->data[0] : 0,
                packet->size > 1 ? packet->data[1] : 0,
                packet->size > 2 ? packet->data[2] : 0,
                packet->size > 3 ? packet->data[3] : 0);
    }
    while (avcodec_receive_frame(decoder, media->frame) >= 0) {
        if (type == AVMEDIA_TYPE_VIDEO) {
            h264_receive_frames++;
            if (h264_receive_frames == 1 ||
                h264_receive_frames % 100 == 0)
                debug_log(
                    "H264 decoder receive: frame=%llu dimensions=%dx%d format=%d\n",
                    (unsigned long long)h264_receive_frames,
                    media->frame->width,
                    media->frame->height,
                    media->frame->format);
        }
        if (type == AVMEDIA_TYPE_VIDEO) {
            if (media->frame->format == AV_PIX_FMT_YUV420P ||
                media->frame->format == AV_PIX_FMT_YUVJ420P) {
                Uint64 render_start = SDL_GetPerformanceCounter();
                if (media->video_callback) {
                    SdpPlayerVideoFrame frame = {0};
                    for (int i = 0; i < 4; i++) {
                        frame.planes[i] = media->frame->data[i];
                        frame.linesizes[i] = media->frame->linesize[i];
                    }
                    frame.width = media->frame->width;
                    frame.height = media->frame->height;
                    media->video_callback(media->video_userdata, &frame);
                }
                Uint64 render_end = SDL_GetPerformanceCounter();
                if (profile)
                    profile_video_frame(
                        profile,
                        0,
                        (render_end - render_start) * 1000000 /
                            SDL_GetPerformanceFrequency()
                    );
                av_frame_unref(media->frame);
                received_video = 1;
                continue;
            }

            if (!media->sws ||
                media->sws_width != media->frame->width ||
                media->sws_height != media->frame->height ||
                media->sws_format != media->frame->format) {
                sws_freeContext(media->sws);
                media->sws = sws_getContext(
                    media->frame->width, media->frame->height,
                    media->frame->format,
                    media->frame->width, media->frame->height,
                    AV_PIX_FMT_YUV420P,
                    SWS_FAST_BILINEAR, NULL, NULL, NULL
                );
                media->sws_width = media->frame->width;
                media->sws_height = media->frame->height;
                media->sws_format = media->frame->format;
            }

            uint8_t *data[4] = {0};
            int linesizes[4] = {0};
            av_image_alloc(data, linesizes,
                           media->frame->width, media->frame->height,
                           AV_PIX_FMT_YUV420P, 1);
            sws_scale(media->sws,
                      (const uint8_t *const *)media->frame->data,
                      media->frame->linesize, 0, media->frame->height,
                      data, linesizes);
            Uint64 render_start = SDL_GetPerformanceCounter();
            if (media->video_callback) {
                SdpPlayerVideoFrame frame = {0};
                for (int i = 0; i < 4; i++) {
                    frame.planes[i] = data[i];
                    frame.linesizes[i] = linesizes[i];
                }
                frame.width = media->frame->width;
                frame.height = media->frame->height;
                media->video_callback(media->video_userdata, &frame);
            }
            Uint64 render_end = SDL_GetPerformanceCounter();
            if (profile)
                profile_video_frame(
                    profile,
                    0,
                    (render_end - render_start) * 1000000 /
                        SDL_GetPerformanceFrequency()
                );
            av_freep(&data[0]);
            received_video = 1;
        }
        av_frame_unref(media->frame);
    }
    return received_video;
}

void media_shutdown(Media *media)
{
#ifdef __SWITCH2__
    if (media->switch2_video_decoder_open) {
        videoDecoderClose(&media->switch2_video_decoder);
        media->switch2_video_decoder_open = 0;
    }
#endif
    if (media->audio_thread) {
        SDL_AtomicSet(&media->audio_stop, 1);
        SDL_WaitThread(media->audio_thread, NULL);
        media->audio_thread = NULL;
    }
    if (media->audio_queue_mutex) {
        SDL_LockMutex(media->audio_queue_mutex);
        while (media->audio_packets_head) {
            struct AudioPacketEntry *entry = media->audio_packets_head;
            media->audio_packets_head = entry->next;
            av_packet_free(&entry->packet);
            free(entry);
        }
        media->audio_packets_tail = NULL;
        media->audio_packet_count = 0;
        SDL_UnlockMutex(media->audio_queue_mutex);
        SDL_DestroyMutex(media->audio_queue_mutex);
        media->audio_queue_mutex = NULL;
    }
    if (media->audio_device)
        SDL_CloseAudioDevice(media->audio_device);
    if (audio_ring_mutex)
        SDL_DestroyMutex(audio_ring_mutex);
    swr_free(&media->swr);
    sws_freeContext(media->sws);
    av_frame_free(&media->frame);
    avcodec_free_context(&media->video_decoder);
    avcodec_free_context(&media->audio_decoder);
    free(media->h264_parameter_sets);
    free(media->h264_sps);
    free(media->h264_pps);
    media->h264_parameter_sets = NULL;
}
