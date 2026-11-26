#include "media.h"

#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <stdio.h>
#include <string.h>

#define AUDIO_RING_SIZE (64 * 1024)

typedef struct AudioRing {
    unsigned char data[AUDIO_RING_SIZE];
    size_t read_pos;
    size_t write_pos;
} AudioRing;

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
    size_t free = AUDIO_RING_SIZE - 1 - ring_available();
    if (len > free)
        len = free;
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

static void audio_callback(void *userdata, Uint8 *stream, int length)
{
    (void)userdata;
    memset(stream, 0, (size_t)length);
    if (!audio_ring_mutex)
        return;
    SDL_LockMutex(audio_ring_mutex);
    ring_read(stream, (size_t)length);
    SDL_UnlockMutex(audio_ring_mutex);
}

static void av_error(const char *operation, int error)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(error, buffer, sizeof(buffer));
    fprintf(stderr, "%s: %s\n", operation, buffer);
}

int media_init(Media *media, AVFormatContext *format,
               int video_index, int audio_index)
{
    memset(media, 0, sizeof(*media));
    media->frame = av_frame_alloc();

    if (video_index >= 0) {
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

    if (audio_index >= 0) {
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

    return media->video_decoder || media->audio_decoder ? 0 : -1;
}

int media_decode_packet(Media *media, AVPacket *packet,
                        enum AVMediaType type, Gui *gui)
{
    AVCodecContext *decoder =
        type == AVMEDIA_TYPE_VIDEO ? media->video_decoder :
        type == AVMEDIA_TYPE_AUDIO ? media->audio_decoder : NULL;
    if (!decoder)
        return 0;

    int error = avcodec_send_packet(decoder, packet);
    if (error < 0 && error != AVERROR(EAGAIN) && error != AVERROR(EINVAL))
        av_error("Video/audio decoder rejected packet", error);

    int received_video = 0;
    while (avcodec_receive_frame(decoder, media->frame) >= 0) {
        if (type == AVMEDIA_TYPE_VIDEO) {
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
            gui_render_yuv(gui, (const uint8_t **)data,
                           linesizes, media->frame->width,
                           media->frame->height);
            av_freep(&data[0]);
            received_video = 1;
        } else if (media->swr) {
            int capacity = av_rescale_rnd(
                media->frame->nb_samples, 48000,
                media->frame->sample_rate, AV_ROUND_UP
            ) + 32;
            uint8_t *output = NULL;
            av_samples_alloc(&output, NULL, 2, capacity,
                             AV_SAMPLE_FMT_S16, 1);
            int converted = swr_convert(
                media->swr, &output, capacity,
                (const uint8_t **)media->frame->extended_data,
                media->frame->nb_samples
            );
            if (converted > 0) {
                size_t bytes = (size_t)converted * 2 * sizeof(int16_t);
                SDL_LockMutex(audio_ring_mutex);
                ring_write(output, bytes);
                SDL_UnlockMutex(audio_ring_mutex);
            }
            av_freep(&output);
        }
        av_frame_unref(media->frame);
    }
    return received_video;
}

void media_shutdown(Media *media)
{
    if (media->audio_device)
        SDL_CloseAudioDevice(media->audio_device);
    if (audio_ring_mutex)
        SDL_DestroyMutex(audio_ring_mutex);
    swr_free(&media->swr);
    sws_freeContext(media->sws);
    av_frame_free(&media->frame);
    avcodec_free_context(&media->video_decoder);
    avcodec_free_context(&media->audio_decoder);
}
