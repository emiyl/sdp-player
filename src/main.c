#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <SDL.h>

#include "gui.h"
#include "media.h"
#include "network.h"
#include "profile.h"

#include <stdio.h>
#include <string.h>

static void render_video_frame(void *userdata,
                               const SdpPlayerVideoFrame *frame)
{
    Gui *gui = userdata;
    gui_render_yuv(
        gui,
        frame->planes,
        frame->linesizes,
        frame->width,
        frame->height
    );
}

int main(int argc, char **argv)
{
    const char *sdp_path = NULL;
    int process_audio = 1;
    int process_video = 1;
    const char *profile_path = NULL;

    if (argc < 2) {
        fprintf(stderr,
                "Usage: %s [--no-video] [--no-audio] [--profile <path>] <file.sdp>\n",
                argv[0]);
        return 2;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-audio") == 0)
            process_audio = 0;
        else if (strcmp(argv[i], "--no-video") == 0)
            process_video = 0;
        else if (strcmp(argv[i], "--profile") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr,
                        "Usage: %s [--no-video] [--no-audio] [--profile <path>] <file.sdp>\n",
                        argv[0]);
                return 2;
            }
            profile_path = argv[++i];
        }
        else
            sdp_path = argv[i];
    }
    if (!sdp_path) {
        fprintf(stderr,
                "Usage: %s [--no-video] [--no-audio] [--profile <path>] <file.sdp>\n",
                argv[0]);
        return 2;
    }

    Gui gui;
    memset(&gui, 0, sizeof(gui));
    if (process_video && gui_init(&gui, process_audio) != 0)
        return 1;
    if (!process_video &&
        SDL_Init(process_audio ? SDL_INIT_AUDIO | SDL_INIT_EVENTS
                               : SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    Profile profile;
    if (profile_init(&profile, profile_path) != 0) {
        fprintf(stderr, "Could not create performance profile log\n");
        return 1;
    }

    Network network;
    if (network_open(&network, sdp_path) != 0) {
        gui_shutdown(&gui);
        return 1;
    }
    if (!process_audio)
        network_disable_audio(&network);
    if (!process_video)
        network_disable_video(&network);

    Media media;
    media.profile = &profile;
    if (media_init(&media, network.format,
                   network.video_index,
                   process_audio ? network.audio_index : -1,
                   process_audio, process_video) != 0)
        fprintf(stderr, "SDP contains no supported video or audio stream\n");
    if (process_video) {
        media.video_callback = render_video_frame;
        media.video_userdata = &gui;
    }
    if (network_start(&network) != 0) {
        media_shutdown(&media);
        network_close(&network);
        gui_shutdown(&gui);
        return 1;
    }

    AVPacket *packet = av_packet_alloc();
    int running = 1;
    int video_received = 0;
    int stream_was_received = 0;
    int displayed_status = 0;
    Uint32 last_packet_ms = SDL_GetTicks();

    while (running) {
        if (process_video) {
            int gui_event = gui_poll_quit(&gui, !video_received);
            if (gui_event == 1) {
                running = 0;
                break;
            }
            if (gui_event == -1) {
                if (stream_was_received) {
                    gui_draw_status(
                        &gui,
                        displayed_status == 2 ? "Connection lost" :
                        displayed_status == 1 ? "Reconnecting..." :
                        "Waiting for video stream..."
                    );
                } else {
                    gui_draw_waiting(&gui);
                }
            }
        }

        if (process_video &&
            network_pop_packet(&network, packet)) {
            last_packet_ms = SDL_GetTicks();
            profile_packet(
                &profile,
                packet->stream_index == network.video_index,
                (size_t)packet->size
            );
            if (stream_was_received && !video_received)
                gui_draw_status(&gui, "Reconnecting...");

            enum AVMediaType type = AVMEDIA_TYPE_UNKNOWN;
            if (packet->stream_index == network.video_index)
                type = AVMEDIA_TYPE_VIDEO;
            else if (packet->stream_index == network.audio_index)
                type = AVMEDIA_TYPE_AUDIO;

            Uint64 decode_start = SDL_GetPerformanceCounter();
            if (media_decode_packet(&media, packet, type, &profile)) {
                video_received = 1;
                stream_was_received = 1;
                displayed_status = 0;
                last_packet_ms = SDL_GetTicks();
            }
            Uint64 decode_end = SDL_GetPerformanceCounter();
            Uint64 decode_us =
                (decode_end - decode_start) * 1000000 /
                SDL_GetPerformanceFrequency();
            profile_decode_time(&profile, decode_us);
        } else if (network_pop_packet(&network, packet)) {
            profile_packet(
                &profile,
                packet->stream_index == network.video_index,
                (size_t)packet->size
            );
            enum AVMediaType type = AVMEDIA_TYPE_UNKNOWN;
            if (packet->stream_index == network.audio_index)
                type = AVMEDIA_TYPE_AUDIO;
            media_decode_packet(&media, packet, type, &profile);
        }

        if (process_video) {
            Uint32 silent_ms = SDL_GetTicks() - last_packet_ms;
            if (!stream_was_received) {
                if (displayed_status != 0) {
                    displayed_status = 0;
                    video_received = 0;
                }
                gui_draw_waiting(&gui);
            } else if (silent_ms >= 5000) {
                if (displayed_status != 2) {
                    video_received = 0;
                    displayed_status = 2;
                    gui_draw_status(&gui, "Connection lost");
                }
            } else if (silent_ms >= 2000) {
                if (displayed_status != 1) {
                    video_received = 0;
                    displayed_status = 1;
                    gui_draw_status(&gui, "Reconnecting...");
                }
            } else if (video_received && displayed_status != 0) {
                displayed_status = 0;
            }
        }

        profile_sample(
            &profile,
            network_queue_depth(&network),
            network_dropped_packets(&network),
            profile.read_errors
        );
        SDL_Delay(1);
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    network_stop(&network);
    media_shutdown(&media);
    network_close(&network);
    profile_finish(&profile);
    if (process_video)
        gui_shutdown(&gui);
    else
        SDL_Quit();
    return 0;
}
