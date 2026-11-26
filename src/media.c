#include "media.h"

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
    if (media->audio_packet_count >= 64) {
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

        if (avcodec_send_packet(media->audio_decoder, entry->packet) >= 0) {
            while (avcodec_receive_frame(media->audio_decoder, frame) >= 0) {
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
                        SDL_LockMutex(audio_ring_mutex);
                        ring_write(output, bytes);
                        SDL_UnlockMutex(audio_ring_mutex);
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

static void cache_h264_parameters(Media *media, AVPacket *packet)
{
    int found_sps = 0;
    int found_pps = 0;
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
            found_sps = 1;
            free(media->h264_sps);
            media->h264_sps_size = next - i;
            media->h264_sps = malloc(media->h264_sps_size);
            if (media->h264_sps)
                memcpy(media->h264_sps, packet->data + i,
                       media->h264_sps_size);
        } else if (type == 8) {
            found_pps = 1;
            free(media->h264_pps);
            media->h264_pps_size = next - i;
            media->h264_pps = malloc(media->h264_pps_size);
            if (media->h264_pps)
                memcpy(media->h264_pps, packet->data + i,
                       media->h264_pps_size);
        }
        i = next;
    }

    if (!found_sps || !found_pps ||
        !media->h264_sps || !media->h264_pps)
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
    if (!media->h264_ready) {
        media->h264_ready = 1;
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
        const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
        if (codec) {
            media->video_decoder = avcodec_alloc_context3(codec);
            avcodec_parameters_to_context(media->video_decoder, parameters);
            media->video_decoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
            media->video_decoder->thread_count = 1;
            media->video_decoder->thread_type = 0;
            if (avcodec_open2(media->video_decoder, codec, NULL) < 0) {
                avcodec_free_context(&media->video_decoder);
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
            wanted.callback = audio_callback;
            audio_ring_mutex = SDL_CreateMutex();
            media->audio_device =
                SDL_OpenAudioDevice(NULL, 0, &wanted, &obtained, 0);
            if (media->audio_device)
                SDL_PauseAudioDevice(media->audio_device, 0);

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

    return media->video_decoder || media->audio_decoder ? 0 : -1;
}

int media_decode_packet(Media *media, AVPacket *packet,
                        enum AVMediaType type, Gui *gui, Profile *profile)
{
    if (type == AVMEDIA_TYPE_AUDIO) {
        if (media->audio_decoder)
            audio_queue_push(media, packet);
        return 0;
    }

    AVCodecContext *decoder =
        type == AVMEDIA_TYPE_VIDEO ? media->video_decoder : NULL;
    if (!decoder)
        return 0;

    if (media->video_decoder == decoder) {
        cache_h264_parameters(media, packet);
        if (!media->h264_ready)
            return 0;
    }

    int error = avcodec_send_packet(decoder, packet);
    if (error < 0 && error != AVERROR(EAGAIN) && error != AVERROR(EINVAL))
        av_error("Video/audio decoder rejected packet", error);

    int received_video = 0;
    while (avcodec_receive_frame(decoder, media->frame) >= 0) {
        if (type == AVMEDIA_TYPE_VIDEO) {
            if (media->frame->format == AV_PIX_FMT_YUV420P ||
                media->frame->format == AV_PIX_FMT_YUVJ420P) {
                Uint64 render_start = SDL_GetPerformanceCounter();
                gui_render_yuv(
                    gui,
                    (const uint8_t **)media->frame->data,
                    media->frame->linesize,
                    media->frame->width,
                    media->frame->height
                );
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
            gui_render_yuv(gui, (const uint8_t **)data,
                           linesizes, media->frame->width,
                           media->frame->height);
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
